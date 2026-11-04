#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Linux / SteamOS helper for Pokopia Recomp: does what the Windows launcher's "Set up" and "Play" do, without the launcher.

The game itself stays a Windows program (pkp.exe + pkp_rt.dll) and runs under Wine or Proton. Everything before that runs
natively here: the dump check, the game build (the release's own tools/build/pkp_build.py, with a Linux build of the same
Zig 0.17.0 it bundles) and the shader extraction. Only the shader translator (tools/spirv_harness, a .NET program) runs
under Wine, unless a natively built one is given with --harness.

    python3 linux/pkp_linux.py check  --dump <dump folder>
    python3 linux/pkp_linux.py setup  --dump <dump folder> [--game ~/Games/PokopiaRecomp] [--jobs N] [--harness <spirv_harness.dll>]
    python3 linux/pkp_linux.py play   [--game ...] [--offline]
    python3 linux/pkp_linux.py status [--game ...]

The dump folder is either the launcher layout (base/ and updates/<version>/, each with exefs/ and romfs/; the highest
complete update wins) or one folder that holds exefs/ and romfs/ directly.

The game folder ends up as the release expects an installed game: exefs/ and romfs/ next to pkp.exe (symlinks to your
dump), pkp.ini with relative paths, saves in ./save. Relative paths are what make the folder portable: copy it with
`rsync -aL` (follows the symlinks) to a Steam Deck and it runs there under Proton unchanged.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_GAME = os.path.expanduser('~/Games/PokopiaRecomp')
DEFAULT_PREFIX = os.path.expanduser('~/.local/share/pokopia-recomp/wine')
PROTON_DATA = os.path.expanduser('~/.local/share/pokopia-recomp/proton')   # the warm-up's Proton prefix (STEAM_COMPAT_DATA_PATH)
STEAM_ROOTS = ('~/.local/share/Steam', '~/.steam/steam', '~/.steam/root', '~/.var/app/com.valvesoftware.Steam/.local/share/Steam')
WARMUP_SECONDS = 60                  # the opening asks for its run-time shaders in its first ~25 s
VENV_PY = os.path.join(REPO, 'tools', '.venv', 'bin', 'python')
EXEFS_FILES = ('main', 'sdk', 'subsdk0', 'rtld', 'main.npdm')
NSO_FILES = ('main', 'sdk', 'subsdk0', 'rtld')
SPIRV_REL = os.path.join('shader_cache', 'spirv')
MISSING_REL = 'shader_cache/missing'      # [nvn] dump_missing: shaders the game built at run time that the cache lacks
CHUNK = 5000                       # blocks per translator run (the launcher uses 1000; at ~115/s a run is ~45 s, so fewer restarts)

ZIG_VERSION = '0.17.0'
ZIG_URL = 'https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz'
ZIG_SHA256 = '1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026'


class Fail(Exception):
    pass


def say(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------------------------------------------------------- dump

def find_dump(dump):
    """(exefs, romfs, label) of the newest complete candidate, as the launcher's dump.rs picks it."""
    dump = os.path.abspath(os.path.expanduser(dump))
    if not os.path.isdir(dump):
        raise Fail('%s is not a folder.' % dump)

    def complete(d):
        return all(os.path.isfile(os.path.join(d, 'exefs', f)) for f in EXEFS_FILES) and os.path.isdir(os.path.join(d, 'romfs'))

    if complete(dump):
        return os.path.join(dump, 'exefs'), os.path.join(dump, 'romfs'), dump
    cands = []
    upd = os.path.join(dump, 'updates')
    if os.path.isdir(upd):
        vs = [v for v in os.listdir(upd) if os.path.isdir(os.path.join(upd, v))]
        vs.sort(key=lambda v: (0, -int(v)) if v.isdigit() else (1, v))
        cands += [os.path.join(upd, v) for v in vs]
    cands.append(os.path.join(dump, 'base'))
    for c in cands:
        if complete(c):
            return os.path.join(c, 'exefs'), os.path.join(c, 'romfs'), c
    raise Fail('No usable game files in %s. Each of base/ or updates/<version>/ needs exefs/ (%s) and romfs/.'
               % (dump, ', '.join(EXEFS_FILES)))


def nso_integrity(path):
    """None when the NSO's three segments decompress and match the SHA-256 in its own header, else what is wrong."""
    import lz4.block
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        return 'not a decrypted NSO (no NSO0 header)'
    flags = struct.unpack_from('<I', d, 12)[0]
    end = 0
    bad = []
    for i, (ho, cso) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
        foff, _, size = struct.unpack_from('<III', d, ho)
        csz = struct.unpack_from('<I', d, cso)[0]
        stored = csz if flags & (1 << i) else size
        end = max(end, foff + stored)
        raw = d[foff:foff + stored]
        try:
            if flags & (1 << i):
                raw = lz4.block.decompress(raw, uncompressed_size=size)
            if hashlib.sha256(raw).digest() != d[0xA0 + i * 0x20:0xC0 + i * 0x20]:
                bad.append(i)
        except Exception:                # noqa: BLE001 - a corrupt LZ4 stream
            bad.append(i)
    if not bad:
        return None
    missing = end - len(d)
    crlf, cr, lf = d.count(b'\r\n'), d.count(b'\r'), d.count(b'\n')
    expected_crlf = cr * lf / max(1, len(d))
    if missing > 0 and expected_crlf > 20 and crlf < expected_crlf * 0.1 and abs(missing - expected_crlf) < expected_crlf * 0.25:
        return ('damaged by a text-mode copy: %d bytes short and almost no CR LF byte pairs left (%d, ~%d expected). '
                'Every CR LF was turned into LF, which is what FTP in ASCII mode (or a line-ending converter) does. '
                'Copy the file again in binary mode.' % (missing, crlf, expected_crlf))
    if missing > 0:
        return 'truncated or damaged (%d bytes shorter than its header says). Copy or dump it again.' % missing
    return 'damaged (segments %s do not match the hashes in its header). Copy or dump it again.' % bad


def check_dump(game, exefs, quiet=False):
    """Integrity of every module, then the supported version (dump_hashes.json of the release). Raises Fail with every problem."""
    problems = []
    for n in NSO_FILES:
        p = os.path.join(exefs, n)
        err = nso_integrity(p)
        if not quiet:
            say('  %-9s %s' % (n, 'intact' if err is None else 'PROBLEM'))
        if err:
            problems.append('exefs/%s: %s' % (n, err))
    if problems:
        raise Fail('Your exefs files are damaged, so nothing can be built from them:\n  ' + '\n  '.join(problems))
    hashes = os.path.join(game, 'tools', 'build', 'dump_hashes.json')
    if not os.path.isfile(hashes):
        hashes = os.path.join(REPO, 'game-build', 'release_b', 'dump_hashes.json')
    want = json.load(open(hashes, encoding='utf-8'))
    bid = open(os.path.join(exefs, 'main'), 'rb').read(0x60)[0x40:0x54].hex()
    if bid != want['main_build_id']:
        raise Fail('This is a different version of the game (main build ID %s). The release supports %s (build ID %s).'
                   % (bid, want['version'], want['main_build_id']))
    for n, h in want['sha256'].items():
        got = hashlib.sha256(open(os.path.join(exefs, n), 'rb').read()).hexdigest()
        if got != h:
            raise Fail('exefs/%s is intact but is not the file the release supports (sha256 %s). Expected %s.' % (n, got, want['version']))
    if not quiet:
        say('  every module matches %s' % want['version'])


# ----------------------------------------------------------------------------------------------------------- game folder

def ini_get(path, section, key):
    """The value of [section] key in an ini file, or None."""
    if not os.path.isfile(path):
        return None
    sec = None
    for line in open(path, encoding='utf-8').read().splitlines():
        t = line.strip()
        if t.startswith('[') and ']' in t:
            sec = t[1:t.index(']')].strip().lower()
        elif sec == section and '=' in t and not t.startswith((';', '#')) and t.split('=', 1)[0].strip().lower() == key:
            return t.split('=', 1)[1].strip()
    return None


def ini_set(path, edits):
    """Sets [section] key = value lines, keeping every other line as it is (like the launcher's ini.rs)."""
    lines = open(path, encoding='utf-8').read().splitlines() if os.path.isfile(path) else []
    for section, key, value in edits:
        sec, idx, sec_end = None, None, None
        for i, line in enumerate(lines):
            t = line.strip()
            if t.startswith('[') and ']' in t:
                if sec == section and sec_end is None:
                    sec_end = i
                sec = t[1:t.index(']')].strip().lower()
                continue
            if sec == section and '=' in t and not t.startswith((';', '#')) and t.split('=', 1)[0].strip().lower() == key:
                idx = i
        if idx is not None:
            lines[idx] = '%s = %s' % (key, value)
        elif any(l.strip().lower() == '[%s]' % section for l in lines):
            if sec_end is None:
                sec_end = len(lines)
            while sec_end > 0 and not lines[sec_end - 1].strip():
                sec_end -= 1
            lines.insert(sec_end, '%s = %s' % (key, value))
        else:
            if lines and lines[-1].strip():
                lines.append('')
            lines += ['[%s]' % section, '%s = %s' % (key, value)]
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(tmp, path)


def link_dir(link, target):
    if os.path.islink(link):
        os.remove(link)
    elif os.path.exists(link):
        if os.path.realpath(link) == os.path.realpath(target):
            return
        raise Fail('%s already exists and is not a link; move it away or point --dump at it.' % link)
    os.symlink(target, link)


def fs_dirs(game):
    """The [fs] exefs and romfs pkp.ini names, as host paths (relative ones are relative to the game folder; Z: is /)."""
    ini = os.path.join(game, 'pkp.ini')
    out = []
    for key in ('exefs', 'romfs'):
        v = (ini_get(ini, 'fs', key) or key).strip()
        if v[:2] in ('Z:', 'z:'):
            v = v[2:]
        out.append(os.path.realpath(v if os.path.isabs(v) else os.path.join(game, v)))
    return out


def prepare_game_folder(game, exefs, romfs):
    if not os.path.isfile(os.path.join(game, 'pkp_rt.dll')):
        raise Fail('%s does not look like an extracted release (no pkp_rt.dll). See linux/README.md.' % game)
    inside = [os.path.relpath(d, game) for d in (exefs, romfs) if os.path.realpath(d).startswith(os.path.realpath(game) + os.sep)]
    if len(inside) == 2:                 # a dump inside the game folder: relative paths, so the folder can be copied as is
        ex_ini, rom_ini = inside
    else:                                # a dump elsewhere: links, so `rsync -aL` takes the files along to a Steam Deck
        link_dir(os.path.join(game, 'exefs'), exefs)
        link_dir(os.path.join(game, 'romfs'), romfs)
        ex_ini, rom_ini = 'exefs', 'romfs'
    os.makedirs(os.path.join(game, 'save'), exist_ok=True)
    # dump_missing: the game builds some shaders while it runs (they are in no file the extractor reads); the runtime saves
    # each one it has no translation for there, in a layout the translator reads, and `refresh` adds them to the cache.
    ini = os.path.join(game, 'pkp.ini')
    ini_set(ini, [('fs', 'exefs', ex_ini), ('fs', 'romfs', rom_ini), ('fs', 'save_dir', 'save'),
                  ('nvn', 'shader_cache', 'shader_cache/spirv'), ('nvn', 'dump_missing', MISSING_REL)])
    # pool_mirror off under Wine/Proton: the mirror tracks the game's writes by page protection, and every write fault is a
    # SIGSEGV that Wine turns into a Windows exception (~0.4 ms each here, ~200 per frame in places). Measured on an RTX 4070:
    # gameplay 17-98 fps with 1% lows of 8-15 -> a steady 162-165 fps with 1% lows of 115-157. Only set when the player hasn't.
    if ini_get(ini, 'nvn', 'pool_mirror') is None:
        ini_set(ini, [('nvn', 'pool_mirror', 'off')])


def venv_python():
    if not os.path.isfile(VENV_PY):
        raise Fail('No Python environment at %s. Create it once:\n  python3 -m venv tools/.venv && tools/.venv/bin/pip install lz4 capstone==5.0.9'
                   % os.path.dirname(os.path.dirname(VENV_PY)))
    r = subprocess.run([VENV_PY, '-I', '-c', 'import lz4.block, capstone'], capture_output=True)
    if r.returncode:
        raise Fail('%s lacks lz4 or capstone: tools/.venv/bin/pip install lz4 capstone==5.0.9' % VENV_PY)
    return VENV_PY


def zig_path(game):
    z = os.path.join(game, 'tools', 'build', 'zig-linux', 'zig')
    if os.path.isfile(z):
        return z
    raise Fail('No Linux Zig at %s. Get the same version the release bundles (%s), checked against ziglang.org\'s sha256:\n'
               '  curl -LO %s\n  echo "%s  %s" | sha256sum -c\n  mkdir -p %s && tar -xJf %s -C %s --strip-components=1'
               % (z, ZIG_VERSION, ZIG_URL, ZIG_SHA256, os.path.basename(ZIG_URL), os.path.dirname(z), os.path.basename(ZIG_URL), os.path.dirname(z)))


# ------------------------------------------------------------------------------------------------------------ game build

def run_progress(cmd, env=None, cwd=None):
    """Runs a launcher-protocol tool (@progress / @error / @cancelled lines) and prints its progress in place."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, cwd=cwd, text=True,
                         errors='replace')
    err, tail, last = None, [], ''
    for line in p.stdout:
        line = line.rstrip('\n')
        if line.startswith('@progress '):
            parts = line.split(' ', 4)
            text = parts[4] if len(parts) > 4 else ''
            if text != last:
                print('\r\033[K  ' + text[:150], end='', flush=True)
                last = text
        elif line.startswith('@error '):
            err = line[7:].replace('\\n', '\n')
        elif line.startswith('@cancelled'):
            err = 'paused'
        else:
            tail = (tail + [line])[-20:]
    rc = p.wait()
    print(flush=True)
    if rc:
        raise Fail(err or 'exit %d:\n%s' % (rc, '\n'.join(tail)))


def build_game(game, exefs, jobs):
    if os.path.isfile(os.path.join(game, 'pkp.exe')) and os.path.isfile(os.path.join(game, 'build', 'done.json')):
        say('pkp.exe is already built.')
        return
    py, zig = venv_python(), zig_path(game)
    script = os.path.join(game, 'tools', 'build', 'pkp_build.py')
    say('Building pkp.exe from your dump (lift + compile + link, several minutes)...')
    t = time.time()
    run_progress([py, '-I', os.path.abspath(__file__), '_build', '--script', script, '--zig', zig,
                  '--exefs', exefs, '--game', game, '--jobs', str(jobs)])
    say('pkp.exe built in %d s.' % (time.time() - t))


def _build_inprocess(a):
    """The release's pkp_build.py, unchanged, with two Windows-only details swapped: Zig is the Linux build of the same
    version, and child processes start without the Windows creation flags (the process runs at lower priority instead)."""
    import importlib.util
    os.nice(10)
    spec = importlib.util.spec_from_file_location('pkp_build', a.script)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    m.ZIG = a.zig
    m.popen = lambda cmd, **kw: subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)
    sys.argv = [a.script, '--exefs', a.exefs, '--game', a.game, '--jobs', str(a.jobs)]
    return m.main()


# --------------------------------------------------------------------------------------------------------------- shaders

def winpath(p):
    return 'Z:' + os.path.abspath(p).replace('/', '\\')


def wine_env(prefix):
    env = dict(os.environ, WINEPREFIX=prefix, WINEDEBUG='-all')
    # No Gecko install prompt in a fresh prefix. mscoree must stay enabled: Wine loads it for every IL-only .NET assembly
    # (_CorValidateImage), so disabling it makes the shader translator fail with "Could not load ... System.Runtime.dll".
    env.setdefault('WINEDLLOVERRIDES', 'mshtml=')
    return env


def harness_cmd(game, harness, prefix, corpus, stage, jobs):
    # Server GC: the translator allocates heavily on every worker, and with the default (workstation) GC each collection
    # stops all of them; under Wine that suspension is slow enough to leave ~2 of 15 workers busy. Measured on 600 blocks
    # with 15 jobs: 29.5 -> 115 blocks/s, byte-identical .spv and results.
    tail = ['--jobs', str(jobs), '--no-val']
    native = os.path.join(game, 'tools', 'spirv_harness_native', 'spirv_harness.dll')
    if not harness and os.path.isfile(native) and shutil.which('dotnet'):
        harness = native                                      # built from source on this machine: no Wine needed
    if harness:                                               # natively built: dotnet spirv_harness.dll, or a native apphost
        h = os.path.abspath(os.path.expanduser(harness))
        base = (['dotnet', h] if h.endswith('.dll') else [h])
        return base + ['--batch', corpus, '--out', stage] + tail, dict(os.environ, DOTNET_gcServer='1')
    exe = os.path.join(game, 'tools', 'spirv_harness', 'spirv_harness.exe')
    if not shutil.which('wine'):
        raise Fail('Wine is needed to run %s (or pass --harness with a natively built translator).' % exe)
    return ['wine', exe, '--batch', winpath(corpus), '--out', winpath(stage)] + tail, dict(wine_env(prefix), DOTNET_gcServer='1')


def read_tsv(p):
    lines = [l for l in open(p, encoding='utf-8').read().splitlines()]
    return (lines[0] if lines else ''), [l for l in lines[1:] if l]


def marker_path(game):
    return os.path.join(game, 'shader_cache', 'setup.json')


def read_marker(game):
    try:
        return dict({'romfs': '', 'extracted': False, 'blocks': 0, 'complete': False}, **json.load(open(marker_path(game))))
    except (OSError, ValueError):
        return {'romfs': '', 'extracted': False, 'blocks': 0, 'complete': False}


def write_marker(game, m):
    json.dump(m, open(marker_path(game), 'w'), indent=2)


def shaders_complete(game):
    return read_marker(game)['complete'] and os.path.isfile(os.path.join(game, SPIRV_REL, 'results.tsv'))


def run_harness(cmd, env):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, errors='replace', preexec_fn=lambda: os.nice(10))
    if r.returncode:
        raise Fail('The shader translator failed (exit %d):\n%s' % (r.returncode, (r.stderr or r.stdout)[-3000:]))


def commit_stage(stage, spirv, skip=frozenset()):
    """Moves a translator run's .spv files into the cache, then appends their results.tsv rows (the rows are what make them
    count, as in the launcher's shaders.rs). Rows whose hash is in `skip` are left out. Returns the committed rows."""
    rheader, rrows = read_tsv(os.path.join(stage, 'results.tsv'))
    rrows = [r for r in rrows if r.split('\t', 1)[0] not in skip]
    for row in rrows:
        h = row.split('\t', 1)[0]
        src = os.path.join(stage, h + '.spv')
        if os.path.isfile(src):
            os.replace(src, os.path.join(spirv, h + '.spv'))
    results = os.path.join(spirv, 'results.tsv')
    new = not os.path.isfile(results)
    with open(results, 'a', encoding='utf-8', newline='') as f:
        f.write((rheader + '\n' if new else '') + ''.join(row + '\n' for row in rrows))
    return rrows


def missing_count(game):
    m = os.path.join(game, MISSING_REL, 'manifest.tsv')
    return len({l.split('\t')[10] for l in read_tsv(m)[1] if l.count('\t') >= 10}) if os.path.isfile(m) else 0


RUNTIME_REL = os.path.join('shader_cache', 'runtime')   # every run-time block refresh has seen (manifest layout), for re-translation


def archive_runtime_blocks(game):
    """Moves the blocks in shader_cache/missing into shader_cache/runtime (one manifest, one row per hash) and empties missing."""
    missing, arch = os.path.join(game, MISSING_REL), os.path.join(game, RUNTIME_REL)
    os.makedirs(arch, exist_ok=True)
    am = os.path.join(arch, 'manifest.tsv')
    header, rows = read_tsv(os.path.join(missing, 'manifest.tsv'))
    have = {r.split('\t')[10] for r in read_tsv(am)[1]} if os.path.isfile(am) else set()
    new = []
    for r in rows:
        f = r.split('\t')
        if len(f) < 13 or f[10] in have:
            continue
        for name in (f[11], f[12]):
            if os.path.isfile(os.path.join(missing, name)):
                os.replace(os.path.join(missing, name), os.path.join(arch, name))
        have.add(f[10])
        new.append(r)
    fresh = not os.path.isfile(am) or os.path.getsize(am) == 0
    with open(am, 'a', encoding='utf-8', newline='') as out:
        out.write((header + '\n' if fresh else '') + ''.join(r + '\n' for r in new))
    shutil.rmtree(missing, ignore_errors=True)


def refresh_shaders(game, jobs, harness, prefix):
    """Translates the shaders the game saved to shader_cache/missing while it ran ([nvn] dump_missing) and adds them."""
    missing, spirv = os.path.join(game, MISSING_REL), os.path.join(game, SPIRV_REL)
    if not missing_count(game):
        say('No missing shaders recorded yet. Play for a while (the game saves the ones it lacks to %s), then run refresh again.' % MISSING_REL)
        return 0
    results = os.path.join(spirv, 'results.tsv')
    have = {r.split('\t', 1)[0] for r in read_tsv(results)[1]} if os.path.isfile(results) else set()
    stage = os.path.join(game, 'shader_cache', 'stage_missing')
    shutil.rmtree(stage, ignore_errors=True)
    say('Translating %d shader(s) the game built while it ran...' % missing_count(game))
    cmd, env = harness_cmd(game, harness, prefix, missing, stage, jobs)
    run_harness(cmd, env)
    rows = commit_stage(stage, spirv, skip=have)
    shutil.rmtree(stage, ignore_errors=True)
    archive_runtime_blocks(game)                     # kept, so a newer translator can redo them; the game records new ones again
    status = {}
    for r in rows:
        s = r.split('\t')[4] if r.count('\t') >= 4 else '?'
        status[s] = status.get(s, 0) + 1
    say('Added %d shader(s) to the cache (%s). They are used from the next start of the game.'
        % (len(rows), ', '.join('%d %s' % (n, s) for s, n in sorted(status.items())) or 'none new'))
    return len(rows)


def build_shaders(game, exefs, romfs, jobs, harness, prefix):
    """Same steps and files as the launcher's shaders.rs, so either one can continue the other's work."""
    if shaders_complete(game):
        say('Shader cache is already complete.')
        return
    py = venv_python()
    cache = os.path.join(game, 'shader_cache')
    corpus, stage, spirv = os.path.join(cache, 'corpus'), os.path.join(cache, 'stage'), os.path.join(game, SPIRV_REL)
    os.makedirs(spirv, exist_ok=True)
    romfs_key = 'romfs'
    m = read_marker(game)
    if m['romfs'] != romfs_key:
        m = {'romfs': romfs_key, 'extracted': False, 'blocks': 0, 'complete': False}
    m['complete'] = False
    write_marker(game, m)
    all_tsv = os.path.join(corpus, 'unique_all.tsv')
    if not (m['extracted'] and os.path.isfile(all_tsv)):
        say('Finding the game\'s shaders in your romfs and main...')
        if os.path.exists(os.path.join(corpus, 'unique.tsv')):
            os.remove(os.path.join(corpus, 'unique.tsv'))
        cmd = [py, '-I', os.path.join(game, 'tools', 'shaders', 'extract_corpus.py'), '--romfs', romfs,
               '--main', os.path.join(exefs, 'main'), '--out', corpus, '--jobs', str(jobs), '--progress']
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                             preexec_fn=lambda: os.nice(10))
        tail = []
        for line in p.stdout:
            if line.startswith('scan: '):
                print('\r\033[K  scanning ' + line[6:].strip(), end='', flush=True)
            else:
                tail = (tail + [line.rstrip()])[-20:]
        print()
        if p.wait():
            raise Fail('Shader extraction failed:\n' + '\n'.join(tail))
        os.replace(os.path.join(corpus, 'unique.tsv'), all_tsv)
        m['extracted'] = True
        write_marker(game, m)

    uheader, urows = read_tsv(all_tsv)
    m['blocks'] = len(urows)
    write_marker(game, m)
    results = os.path.join(spirv, 'results.tsv')
    done = {r.split('\t', 1)[0] for r in read_tsv(results)[1]} if os.path.isfile(results) else set()
    todo = [r for r in urows if r.split('\t', 1)[0] not in done]
    total, finished = len(urows), len(urows) - len(todo)
    say('Translating %d shaders (%d already done). This is the long step; Ctrl+C pauses, run setup again to continue.' % (total, finished))
    t0 = time.time()
    for k in range(0, len(todo), CHUNK):
        chunk = todo[k:k + CHUNK]
        with open(os.path.join(corpus, 'unique.tsv'), 'w', encoding='utf-8') as f:
            f.write(uheader + '\n' + ''.join(r + '\n' for r in chunk))
        shutil.rmtree(stage, ignore_errors=True)
        cmd, env = harness_cmd(game, harness, prefix, corpus, stage, jobs)
        run_harness(cmd, env)
        commit_stage(stage, spirv)
        finished += len(chunk)
        rate = (finished - (total - len(todo))) / max(1e-6, time.time() - t0)
        eta = (total - finished) / rate if rate else 0
        print('\r\033[K  %d of %d shaders  (~%d min left)' % (finished, total, eta / 60), end='', flush=True)
    print()
    shutil.rmtree(stage, ignore_errors=True)
    say('Cleaning up the extracted shader corpus...')
    shutil.rmtree(corpus, ignore_errors=True)
    m['extracted'], m['complete'] = False, True
    write_marker(game, m)
    say('Shader cache complete.')


# ---------------------------------------------------------------------------------------------------------------- play

def play(game, prefix, offline, extra):
    exe = os.path.join(game, 'pkp.exe')
    if not os.path.isfile(exe):
        raise Fail('pkp.exe is not built yet: run setup first.')
    if not shaders_complete(game):
        raise Fail('The shader cache is not finished: run setup again to continue it.')
    if not shutil.which('wine'):
        raise Fail('Wine is not installed. Install it (Fedora: sudo dnf install wine), or add pkp.exe to Steam and use Proton.')
    os.makedirs(prefix, exist_ok=True)
    cmd = ['wine', exe, '--log', 'pkp.log'] + extra
    if offline:                       # no network at all: a new, empty network namespace (the game never needs one)
        cmd = ['unshare', '--user', '--map-current-user', '--net', '--'] + cmd
    say('Starting pkp.exe under Wine (prefix %s)%s. Log: %s' % (prefix, ', offline' if offline else '', os.path.join(game, 'pkp.log')))
    os.chdir(game)
    os.execvpe(cmd[0], cmd, wine_env(prefix))


# ---------------------------------------------------------------------------------------------------------------- main

# ---------------------------------------------------------------------------------------------------------------- warm-up

def steam_libraries():
    """(Steam root, [library folders]) from libraryfolders.vdf; (None, []) without Steam."""
    for r in STEAM_ROOTS:
        root = os.path.realpath(os.path.expanduser(r))
        vdf = os.path.join(root, 'steamapps', 'libraryfolders.vdf')
        if os.path.isfile(vdf):
            libs = [root] + re.findall(r'"path"\s+"([^"]+)"', open(vdf, encoding='utf-8', errors='replace').read())
            seen, out = set(), []
            for lib in libs:
                lib = os.path.realpath(lib.replace('\\\\', '\\'))
                if lib not in seen and os.path.isdir(os.path.join(lib, 'steamapps')):
                    seen.add(lib)
                    out.append(lib)
            return root, out
    return None, []


def find_proton():
    """[command prefix], env, label for Steam's Proton (Experimental first, else the newest Proton N.N), inside the Steam Linux Runtime
    container its toolmanifest.vdf asks for when that is installed (as Steam starts it). None without Steam or Proton."""
    root, libs = steam_libraries()
    if not root:
        return None
    found = []
    for lib in libs:
        common = os.path.join(lib, 'steamapps', 'common')
        for name in (os.listdir(common) if os.path.isdir(common) else []):
            if name.startswith('Proton') and os.path.isfile(os.path.join(common, name, 'proton')):
                found.append((name, os.path.join(common, name)))
    if not found:
        return None
    def rank(item):
        name = item[0]
        if 'Experimental' in name:
            return (2, ())
        nums = tuple(int(x) for x in re.findall(r'\d+', name))
        return (1, nums) if nums else (0, ())
    name, pdir = max(found, key=rank)
    cmd = [os.path.join(pdir, 'proton'), 'waitforexitandrun']
    label = name
    manifest = os.path.join(pdir, 'toolmanifest.vdf')
    tool = re.search(r'"require_tool_appid"\s+"(\d+)"', open(manifest, encoding='utf-8', errors='replace').read()) if os.path.isfile(manifest) else None
    if tool:
        for lib in libs:
            acf = os.path.join(lib, 'steamapps', 'appmanifest_%s.acf' % tool.group(1))
            m = re.search(r'"installdir"\s+"([^"]+)"', open(acf, encoding='utf-8', errors='replace').read()) if os.path.isfile(acf) else None
            entry = os.path.join(lib, 'steamapps', 'common', m.group(1), '_v2-entry-point') if m else ''
            if entry and os.path.isfile(entry):
                cmd = [entry, '--verb=waitforexitandrun', '--'] + cmd
                label += ' in ' + m.group(1)
                break
    os.makedirs(PROTON_DATA, exist_ok=True)
    env = dict(os.environ, STEAM_COMPAT_DATA_PATH=PROTON_DATA, STEAM_COMPAT_CLIENT_INSTALL_PATH=root)
    return cmd, env, label


def runtime_collected(game):
    """Whether run-time shaders were collected already (shader_cache/runtime holds what refresh translated)."""
    return len(read_tsv(os.path.join(game, RUNTIME_REL, 'manifest.tsv'))[1]) > 0 if os.path.isfile(os.path.join(game, RUNTIME_REL, 'manifest.tsv')) else False


def warm_up(game, jobs, harness, prefix):
    """Runs the game once without a window or sound for WARMUP_SECONDS, so the shaders it builds while it runs (the romfs cannot have
    them: ~212 in the opening, Ditto is black without them) are recorded, then translates them. The first real start then has them,
    also when it is started from Steam. A save folder of its own (shader_cache/warmup/save): the player's save is never touched.
    Proton (as Steam runs the game) when Steam has one, else Wine. Returns how many were translated; raises Fail when it cannot run."""
    if not os.path.isfile(os.path.join(game, 'pkp.exe')):
        raise Fail('pkp.exe is not built yet.')
    runner = find_proton()
    if runner is None:
        if not shutil.which('wine'):
            raise Fail('Neither Steam\'s Proton nor Wine was found to run the game once.')
        os.makedirs(prefix, exist_ok=True)
        runner = (['wine'], wine_env(prefix), 'Wine')
    cmd, env, label = runner
    scratch_rel = os.path.join('shader_cache', 'warmup')
    scratch = os.path.join(game, scratch_rel)
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(os.path.join(scratch, 'save'), exist_ok=True)
    # relative paths: the game's working directory is the game folder under Wine and Proton alike; environment variables win over pkp.ini
    env.update(NXRT_FS_SAVE_DIR=os.path.join(scratch_rel, 'save'), NXRT_NVN_DUMP_MISSING=MISSING_REL, NXRT_AUDIO_SINK='null')
    before = missing_count(game)
    say('Running the game once in the background (%s, about %d s) to record the shaders it builds while it runs...' % (label, WARMUP_SECONDS))
    p = subprocess.Popen(cmd + [os.path.join(game, 'pkp.exe'), '--headless', '--max-seconds', str(WARMUP_SECONDS),
                                '--log', os.path.join(scratch_rel, 'warmup.log')],
                         cwd=game, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    try:
        p.wait(timeout=WARMUP_SECONDS + 240)      # a first Proton start also creates its prefix
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, 9)
        p.wait()
    except KeyboardInterrupt:
        os.killpg(p.pid, 9)
        raise
    recorded = missing_count(game) - before
    say('The run recorded %d shader(s).' % max(0, recorded))
    n = refresh_shaders(game, jobs, harness, prefix) if missing_count(game) else 0
    shutil.rmtree(os.path.join(scratch, 'save'), ignore_errors=True)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    jobs_default = max(1, (os.cpu_count() or 2) - 1)
    for name in ('check', 'setup', 'play', 'refresh', 'warmup', 'status'):
        s = sub.add_parser(name)
        s.add_argument('--game', default=DEFAULT_GAME, help='release folder (default %(default)s)')
        s.add_argument('--prefix', default=DEFAULT_PREFIX, help='Wine prefix (default %(default)s)')
        if name in ('check', 'setup'):
            s.add_argument('--dump', required=name == 'check', help='your dump folder (base/ + updates/, or exefs/ + romfs/)')
        if name == 'setup':
            s.add_argument('--exefs', help='exefs folder, when it is not inside --dump (overrides the one found there)')
            s.add_argument('--romfs', help='romfs folder, when it is not inside --dump (overrides the one found there)')
            s.add_argument('--no-shaders', action='store_true', help='stop after building pkp.exe')
        if name == 'setup':
            s.add_argument('--no-warmup', action='store_true', help='skip the one hidden run of the game at the end')
        if name == 'warmup':
            s.add_argument('--if-needed', action='store_true', help='only while no run-time shaders were collected yet')
        if name in ('setup', 'refresh', 'warmup'):
            s.add_argument('--jobs', type=int, default=jobs_default)
            s.add_argument('--harness', help='natively built spirv_harness (.dll for dotnet, or an executable) instead of the release one under Wine')
        if name == 'play':
            s.add_argument('--offline', action='store_true', help='run with no network access at all')
            s.add_argument('extra', nargs=argparse.REMAINDER, help='extra pkp.exe arguments')
    b = sub.add_parser('_build')
    for k in ('--script', '--zig', '--exefs', '--game'):
        b.add_argument(k, required=True)
    b.add_argument('--jobs', type=int, required=True)
    a = ap.parse_args()
    if a.cmd == '_build':
        return _build_inprocess(a)
    game = os.path.abspath(os.path.expanduser(a.game))
    try:
        if a.cmd == 'check':
            exefs, romfs, label = find_dump(a.dump)
            say('Checking %s' % label)
            check_dump(game, exefs)
            say('The dump is good.')
        elif a.cmd == 'setup':
            def folder(p):
                p = os.path.abspath(os.path.expanduser(p))
                if not os.path.isdir(p):
                    raise Fail('%s is not a folder.' % p)
                return p
            if a.exefs and a.romfs:
                exefs, romfs = folder(a.exefs), folder(a.romfs)
            elif a.dump:
                exefs, romfs, _ = find_dump(a.dump) if not (a.exefs or a.romfs) else (None, None, None)
                if a.exefs or a.romfs:                # one given: take the other from the dump without requiring both there
                    d = folder(a.dump)
                    other = 'romfs' if a.exefs else 'exefs'
                    hits = sorted((os.path.join(r, other) for r, ds, _ in os.walk(d) if other in ds), reverse=True)
                    if not hits:
                        raise Fail('No %s/ folder under %s.' % (other, d))
                    exefs, romfs = (folder(a.exefs), hits[0]) if a.exefs else (hits[0], folder(a.romfs))
            elif all(os.path.isdir(d) for d in fs_dirs(game)):
                exefs, romfs = fs_dirs(game)
            else:
                raise Fail('Pass --dump <your dump folder> (or --exefs and --romfs) the first time.')
            say('exefs: %s\nromfs: %s\nChecking the exefs...' % (exefs, romfs))
            check_dump(game, exefs)
            prepare_game_folder(game, exefs, romfs)
            build_game(game, exefs, a.jobs)
            if a.no_shaders:
                say('\npkp.exe is built. Run setup again without --no-shaders for the shader cache.')
                return 0
            build_shaders(game, exefs, romfs, a.jobs, a.harness, a.prefix)
            if not a.no_warmup and not runtime_collected(game):
                try:
                    warm_up(game, a.jobs, a.harness, os.path.abspath(os.path.expanduser(a.prefix)))
                except Fail as e:                        # not fatal: the first start records them instead (refresh translates them)
                    say('The hidden run was skipped (%s) The first start records the shaders instead; run refresh after it.' % e)
            say('\nSetup finished. Play with:  python3 %s play' % os.path.relpath(__file__))
        elif a.cmd == 'status':
            m = read_marker(game)
            say('game folder   %s' % game)
            say('exefs/romfs   %s / %s' % tuple(fs_dirs(game)))
            say('pkp.exe       %s' % ('built' if os.path.isfile(os.path.join(game, 'build', 'done.json')) else 'not built'))
            say('shader cache  %s (%d blocks)' % ('complete' if shaders_complete(game) else 'not finished', m['blocks']))
            say('missing       %d shader(s) recorded while playing%s' % (missing_count(game), ' (run refresh)' if missing_count(game) else ''))
        elif a.cmd == 'refresh':
            refresh_shaders(game, a.jobs, a.harness, os.path.abspath(os.path.expanduser(a.prefix)))
        elif a.cmd == 'warmup':
            if a.if_needed and runtime_collected(game):
                say('The run-time shaders were collected already; no hidden run needed.')
            else:
                warm_up(game, a.jobs, a.harness, os.path.abspath(os.path.expanduser(a.prefix)))
        elif a.cmd == 'play':
            play(game, os.path.abspath(os.path.expanduser(a.prefix)), a.offline, [x for x in a.extra if x != '--'])
    except Fail as e:
        say('\nERROR: %s' % e)
        return 1
    except KeyboardInterrupt:
        say('\nPaused. Run the same command again to continue.')
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())

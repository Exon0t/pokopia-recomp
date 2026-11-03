# SPDX-License-Identifier: GPL-3.0-or-later
"""Adds pkp.exe to Steam as a non-Steam game that runs with Proton (what the Steam Deck needs to play it in Game Mode).

    python -I steam_shortcut.py status --exe <game>/pkp.exe
    python -I steam_shortcut.py add    --exe <game>/pkp.exe [--name "Pokémon Pokopia"] [--proton proton_experimental] [--close-steam]
    python -I steam_shortcut.py launch --exe <game>/pkp.exe

Steam keeps non-Steam games in userdata/<account>/config/shortcuts.vdf (binary VDF) and each game's compatibility tool in
config/config.vdf (text VDF, CompatToolMapping). It rewrites both when it exits, so `add` refuses to run while Steam is
running (--close-steam asks it to exit first). Both files are backed up next to themselves before any change. An existing
shortcut for the same pkp.exe is reused, never duplicated. Output: one JSON object on stdout; exit code 1 on errors.
"""
import argparse
import binascii
import json
import os
import shutil
import struct
import subprocess
import sys
import time

STEAM_ROOTS = ('~/.local/share/Steam', '~/.steam/steam', '~/.var/app/com.valvesoftware.Steam/.local/share/Steam')
ACCOUNT_ID_BASE = 76561197960265728


class Fail(Exception):
    pass


# ------------------------------------------------------------------------------------------------- binary VDF (shortcuts)

def bvdf_load(data):
    """Binary VDF -> nested list of (type, key, value); maps are lists, so order and duplicate keys survive."""
    pos = 0

    def cstr():
        nonlocal pos
        end = data.index(b'\0', pos)
        s = data[pos:end].decode('utf-8', 'surrogateescape')
        pos = end + 1
        return s

    def map_():
        nonlocal pos
        items = []
        while True:
            t = data[pos]
            pos += 1
            if t == 0x08:
                return items
            k = cstr()
            if t == 0x00:
                items.append((t, k, map_()))
            elif t == 0x01:
                items.append((t, k, cstr()))
            elif t == 0x02:
                items.append((t, k, struct.unpack_from('<i', data, pos)[0]))
                pos += 4
            elif t == 0x03:
                items.append((t, k, struct.unpack_from('<f', data, pos)[0]))
                pos += 4
            elif t == 0x07:
                items.append((t, k, struct.unpack_from('<Q', data, pos)[0]))
                pos += 8
            else:
                raise Fail('shortcuts.vdf: unknown field type 0x%02x at byte %d' % (t, pos - 1))

    root = map_()
    if data[pos:].strip(b'\x08'):
        raise Fail('shortcuts.vdf: unexpected data after the end')
    return root, len(data) - pos


def bvdf_dump(items, trailing=0):
    out = bytearray()

    def cstr(s):
        out.extend(s.encode('utf-8', 'surrogateescape') + b'\0')

    def map_(its):
        for t, k, v in its:
            out.append(t)
            cstr(k)
            if t == 0x00:
                map_(v)
            elif t == 0x01:
                cstr(v)
            elif t == 0x02:
                out.extend(struct.pack('<i', v))
            elif t == 0x03:
                out.extend(struct.pack('<f', v))
            elif t == 0x07:
                out.extend(struct.pack('<Q', v))
        out.append(0x08)

    map_(items)
    out.extend(b'\x08' * trailing)
    return bytes(out)


def field(entry, key, default=None):
    for t, k, v in entry:
        if k.lower() == key.lower():
            return v
    return default


# ----------------------------------------------------------------------------------------------- text VDF (config.vdf)

def tvdf_load(text):
    """Text VDF -> nested list of [key, value] where value is a str or a list (a block)."""
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == '/' and text.startswith('//', i):
            i = text.find('\n', i) if '\n' in text[i:] else n
        elif c in '{}':
            toks.append(c)
            i += 1
        elif c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == '\\' and j + 1 < n:
                    buf.append(text[j:j + 2])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            toks.append(('s', ''.join(buf)))
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '{}"':
                j += 1
            toks.append(('s', text[i:j]))
            i = j
    pos = 0

    def block():
        nonlocal pos
        items = []
        while pos < len(toks):
            t = toks[pos]
            if t == '}':
                pos += 1
                return items
            key = t[1]
            pos += 1
            if toks[pos] == '{':
                pos += 1
                items.append([key, block()])
            else:
                items.append([key, toks[pos][1]])
                pos += 1
        return items

    return block()


def tvdf_dump(items, depth=0):
    out = []
    tab = '\t' * depth
    for k, v in items:
        if isinstance(v, list):
            out.append('%s"%s"\n%s{\n%s%s}\n' % (tab, k, tab, tvdf_dump(v, depth + 1), tab))
        else:
            out.append('%s"%s"\t\t"%s"\n' % (tab, k, v))
    return ''.join(out)


def tvdf_child(items, key, create=False):
    for kv in items:
        if kv[0].lower() == key.lower() and isinstance(kv[1], list):
            return kv[1]
    if not create:
        return None
    new = []
    items.append([key, new])
    return new


# ---------------------------------------------------------------------------------------------------------- steam

def steam_root():
    for r in STEAM_ROOTS:
        p = os.path.realpath(os.path.expanduser(r))
        if os.path.isdir(os.path.join(p, 'userdata')):
            return p
    return None


def account_dir(root):
    """The userdata/<account> folder of the last Steam login: loginusers.vdf's MostRecent, else its newest timestamp, else
    AutoLogin (Steam versions differ in which they write), else the account whose localconfig.vdf changed last."""
    ud = os.path.join(root, 'userdata')
    ids = [d for d in os.listdir(ud) if d.isdigit() and d != '0']
    if not ids:
        raise Fail('Steam has no user folder yet: sign in to Steam once, then try again.')
    lu = os.path.join(root, 'config', 'loginusers.vdf')
    best = None
    if os.path.isfile(lu):
        users = tvdf_child(tvdf_load(open(lu, encoding='utf-8', errors='replace').read()), 'users') or []
        for sid, props in users:
            if not (isinstance(props, list) and sid.isdigit()):
                continue
            acc = str(int(sid) - ACCOUNT_ID_BASE)
            if acc not in ids:
                continue
            d = {k.lower(): v for k, v in props if isinstance(v, str)}
            ts = int(d.get('timestamp', '0')) if d.get('timestamp', '0').isdigit() else 0
            score = (d.get('mostrecent') == '1', ts, d.get('autologin') == '1')
            if best is None or score > best[0]:
                best = (score, acc)
    if best:
        return os.path.join(ud, best[1])

    def used(acc):
        p = os.path.join(ud, acc, 'config', 'localconfig.vdf')
        return os.path.getmtime(p) if os.path.isfile(p) else 0
    return os.path.join(ud, max(ids, key=used))


def steam_running():
    try:
        return subprocess.run(['pgrep', '-x', 'steam'], capture_output=True).returncode == 0
    except FileNotFoundError:
        pid = os.path.expanduser('~/.steam/steam.pid')
        try:
            os.kill(int(open(pid).read().strip()), 0)
            return True
        except (OSError, ValueError):
            return False


def shortcut_appid(exe_quoted, name):
    return (binascii.crc32((exe_quoted + name).encode('utf-8')) & 0xFFFFFFFF) | 0x80000000


def norm_exe(s):
    return os.path.realpath(s.strip().strip('"')) if s else ''


def find_shortcut(entries, exe):
    for idx, (t, k, e) in enumerate(entries):
        if t == 0x00 and norm_exe(field(e, 'Exe', '')) == norm_exe(exe):
            return idx, e
    return None, None


def load_shortcuts(acc):
    p = os.path.join(acc, 'config', 'shortcuts.vdf')
    if not os.path.isfile(p):
        return p, [(0x00, 'shortcuts', [])], 0
    root, trailing = bvdf_load(open(p, 'rb').read())
    return p, root, trailing


def status(exe):
    root = steam_root()
    st = {'steam_found': bool(root), 'steam_running': steam_running(), 'shortcut': False, 'appid': None, 'proton': None}
    if not root:
        return st
    acc = account_dir(root)
    _, broot, _ = load_shortcuts(acc)
    _, e = find_shortcut(field(broot, 'shortcuts', []), exe)
    if e is not None:
        appid = field(e, 'appid')
        appid = (appid & 0xFFFFFFFF) if appid is not None else shortcut_appid(field(e, 'Exe', ''), field(e, 'AppName', ''))
        st.update(shortcut=True, appid=appid, name=field(e, 'AppName', ''))
        cfg = os.path.join(root, 'config', 'config.vdf')
        if os.path.isfile(cfg):
            ctm = mapping_block(tvdf_load(open(cfg, encoding='utf-8', errors='replace').read()))
            for k, v in (ctm or []):
                if k == str(appid) and isinstance(v, list):
                    st['proton'] = next((val for kk, val in v if kk == 'name'), None)
    return st


def mapping_block(cfg, create=False):
    node = cfg
    for k in ('InstallConfigStore', 'Software', 'Valve', 'Steam', 'CompatToolMapping'):
        node = tvdf_child(node, k, create)
        if node is None:
            return None
    return node


def backup(path):
    if os.path.isfile(path):
        shutil.copy2(path, '%s.pokopia-%s.bak' % (path, time.strftime('%Y%m%d-%H%M%S')))


def write_atomic(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def add(exe, name, proton, close_steam):
    exe = os.path.realpath(exe)
    if not os.path.isfile(exe):
        raise Fail('%s does not exist: run Set up first.' % exe)
    root = steam_root()
    if not root:
        raise Fail('Steam was not found. Install Steam and sign in once.')
    if steam_running():
        if not close_steam:
            raise Fail('Steam is running. Close Steam (Steam menu, Exit) and try again: it rewrites its settings when it exits.')
        subprocess.run(['steam', '-shutdown'], capture_output=True, timeout=30)
        for _ in range(60):
            if not steam_running():
                break
            time.sleep(0.5)
        else:
            raise Fail('Steam did not exit. Close it from its menu and try again.')
        time.sleep(1)
    acc = account_dir(root)
    sc_path, broot, trailing = load_shortcuts(acc)
    entries = field(broot, 'shortcuts')
    if entries is None:
        entries = []
        broot.append((0x00, 'shortcuts', entries))
    idx, e = find_shortcut(entries, exe)
    folder = os.path.dirname(exe)
    if e is None:
        exe_q = '"%s"' % exe
        appid = shortcut_appid(exe_q, name)
        e = [(0x02, 'appid', appid - (1 << 32) if appid >= 1 << 31 else appid), (0x01, 'AppName', name), (0x01, 'Exe', exe_q),
             (0x01, 'StartDir', '"%s/"' % folder), (0x01, 'icon', ''), (0x01, 'ShortcutPath', ''), (0x01, 'LaunchOptions', ''),
             (0x02, 'IsHidden', 0), (0x02, 'AllowDesktopConfig', 1), (0x02, 'AllowOverlay', 1), (0x02, 'OpenVR', 0),
             (0x02, 'Devkit', 0), (0x01, 'DevkitGameID', ''), (0x02, 'DevkitOverrideAppID', 0), (0x02, 'LastPlayTime', 0),
             (0x01, 'FlatpakAppID', ''), (0x00, 'tags', [])]
        nums = [int(k) for t, k, _ in entries if k.isdigit()]
        entries.append((0x00, str(max(nums) + 1 if nums else 0), e))
        backup(sc_path)
        os.makedirs(os.path.dirname(sc_path), exist_ok=True)
        write_atomic(sc_path, bvdf_dump(broot, trailing))
        created = True
    else:
        appid = field(e, 'appid')
        appid = (appid & 0xFFFFFFFF) if appid is not None else shortcut_appid(field(e, 'Exe', ''), field(e, 'AppName', ''))
        created = False
    cfg_path = os.path.join(root, 'config', 'config.vdf')
    cfg = tvdf_load(open(cfg_path, encoding='utf-8', errors='replace').read()) if os.path.isfile(cfg_path) else []
    ctm = mapping_block(cfg, create=True)
    ctm[:] = [kv for kv in ctm if kv[0] != str(appid)]
    ctm.append([str(appid), [['name', proton], ['config', ''], ['priority', '250']]])
    backup(cfg_path)
    write_atomic(cfg_path, tvdf_dump(cfg).encode('utf-8'))
    return {'ok': True, 'created': created, 'appid': appid, 'proton': proton, 'shortcuts': sc_path, 'config': cfg_path}


def launch(exe):
    st = status(exe)
    if not st.get('shortcut'):
        raise Fail('pkp.exe is not in Steam yet: use Add to Steam first.')
    gameid = (st['appid'] << 32) | 0x02000000
    url = 'steam://rungameid/%d' % gameid
    for cmd in (['steam', url], ['xdg-open', url]):
        try:
            subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            return {'ok': True, 'url': url}
        except FileNotFoundError:
            continue
    raise Fail('Could not start Steam (%s).' % url)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=('status', 'add', 'launch'))
    ap.add_argument('--exe', required=True)
    ap.add_argument('--name', default='Pokémon Pokopia')
    ap.add_argument('--proton', default='proton_experimental')
    ap.add_argument('--close-steam', action='store_true')
    a = ap.parse_args()
    try:
        out = status(a.exe) if a.cmd == 'status' else add(a.exe, a.name, a.proton, a.close_steam) if a.cmd == 'add' else launch(a.exe)
        print(json.dumps(out))
        return 0
    except Fail as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 1


if __name__ == '__main__':
    sys.exit(main())

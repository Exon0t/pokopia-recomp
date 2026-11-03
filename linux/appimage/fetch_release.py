# SPDX-License-Identifier: GPL-3.0-or-later
"""Installs the official Windows release package into a game folder, for the Linux launcher.

    python -I fetch_release.py --dir <game folder> [--zip <already downloaded PokopiaRecomp-*.zip>]

The zip comes from the project's own GitHub release and must match the pinned size and sha256. Everything in it is kept
except what the Linux launcher replaces with its own Linux builds: the Windows Python and Zig, the shader translator
(the release's copy predates the release's own sine/cosine fix), the Windows launcher and Play.bat.
Output, one per line on stdout:  @progress <download|extract> <done> <total> <text>  /  @error <text>
Exit codes: 0 installed, 1 error.
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

VERSION = '0.1.1'
URL = 'https://github.com/drakolordx7/pokopia-recomp/releases/download/v0.1.1/PokopiaRecomp-0.1.1-win64.zip'
SHA256 = '170493047c51608e570dd081b57e4a1b80b665e54a9b6fdaa071df95a4674258'
SIZE = 169907643
TOP = 'PokopiaRecomp-0.1.1/'
SKIP = ('tools/python/', 'tools/spirv_harness/', 'tools/build/zig/', 'PokopiaLauncher.exe', 'Play.bat')


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def download(dest):
    part = dest + '.part'
    h, done = hashlib.sha256(), 0
    req = urllib.request.Request(URL, headers={'User-Agent': 'PokopiaLauncher-Linux'})
    try:
        with urllib.request.urlopen(req, timeout=30) as r, open(part, 'wb') as out:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                out.write(b)
                h.update(b)
                done += len(b)
                if done > SIZE:
                    raise RuntimeError('the download is larger than the release; stopped')
                say('progress', 'download', done, SIZE, 'Downloading Pokopia Recomp %s from GitHub...' % VERSION)
        if done != SIZE or h.hexdigest() != SHA256:
            raise RuntimeError('the download does not match the pinned release (%d bytes, sha256 %s); it was discarded' % (done, h.hexdigest()))
        os.replace(part, dest)
    finally:
        if os.path.exists(part):
            os.remove(part)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--zip')
    a = ap.parse_args()
    game = os.path.abspath(os.path.expanduser(a.dir))
    try:
        os.makedirs(game, exist_ok=True)
        if a.zip:
            z = os.path.abspath(os.path.expanduser(a.zip))
            say('progress', 'download', 0, SIZE, 'Checking %s...' % os.path.basename(z))
            if os.path.getsize(z) != SIZE or sha256_of(z) != SHA256:
                raise RuntimeError('%s is not the Pokopia Recomp %s release zip (size or sha256 differs)' % (z, VERSION))
        else:
            z = os.path.join(game, os.path.basename(URL))
            if not (os.path.isfile(z) and os.path.getsize(z) == SIZE and sha256_of(z) == SHA256):
                download(z)
        with zipfile.ZipFile(z) as zf:
            members = [m for m in zf.infolist() if m.filename.startswith(TOP) and not m.is_dir()]
            members = [m for m in members if not m.filename[len(TOP):].startswith(SKIP)]
            for i, m in enumerate(members):
                rel = m.filename[len(TOP):]
                dst = os.path.normpath(os.path.join(game, rel))
                if not dst.startswith(game + os.sep):
                    raise RuntimeError('unsafe path in the zip: %s' % m.filename)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with zf.open(m) as src, open(dst + '.part', 'wb') as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                os.replace(dst + '.part', dst)
                if i % 25 == 0 or i == len(members) - 1:
                    say('progress', 'extract', i + 1, len(members), 'Unpacking: %s' % rel)
        if not a.zip:
            os.remove(z)
        open(os.path.join(game, 'release.txt'), 'w').write(VERSION + '\n')
        say('progress', 'extract', 1, 1, 'Pokopia Recomp %s is installed in %s' % (VERSION, game))
        return 0
    except Exception as e:  # noqa: BLE001 - reported to the launcher in one line
        say('error', str(e).replace('\n', ' '))
        return 1


if __name__ == '__main__':
    sys.exit(main())

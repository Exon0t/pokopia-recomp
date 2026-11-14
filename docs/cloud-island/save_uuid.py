# save_uuid.py <save dir> [show|reroll]: the player UUID of a Pokopia recomp save; reroll gives it a new random one in every place
# (G00/B00 CMNDAT: zlib from 0x130, end offset at +0x10; S00/SYDAT plain), as runtime/games/pkp/core/pkp_save_uuid.cpp does for the shared id.
# For test copies only: two copies of one save are the same player, and a Link Play / Cloud Island host refuses its own id (error 9005).
import sys, os, zlib, struct, glob, collections
base = sys.argv[1]; mode = sys.argv[2] if len(sys.argv) > 2 else 'show'
root = [d for d in sorted(glob.glob(os.path.join(base, 'account', '*/'))) if '.' not in os.path.basename(os.path.normpath(d))][0]
def cmn(p):
    f = open(p, 'rb').read(); end = struct.unpack_from('<I', f, 0x10)[0]
    return f, zlib.decompress(f[0x130:end])
sy = open(os.path.join(root, 'S00', 'SYDAT.BIN'), 'rb').read()
f, raw = cmn(os.path.join(root, 'G00', 'CMNDAT.BIN'))
cands = collections.Counter()
for i in range(len(sy) - 15):
    u = sy[i:i + 16]
    if (u[6] >> 4) == 4 and (u[8] >> 6) == 2 and len(set(u)) > 8 and raw.count(u): cands[u] += raw.count(u)
if not cands: sys.exit('no UUID shared by SYDAT and CMNDAT found')
old = cands.most_common(1)[0][0]
print('player UUID', old.hex(), 'candidates', [(k.hex(), v) for k, v in cands.most_common(3)])
if mode != 'reroll': sys.exit(0)
new = bytearray(os.urandom(16)); new[6] = (new[6] & 0x0F) | 0x40; new[8] = (new[8] & 0x3F) | 0x80; new = bytes(new)
for rel, is_cmn in (('G00/CMNDAT.BIN', True), ('B00/CMNDAT.BIN', True), ('S00/SYDAT.BIN', False)):
    p = os.path.join(root, rel)
    if not os.path.isfile(p): continue
    if not is_cmn:
        d = open(p, 'rb').read(); n = d.count(old); open(p, 'wb').write(d.replace(old, new)); print(rel, n); continue
    f, raw = cmn(p); n = raw.count(old)
    if n:
        z = zlib.compress(raw.replace(old, new), 9); end = 0x130 + len(z)
        out = bytearray(max(end, len(f))); out[:0x130] = f[:0x130]; out[0x130:end] = z; struct.pack_into('<I', out, 0x10, end)
        open(p, 'wb').write(out)
    print(rel, n)
print('now', new.hex())

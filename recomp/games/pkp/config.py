# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-game configuration of Pokémon Pokopia (Nintendo Switch 2, title 0400A3A01EE4C000).

Supported builds of main are listed in BUILDS (build id -> display version); the setup tool checks the build id. The
lifter itself is build-neutral: the hook sites of main are in main.sites.
Modules: main at 0x7100000000, sdk, subsdk0 (nn::movie) and rtld at the runtime's bases.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from nxrecomp.config import GameConfig, ModuleSpec          # noqa: E402

BUILDS = {
    '76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E': '2.0.1',
}


class Config(GameConfig):
    name = 'pkp'
    title_id = '0400A3A01EE4C000'
    version = '2.0.1'
    build_id = '76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E'       # of main (first 20 bytes; the rest of the 32-byte field is zero)
    # in NxModule.index order; translation units about 45k instructions each (main has 10.3 M instruction words)
    modules = [
        ModuleSpec('main', 0x7100000000, 'main', shards=230, sites='main.sites'),
        ModuleSpec('sdk', 0x7500000000, 'sdk', shards=45),
        ModuleSpec('subsdk0', 0x7600000000, 'subsdk0', shards=4),
        ModuleSpec('rtld', 0x7700000000, 'rtld', shards=1, entry=True),
    ]
    overrides = {}
    # registers and flags in C locals, flags stored around calls only where a module summary says they can be read (nxrecomp/config.py)
    reg_locals = 'lean'
    # rtld's process entry (0x0) has no FDE, symbol or direct branch, so discovery never roots it (runtime/docs/S6.md)
    # cloud island proof of concept: function entries in main that discovery misses (stored as words, never lifted in 0.11.1)
    extra_entries = {'rtld': [0x0], 'main': [0xd0c0, 0x1730338, 0x17317a0, 0x1731b38, 0x1732400, 0x1733f64, 0x1734550, 0x1734e1c, 0x17351d8, 0x1735210, 0x173611c, 0x1736fb8, 0x1737034, 0x1738c20, 0x1738c78, 0x1738cc4, 0x1738cf0, 0x1738d70, 0x1738dec, 0x1738f5c, 0x1739164, 0x17391e0, 0x1739248, 0x173b03c, 0x173b1c0, 0x173b4f8, 0x173e1f0, 0x173e200, 0x173e214, 0x173e218, 0x173ef48, 0x173efc4, 0x17406d0, 0x1740920, 0x1740974, 0x1741710, 0x1743a48, 0x1743d68, 0x1744614, 0x1744664, 0x17446b4, 0x17446ec, 0x1744884, 0x17448fc, 0x1744ccc, 0x1744dd8, 0x1744ea0, 0x1744f08, 0x1745264, 0x17456b8, 0x1747354, 0x1747358, 0x174735c, 0x1747360, 0x174acb0, 0x1750c64, 0x1750cfc, 0x1750d7c, 0x1751304, 0x17531ac, 0x17531c0, 0x17531dc, 0x17531e0, 0x1753268, 0x1753290, 0x17532a0, 0x17532ac, 0x17535cc, 0x1753694, 0x17538d4, 0x17539e8, 0x17539fc, 0x1753a1c, 0x1753a20, 0x1753a34, 0x1753a88, 0x17547ac, 0x1754900, 0x1756010, 0x175a84c, 0x175b89c, 0x175d4cc, 0x1760a74, 0x1760f4c, 0x1762680, 0x17626c8, 0x1762a50, 0x1762bb4, 0x1765168, 0x17651c4, 0x1765220, 0x1765500, 0x17660a0, 0x17671d4, 0x17671d8, 0x17671e0, 0x17671e8, 0x17671f0, 0x17671f4, 0x17671f8, 0x1767200, 0x1767208, 0x1767210, 0x1767218, 0x1767220, 0x1767228, 0x176729c, 0x17672dc, 0x176743c, 0x1767498, 0x1767508, 0x1767564, 0x176761c, 0x1767828, 0x1767864, 0x17678b4, 0x17678b8, 0x17678e4, 0x1767974, 0x17679c8, 0x17679d0, 0x17679e4, 0x17679f8, 0x1767a14, 0x1767a78, 0x1768418, 0x17684bc, 0x1769584, 0x176974c, 0x1769834, 0x1769838]}

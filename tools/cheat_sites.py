#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Awaken (Lantern Engine, the signatures); bbport contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""The cheats' code sites (docs/CHEATS_PLAN.md, phase 0): Lantern Engine's byte signatures
(engine/gpu/shim/engine/cheats.cpp at 4715bca, github.com/Awakenchan/LanternEngine) against
this tree and a game image.

    python3 tools/cheat_sites.py
        what could change those bytes: every write of patches/Bloodborne.xml (chosen or not) and
        every code address the port's sources name (hooks, detours, fixes, BBSC) near a site
    python3 tools/cheat_sites.py --boot ~/.local/share/bbport/out/boot-linked.bin
        the image's bytes at each site, as built and after the chosen patches (patches.bin next to
        the boot file, or --patches FILE)

Addresses are image offsets: Lantern's PS4 addresses - 0x400000 (the eboot at image offset 0).
Exit status 1 when a site differs or something writes there.
"""
import argparse
import pathlib
import re
import struct
import sys

EBOOT_BASE = 0x400000
ROOT = pathlib.Path(__file__).resolve().parent.parent

# (cheats that need it, name, image offset, bytes). Firearm range and the echo multiplier have none:
# they check the param tables' layout instead.
SITES = [
    ('vials, bullets', 'Row', 0x1f1e3d0, '554889e5415741564154534189f44989fe31c94585e40f88f2000000'),
    ('vials, bullets', 'Cap', 0x1f1e7e0, '554889e5534883ec28488b1df0534c03488b03488945f0488b4f08b8630000004885c90f848c0000008b51420fb74146'),
    ('vials, bullets', 'CapMaxNum', 0x1f1e887, '0fbf493685c9b8630000000f4fc1'),
    ('vials, bullets', 'Slot', 0x14d9f60, '8b0683f8ff0f84a600000089c14869c9153fef0348c1e920'),
    ('vials, bullets', 'Add', 0x14cdfd0, '554889e54157415641554154534881ec180100004189cd4189d44989ff488b1dec5bf103488b03488945d041c7879c02'),
    ('vials, bullets', 'AddInventory', 0x14ce1a9, 'e82202a5004d8db758010000be000000404c89f74489e2e8bbbc0000'),
    ('vials, bullets, consumables', 'Consume', 0x14d0dc0, 'e80bd6a4004889de488b45a04885c00f84c50200008b4042a9000080000f84b7020000'),
    ('vials, bullets', 'Bullets', 0x18fa1d0, 'e8cbf86200488b45984531e441bd000000004885c07408440fbea83a010000'),
    ('vials, bullets', 'Equip', 0x13105ef, '488d053aab2204488b004c8b6808498dbdd0010000'),
    ('vials, bullets', 'Entry', 0x14d44be, 'e89d5a0000c78534feffff0000000085c0784f418b8c24e0010000ffc139c1764189c1412b8c247c010000731089c148c1e10449038c24b0010000eb0c48c1e10449038c24a001000083390074144885c9740f8b4108898534feffff'),
    ('stamina', 'StaminaLoad', 0x18b6451, '498b8c24b00300004c8b7920418b9f34010000'),
    ('stamina', 'StaminaClamp', 0x18b64a2, '488d05e183c803803800751fb8ceffffff83fbce7c0e418b873801000039d80f4ed889d841898734010000'),
    ('stealth', 'StealthMenu', 0x19214b7, '488d35fc480603488d15c9d3c1'),
    ('stealth', 'SilentMenu', 0x19214f9, '488d35da480603488d1588d3c1'),
    ('stealth', 'StealthUse', 0x1a554db, '488d05ac93ae038a00884364'),
    ('stealth', 'SilentUse', 0x1a552c3, '488d05c595ae03803800'),
    ('keep echoes', 'BloodMark', 0x13d2b82, '458ba594000000488d0578851604488b004885c0752b488d3dca875603488d1517885603488d0dfe855603beb100000031c0e8f729ce00488d0548851604488b00488b400844036028c74028000000004489633841c7859400000000000000'),
    ('no weapon wear', 'Durability', 0x14d24e0, '554889e54157415641554154534883ec684189f54889fb4c8b25e216f103498b0424488945d04183fd070f87af00000089558c4d63f5428b84b39400000089459048c7459800000000c745a0ffffffff488d7d90e8376b5b008b45a025ffffff0f3dffffff0f41bfffffffff440f45f84183ffff'),
    ('no weapon wear', 'DurabilityClamp', 0x14d265d, '4401f3488b4d9831d2b8000000004885c974070fb781be00000085db780739c30f4fd889da488b7d804881c7580100008bb57cffffffe858710000'),
    ('no weapon wear', 'DurabilityCall', 0x18fbacb, '498bbfc00300004881c7d001000089de4489e2e8fd69bdff'),
    ('add echoes / insight', 'Insight', 0x18fc567, '4403b884000000780d4183ff63b963000000410f4ecf898884000000'),
    ('add echoes / insight', 'Echoes', 0x18fc6a4, '488b93c00300008bb29400000031c989f701c7780e81ffffc99a3bb9ffc99a3b0f4ecf898a94000000'),
]
# Code the cheats write or call into (not signatures): the keep-echoes patch, the game functions
# phases 2 and 3 call. A hook there would matter too.
TARGETS = [
    ('keep echoes', 'BloodMarkPatch', 0x13d2bd2, 15),
    ('no weapon wear', 'ChangeDurability', 0x14d24e0, 6),
    ('no weapon wear', 'GaitemLookup', 0x1a89070, 6),
]
# Where the port names code addresses (image offsets, or PS4 addresses in the patch lists).
SOURCES = ['src/*.c', 'src/*/*.c', 'src/*.h', 'gpu/shim/*.cpp', 'gpu/shim/*.h', 'gpu/games/bloodborne/*.cpp',
           'gpu/games/bloodborne/*.h', 'gpu/bbnet20/bbsc/core/*.cpp', 'gpu/bbnet20/bbsc/core/*.h',
           'gpu/bbnet20/bbsc/core/*.inc', 'scripts/*.py']
REACH = 16  # an address named this far before a site may still cover its first bytes (a hook's span)
ONLINE_ONLY = 'gpu/bbnet20/'  # the online module: it runs only online, where every cheat is off anyway


def ranges():
    for cheats, name, at, data in SITES:
        yield cheats, name, at, len(data) // 2
    yield from TARGETS


def xml_writes(path):
    """(patch name, image offset, length) of every literal write in a shadPS4 patch list."""
    sizes = {'bytes16': 2, 'bytes32': 4, 'bytes64': 8, 'float32': 4, 'float64': 8}
    text = path.read_text(encoding='utf-8', errors='replace')
    for block in re.finditer(r'<Metadata\b([^>]*)>(.*?)</Metadata>', text, re.S):
        name = re.search(r'\bName="([^"]*)"', block.group(1))
        for line in re.finditer(r'<Line\s+([^>]*)/>', block.group(2)):
            attrs = dict(re.findall(r'(\w+)="([^"]*)"', line.group(1)))
            kind, value = attrs.get('Type', ''), attrs.get('Value', '')
            try:
                address = int(attrs.get('Address', ''), 16)
            except ValueError:
                continue
            if kind == 'bytes':
                length = len(value.replace(' ', '')) // 2
            elif kind in sizes:
                length = sizes[kind]
            else:
                length = max(1, len(value))  # utf8 / utf16: an upper bound is enough here
            yield name.group(1) if name else '?', address - EBOOT_BASE, length


def static_check():
    problems = 0
    xml = ROOT / 'patches' / 'Bloodborne.xml'
    if not xml.is_file():
        print(f'{xml}: not there (not run from the source tree): only the image is checked')
        return 0
    writes = list(xml_writes(xml))
    print(f'{xml.relative_to(ROOT)}: {len(writes)} writes')
    for patch, at, length in writes:
        for cheats, name, start, size in ranges():
            if at < start + size and start < at + length:
                print(f'  OVERLAP: patch "{patch}" writes {length} bytes at {at:#x}: {name} ({cheats}) {start:#x}+{size}')
                problems += 1
    named = online = 0
    for pattern in SOURCES:
        for path in sorted(ROOT.glob(pattern)):
            relative = path.relative_to(ROOT).as_posix()
            text = path.read_text(encoding='utf-8', errors='replace')
            for number, line in enumerate(text.splitlines(), 1):
                code = line.strip()
                if code.startswith(('//', '/*', '*', '#')) or path.name == 'cheat_sites.py':
                    continue  # comments name addresses without writing there
                for m in re.finditer(r'\b0x([0-9a-fA-F]{6,9})\b', code):
                    value = int(m.group(1), 16)
                    for at in {value, value - EBOOT_BASE}:
                        for cheats, name, start, size in ranges():
                            if start - REACH <= at < start + size:
                                kind = 'online only' if relative.startswith(ONLINE_ONLY) else 'NAMED'
                                print(f'  {kind}: {relative}:{number}: {at:#x} near {name} ({cheats}) '
                                      f'{start:#x}+{size}: {code[:90]}')
                                online += kind == 'online only'
                                named += kind == 'NAMED'
    print(f'port sources: {named} code addresses at or near a cheat site'
          + (f' ({online} more in the online module, which runs only when the cheats are off)' if online else ''))
    return problems + named


def boot_image(path):
    data = path.read_bytes()
    magic = data[:8]
    if not re.fullmatch(rb'BBPROBE[1-5]', magic):
        sys.exit(f'{path}: not a boot file (magic {magic!r})')
    size = struct.unpack_from('<Q', data, 8)[0]
    if size > len(data) - 16:
        sys.exit(f'{path}: image size {size} beyond the file')
    return bytearray(data[-size:])  # the image is the file's last `size` bytes (src/probe.c)


def apply_patch_file(image, path):
    data = path.read_bytes()
    if data[:8] != b'BBPATCH2':
        sys.exit(f'{path}: not a patch file')
    base, count = struct.unpack_from('<QQ', data, 8)
    at, touched = 24, []
    for _ in range(count):
        offset, length = struct.unpack_from('<QQ', data, at)
        at += 16
        image[offset:offset + length] = data[at:at + length]  # pointer rebasing does not matter here
        touched.append((offset, length))
        at += length
    return touched


def image_check(boot, patches):
    image = boot_image(boot)
    print(f'{boot}: image {len(image):,} bytes')
    built = bytes(image)
    touched = apply_patch_file(image, patches) if patches else []
    print(f'{patches}: {len(touched)} writes applied' if patches else 'no patches.bin: the image as built only')
    problems = 0
    for cheats, name, at, data in SITES:
        want = bytes.fromhex(data)
        as_built = built[at:at + len(want)] == want
        patched = bytes(image[at:at + len(want)]) == want
        hit = [f'{o:#x}+{n}' for o, n in touched if o < at + len(want) and at < o + n]
        state = 'ok' if as_built and patched else 'DIFFERS as built' if not as_built else 'DIFFERS after patches'
        where = f': patch writes at {", ".join(hit)}' if hit else ''
        print(f'  {state:22} {name:16} {at:#09x} +{len(want):<3} ({cheats}){where}')
        problems += state != 'ok'
    return problems


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('--boot', type=pathlib.Path, help='boot-linked.bin (bbport data folder, out/)')
    p.add_argument('--patches', type=pathlib.Path, help='patches.bin (default: next to --boot)')
    a = p.parse_args()
    problems = static_check()
    if a.boot:
        patches = a.patches or a.boot.with_name('patches.bin')
        problems += image_check(a.boot, patches if patches.is_file() else None)
    print('cheat sites: ' + ('all clear' if not problems else f'{problems} to look at'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())

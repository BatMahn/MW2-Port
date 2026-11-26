#!/usr/bin/env python3
"""Compare the port with collision and firing routines from the original Titanium disc.

Run tools/audit/build.sh first. Pass Install/8bit/battle.dll and
Install/16bit/mercsw.dll from your disc. Substituting the Battle DLL from the
16-bit installer checks the duplicate Battle table and its firing loop.
"""
import argparse
import ctypes as c
import math
from pathlib import Path
import random
import struct

from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_MODE_32

from mw2emu import Emu, HEAP, HEAP_SIZE, RET_MAGIC, STACK_BASE, STACK_TOP
from simdiff import lib, Report


class WindowsImage(Emu):
    def __init__(self, path):
        self.img = Path(path).read_bytes()
        pe, = struct.unpack_from('<I', self.img, 60)
        count, optional_size = struct.unpack_from('<H12xH', self.img, pe + 6)
        optional = pe + 24
        base, = struct.unpack_from('<I', self.img, optional + 28)
        size, = struct.unpack_from('<I', self.img, optional + 56)
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(base, (size + 4095) & ~4095)
        for i in range(count):
            va, length, start = struct.unpack_from('<III', self.img, optional + optional_size + i * 40 + 12)
            if length:
                self.write(base + va, self.img[start:start + length])
        self.uc.mem_map(RET_MAGIC, 4096)
        self.uc.mem_map(STACK_BASE, STACK_TOP - STACK_BASE)
        self.uc.mem_map(HEAP, HEAP_SIZE)
        self.heap_ptr = HEAP
        self.stubs = {}
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._bad)

    def float_at(self, address):
        return struct.unpack('<f', self.read(address, 4))[0]


def as_float(value):
    return c.c_float(value).value


def projected_faces(image):
    report = Report('3D polygon boundaries')
    shim = lib('msim')
    shim.shim_set_dos(0)
    shim.shim_projected_face.argtypes = (c.POINTER(c.c_float), c.c_int, c.c_int, c.c_int, c.c_float, c.c_float)
    rng = random.Random(17)
    face = image.alloc(128)
    vertices = image.alloc(88 * 8)
    image.w32(face + 4, 40)
    for a, b, routine in ((0, 2, 0x1001941a), (0, 1, 0x10019730), (1, 2, 0x10019a46)):
        for _ in range(300):
            x, y = (rng.randrange(-500, 500) / 8 for _ in range(2))
            dx, dy = (rng.randrange(1, 90) / 8 for _ in range(2))
            points = [(x, y), (x + dx, y), (x + dx * 1.5, y + dy), (x, y + dy)]
            if rng.getrandbits(1):
                points.insert(2, (x + dx * 0.25, y + dy * 0.5))
            if rng.getrandbits(1):
                points.reverse()
            image.w16(face + 2, len(points))
            flat = []
            for i, (u, v) in enumerate(points):
                image.w8(face + 40 + i, i)
                pos = [0.0] * 3
                pos[a], pos[b] = u, v
                image.write(vertices + i * 88 + 12, struct.pack('<3f', *pos))
                flat.extend(pos)
            storage = (c.c_float * len(flat))(*flat)
            for i, (u, v) in enumerate(points):
                end_u, end_v = points[(i + 1) % len(points)]
                mid_u, mid_v = (u + end_u) / 2, (v + end_v) / 2
                for qx, qy in ((u, v), (mid_u, mid_v), (mid_u - 1 / 1024, mid_v),
                               (mid_u + 1 / 1024, mid_v), (x + dx / 2, y + dy / 2)):
                    bits = struct.unpack('<2I', struct.pack('<2f', qx, qy))
                    original = image.call(routine, stack=(face, vertices, *bits))
                    port = shim.shim_projected_face(storage, len(points), a, b, qx, qy)
                    report.check(original == port, (a, b, points, qx, qy, original, port))
    return report.done()


def weapon_tables(image, fixed):
    report = Report('Battle weapon table' if fixed else '16-bit weapon table')
    shim = lib('combat')
    shim.shim_set_dos(0)
    base = next(i for i in ((0xb4ff0, 0xb51f8) if fixed else (0x98638,))
                if image.img[i + 0x60:i + 0x65] == b'LRM20')
    for weapon in range(66):
        row = base + weapon * 104

        def number(offset, floating=False):
            return struct.unpack_from('<f' if floating else '<i', image.img, row + offset)[0]

        expected = [number(offset, not fixed and offset in (0x3c, 0x4c, 0x50))
                    for offset in (0x20, 0x2c, 0x3c, 0x28, 0x4c, 0x50, 0x30, 0x54, 0x58, 0x5c)]
        expected.extend(number(offset, not fixed) / (65536 if fixed and offset != 0x40 else 1)
                        for offset in (0x40, 0x44, 0x48))
        got = (c.c_float * len(expected))()
        shim.shim_titanium_weapon(1 if fixed else 2, weapon, got)
        for field, (original, port) in enumerate(zip(expected, got)):
            report.check(as_float(original) == port, (weapon, field, original, port))
    return report.done()


def volley_timing(image, fixed):
    report = Report('Battle firing loop' if fixed else '16-bit firing loop')
    if fixed and image.img[0xb51f8 + 0x60:0xb51f8 + 0x65] == b'LRM20':
        offsets = (0x10061275, 0x1006168b, 0x100b6df8, 269079384, 269161972)
        sound_calls = (0x1006132b - 389042, 0x10061340 - 389063)
        player = 269085144
    elif fixed:
        offsets = (0x10061075, 0x1006148b, 0x100b6df0, 269079384, 269161964)
        sound_calls = (0x1006112b - 388530, 0x10061140 - 388551)
        player = 269085144
    else:
        offsets = (0x1006c455, 0x1006c7c6, 0x1009a038, 269117092, 269162936)
        sound_calls = (0x1006c503 - 370138, 0x1006c518 - 370159)
        player = 269150888
    start, end, table, clock, first = offsets
    for sound in sound_calls:
        image.stub(sound)
    unit, obj, slot = (image.alloc(n) for n in (4096, 256, 64))
    image.w32(unit, obj)
    image.w32(obj + 4, 1)
    image.w32(player, 0)
    bp = STACK_TOP - 0x800
    shim = lib('combat')
    shim.shim_set_dos(0)
    for weapon in (0, 1, 2, 3, 4, 5, 6, 7, 25, 26, 35, 36, 37, 38, 39, 40, 41, 42, 51, 58, 59, 60):
        row = table + 104 * weapon
        interval = image.r32(row + 88)
        shots = image.r32(row + 44) - 1
        released = []
        image.stub(image.r32(row + 4), lambda _: released.append(1) or 1)
        for age, ticks in ((0, 1), (0, interval - 1), (0, interval), (interval - 1, 0),
                           (interval - 1, 1), (interval - 1, 2), (1, interval), (0, 2 * interval)):
            if shots <= (age + ticks) // interval:
                continue
            released.clear()
            for addr, value in ((bp + 8, unit), (bp - 16, slot), (bp - 24, row), (unit + 3524, 0),
                                (first, 0), (clock, ticks), (slot + 4, weapon), (slot + 8, 2),
                                (slot + 16, age), (slot + 36, shots)):
                image.w32(addr, value)
            image.run(start, end, ebp=bp)
            heat = image.rs32(unit + 3524) / 65536 if fixed else image.float_at(unit + 3524)
            expected = (len(released), image.rs32(slot + 16), heat)
            got = (c.c_float * 3)()
            shim.shim_titanium_volley(1 if fixed else 2, weapon, age, ticks, shots, got)
            report.check(list(got[:2]) == list(expected[:2]) and math.isclose(got[2], heat, abs_tol=1e-7),
                         (weapon, age, ticks, expected, tuple(got)))
    return report.done()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('battle')
    parser.add_argument('mercsw')
    args = parser.parse_args()
    battle, mercsw = WindowsImage(args.battle), WindowsImage(args.mercsw)
    results = [projected_faces(mercsw), weapon_tables(battle, True), weapon_tables(mercsw, False),
               volley_timing(battle, True), volley_timing(mercsw, False)]
    return 0 if all(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())

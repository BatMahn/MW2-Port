#!/usr/bin/env python3
"""simdiff - differential tests: the original DOS MW2.EXE (1.1) functions (run under unicorn by mw2emu.py from the ELF
image of the LE executable, NOT in the repo: MW2_ELF=/path/mw2dos.elf, made by le2elf.py) against the port's C in its
DOS edition mode (tools/audit/build.sh builds the shims into SIMDIFF_BUILD, default /tmp/simdiff_build).

  tools/audit/build.sh && MW2_ELF=mw2dos.elf python3 tools/audit/simdiff.py [-n cases] [-s seed] [test ...]

Each test drives both sides with the same random / edge inputs and prints PASS / FAIL with the first differences.
Modelled on the GBL port's tools/audit/simdiff.py; MW2.EXE's controller offsets are mostly GBL's minus 4."""
import ctypes, os, random, sys, math, struct
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mw2emu
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW
s32 = mw2emu.s32

BUILD = os.environ.get('SIMDIFF_BUILD', '/tmp/simdiff_build')
TESTS = {}
def test(fn): TESTS[fn.__name__] = fn; return fn

_libs = {}
def lib(name):
    if name not in _libs:
        L = ctypes.CDLL(os.path.join(BUILD, 'libshim_%s.so' % name))
        L.shim_set_dos(1)
        _libs[name] = L
    return _libs[name]

_emu = None
def emu():
    global _emu
    if _emu is None: _emu = mw2emu.Emu()
    _emu.reset_data(); _emu.stubs.clear()
    return _emu

class Report:
    def __init__(self, name): self.name, self.n, self.bad, self.notes = name, 0, [], []
    def check(self, ok, what):
        self.n += 1
        if not ok and len(self.bad) < 12: self.bad.append(what)
        elif not ok: self.bad.append(None)
    def done(self):
        st = 'PASS' if not self.bad else 'FAIL'
        print('%-22s %s  %d checks, %d differ' % (self.name, st, self.n, len(self.bad)))
        for b in self.bad:
            if b: print('    ', b)
        for n in self.notes: print('     note:', n)
        return not self.bad

# MW2.EXE globals (VA)
NOW, TICKS, PLAYER, DIFPTR, TEMP = 0x9581c, 0x95820, 0x95870, 0x9586c, 0x957d0
WARN_LATCH, WARN_TIME = 0x961e0, 0xfb530
MSG, SND, RAND, BLOW = 0x42340, 0x40c50, 0x4a070, 0x24ec0

class ShimHeat(ctypes.Structure):
    _fields_ = [(n, ctypes.c_int) for n in ('heat16', 'sink16', 'ticks', 'add16', 'shutdown', 'pending', 'override',
                'latch', 'warn_latch', 'destroyed', 'msg_warn', 'msg_seq', 'msg_down', 'restarted')] + \
               [('seq_ticks', ctypes.c_float), ('heat', ctypes.c_float)]

def dif_block(e, invuln=0, coll=1, heat=1, level=1):
    d = e.alloc(8); e.write(d, bytes([0, invuln, 1, coll, heat, level, 0, 0])); e.w32(DIFPTR, d); return d

@test
def heat_state(rep, rnd, n):
    """MW2.EXE 0x24f90 (the per-frame heat: sinking - doubled shut down -, the 66 warning, the 81 sequence, the shutdown
    0x444 ticks on, BlowAmmo 0x71c ticks into a shutdown above 100, the restart) vs combat_tick (DOS), for the player;
    the override roll is kept out (heat_override_roll)"""
    L = lib('combat'); L.shim_unit_new.restype = ctypes.c_void_p
    u = ctypes.c_void_p(L.shim_unit_new())
    for case in range(n):
        e = emu()
        calls = []
        e.stub(MSG, lambda em: calls.append(('msg', em.reg('eax'))) or 0)
        e.stub(SND, lambda em: calls.append(('snd', em.reg('eax'))) or 0, pops=4)
        e.stub(RAND, lambda em: calls.append(('rand', em.reg('eax'))) or 1)
        e.stub(BLOW, lambda em: calls.append(('blow', 0)) or 0)
        sink = rnd.choice([50, 70, 100, 45]) * rnd.randint(10, 30)
        dif_block(e)
        obj = e.alloc(0x80); e.w32(obj + 4, 0); e.w16(obj + 0x14, 0)
        c = e.alloc(0x200); e.w32(c, obj); e.w32(c + 0x9c, sink); e.w32(c + 0xa0, 2)
        e.w32(PLAYER, 0); e.w32(WARN_LATCH, 0)
        now = 1000; e.w32(NOW, now)
        L.shim_heat_setup(u, sink, 1, case + 1)
        h = ShimHeat()
        for f in range(400):
            t = rnd.choice([1, 2, 3, 4, 5, 6, 8, 12, 20])
            add = rnd.choice([0, 0, 0, rnd.randint(0, 30) << 16, rnd.randint(0, 0x8000)])
            flags = e.r16(c + 0x10c)
            ovr = int((flags & 4) and not (flags & 8) and e.rs32(c + 0xa0) != 3 and rnd.random() < 0.03)
            if e.rs32(c + 0x98) >> 16 > 100 and (flags & 8): ovr = 0
            now += t
            e.w32(NOW, now); e.w32(TICKS, t); e.w32(c + 0x94, add)
            if ovr: e.w16(c + 0x10c, flags | 8)
            calls.clear()
            e.call(0x24f90, eax=c)
            h.ticks, h.add16 = t, add
            if ovr: h.override = 1
            L.shim_heat_frame(u, ctypes.byref(h))
            if any(x[0] == 'rand' for x in calls): break
            st = e.rs32(c + 0xa0); fl = e.r16(c + 0x10c)
            dos = dict(heat=e.rs32(c + 0x98) / 65536.0, shutdown=int(st == 3), pending=int(bool(fl & 4)),
                       override=int(bool(fl & 8)), latch=int(bool(fl & 0x1000)), warn_latch=e.r32(WARN_LATCH),
                       msg_warn=int(('msg', 0) in calls), msg_seq=int(('msg', 3) in calls), msg_down=int(('msg', 0xd) in calls),
                       restarted=int(st == 0), blow=int(('blow', 0) in calls))
            prt = dict(heat=h.heat, shutdown=h.shutdown, pending=h.pending, override=h.override, latch=h.latch,
                       warn_latch=h.warn_latch, msg_warn=h.msg_warn, msg_seq=h.msg_seq, msg_down=h.msg_down,
                       restarted=h.restarted, blow=h.destroyed)
            diff = [k for k in dos if (abs(dos[k] - prt[k]) > 0.01 if k == 'heat' else dos[k] != prt[k])]
            rep.check(not diff, 'case %d frame %d t=%d add=%.2f: %s' % (case, f, t, add / 65536.0,
                      ', '.join('%s dos %s port %s' % (k, dos[k], prt[k]) for k in diff)))
            if diff: break
            if st == 0:
                e.w32(c + 0xa0, 2); e.w32(c + 0x8c, now); L.shim_set_seq(u, ctypes.c_float(0))
            if dos['blow']: break
    L.shim_unit_free(u)

@test
def heat_sink_tiers(rep, rnd, n):
    """MW2.EXE 0x4cd7b..0x4ce25 (the loader's dissipation per heat sinking point by player / difficulty / temperature;
    16.16 per tick) vs the unit's rate as combat_init + combat_set_player_difficulty set it"""
    L = lib('combat'); L.shim_unit_sink.restype = ctypes.c_float
    temps = [-200, -31, -30, -29, 0, 49, 50, 51, 120] + [rnd.randint(-150, 150) for _ in range(n // 3)]
    for player in (0, 1):
        for dif in (0, 1, 2):
            for t in temps:
                e = emu()
                d = dif_block(e, level=dif)
                obj = e.alloc(0x40); e.w32(obj + 4, 0 if player else 5)
                c = e.alloc(0x100); e.w32(c, obj)
                e.w32(PLAYER, 0); e.w32(TEMP, t)
                e.run(0x4cd7b, 0x4ce25, edx=c)
                dos = s32(e.reg('ebx'))
                prt = L.shim_unit_sink(1, player, dif, t) * 65536
                rep.check(abs(prt - dos) < 0.01, 'player %d difficulty %d temp %d: dos %d port %.2f' % (player, dif, t, dos, prt))

@test
def heat_override_roll(rep, rnd, n):
    """Exercise the DOS modulo roll with the same 15-bit sample on both sides."""
    L = lib('combat')
    L.shim_override_roll.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_int]
    rng_index, rng_table = 0x9e8f4, 0x152ffc
    modulus = 1 << 32
    inverse = pow(1103515245, -1, modulus)
    for ticks in (*range(21), 65, 66, 100):
        bound = 500 * ticks
        edge = {0, 1, 32767}
        edge.update(k * bound + d for k in (1, 2) for d in (-1, 0, 1))
        samples = sorted(x for x in edge if 0 <= x < 32768)
        samples.extend(rnd.randrange(32768) for _ in range(n // 10))
        for sample in samples:
            for invulnerable in (0, 1):
                e = emu()
                blows = []
                e.stub(BLOW, lambda em: blows.append(True) or 0)
                e.stub(MSG, ret=0); e.stub(SND, ret=0, pops=4)
                dif_block(e, invuln=invulnerable)
                obj, ctrl = e.alloc(0x40), e.alloc(0x200)
                e.w32(ctrl, obj); e.w32(ctrl + 0xa0, 2)
                e.w32(ctrl + 0x98, 105 << 16); e.w16(ctrl + 0x10c, 4 | 8)
                e.w32(PLAYER, 0); e.w32(obj + 4, 0)
                e.w32(NOW, 100); e.w32(TICKS, ticks)
                e.w32(rng_index, 0); e.w32(rng_table, sample)
                e.call(0x24f90, eax=ctrl)
                dos = bool(blows) | (e.rs32(rng_index) << 1)
                seed = (((sample << 16) - 12345) * inverse) % modulus
                port = L.shim_override_roll(ticks, seed, invulnerable)
                rep.check(dos == port, 'ticks %d sample %d invulnerable %d: DOS %d port %d' %
                          (ticks, sample, invulnerable, dos, port))

@test
def weapon_parameters(rep, rnd, n):
    """Check the live DOS weapon table and the AI's random bound for every weapon."""
    L = lib('combat')
    e = emu()
    result = (ctypes.c_int * 7)()
    for w in range(31):
        row = 0x9ee58 + w * 0x58
        offsets = (0x44, 0x48, 0x4c, 0x30, 0x34, 0x20)
        expected = tuple(e.rs32(row + off) for off in offsets) + (e.rs32(row + 0x44) // 91 + 1,)
        L.shim_weapon_parameters(w, result)
        rep.check(tuple(result) == expected, 'weapon %d: DOS %s port %s' % (w, expected, tuple(result)))

@test
def ai_volley_without_target(rep, rnd, n):
    """An in-progress volley keeps releasing rounds after the AI loses its target."""
    L = lib('msim')
    lock = ctypes.c_int()
    for online in (0, 1):
        for rounds in (1, 2):
            for ticks, elapsed, dt in ((1, 0, 1/182), (44, 0, 44/182), (45, 0, 45/182),
                                       (89, 0, 89/182), (90, 0, 90/182), (91, 0, 91/182),
                                       (6, 38, .033), (7, 38, .033), (6, 39, .033), (7, 39, .034)):
                e = emu()
                unit, obj, inputs = e.alloc(0x170), e.alloc(0x200), e.alloc(0x80)
                slots = e.alloc(10 * 0x68, fill=255)
                e.w32(unit, obj)
                e.w32(unit + 0x54, slots)
                e.w32(unit + 0xa0, 2 if online else 1)
                e.w32(obj + 4, 1)
                e.w32(obj + 0x4c, inputs)
                e.w32(PLAYER, 0)
                e.w32(NOW, 500)
                e.w32(TICKS, ticks)
                for offset, value in ((4, 5), (8, 2), (0x10, elapsed), (0x24, rounds), (0x28, 0)):
                    e.w32(slots + offset, value)
                launched = []
                e.stub(0x52620, lambda em: launched.append(em.reg('edx')) or 1)
                e.stub(0x51000)
                e.call(0x52080, eax=unit)
                port = L.shim_ai_volley(ctypes.c_float(dt), ticks, online, rounds, elapsed, ctypes.byref(lock))
                rep.check(len(launched) == port, 'online %d, %d rounds, %d+%d ticks: DOS %d port %d' %
                          (online, rounds, elapsed, ticks, len(launched), port))
                if online:
                    e.w16(unit + 0x10c, 0x80c0)
                    e.call(0x52ea0, eax=unit)
                    dos_lock = e.r16(unit + 0x10c)
                    rep.check(dos_lock == lock.value, 'without a target: DOS lock %#x, port lock %#x' %
                              (dos_lock, lock.value))

@test
def reticle_nearest_contact(rep, rnd, n):
    """DOS 0x53190 remembers the closest collision object, including solid wreck geometry."""
    L = lib('msim')
    distance = ctypes.c_float()
    for first, second, radius in ((7000, 10000, 270), (10000, 7000, 270), (10030, 10050, 270),
                                  (10050, 10030, 270), (7000, 10000, 250), (10000, 7000, 250)):
        for wreck in (-1, 0, 1):
            e = emu()
            unit, head, parts = e.alloc(0xc0), e.alloc(0x20), (e.alloc(0x60), e.alloc(0x60))
            contacts = dict(zip(parts, (first, second)))
            e.w32(PLAYER, 0)
            e.w32(0x95b44, head)
            e.w32(head + 0x10, parts[0])
            e.w32(parts[0] + 0x10, parts[1])
            for i, part in enumerate(parts):
                # A solid DOS wreck representation keeps the same unit collision flag (0x37381).
                e.w16(part + 2, 0x100)
                e.w16(part + 0x14, i + 1)

            def segment(em):
                ray = em.reg('edx')
                em.w32(ray + 0x30, 150000)
                em.w32(ray + 0x34, 1)

            def clip(em):
                em.w32(em.reg('edx') + 0x30, contacts[em.reg('eax')])
                return 1

            e.stub(0x49b80)
            e.stub(0x532c0, segment)
            e.stub(0x3a745, lambda em: contacts[em.reg('eax')])
            e.stub(0x1c600, clip)
            e.call(0x53190, eax=unit)
            dos = parts.index(e.r32(0x159440))
            port = L.shim_reticle_contact(first, second, radius, wreck, ctypes.byref(distance))
            rep.check(dos == port and abs(e.r32(unit + 0xb0) - distance.value) < 1.0,
                      'ranges %d/%d, wreck %d: DOS %d at %d, port %d at %g' %
                      (first, second, wreck, dos, e.r32(unit + 0xb0), port, distance.value))

@test
def missile_lock_range(rep, rnd, n):
    """Check the DOS missile lock's elevation, inclusive outer range, and timer."""
    L = lib('msim')
    for y, z in ((0, 100100), (0, 100101), (0, 7500), (1300, 7400),
                 (2920, 10000), (0, 7501), (0, 16000)):
        for flags, timer in ((0, 0), (0x8040, 7), (0x8040, 0), (0x80c0, 361)):
            e = emu()
            unit, obj, weapon = e.alloc(0x170), e.alloc(0x200), e.alloc(0x68)
            bearing, slant, horizontal, elevation = (e.alloc(4) for _ in range(4))
            e.call(0x2ab60, eax=0, edx=y, ebx=z, ecx=bearing, stack=(slant, horizontal, elevation))
            e.w32(unit, obj)
            e.w32(unit + 0x54, weapon)  # weapon 0, the LRM 20
            e.w32(unit + 0x8c, timer)
            e.w16(unit + 0x10c, flags)
            e.w32(obj + 0xdc, 0x201)
            e.w32(obj + 0xcc, y)
            e.w32(obj + 0xd0, z)
            e.w32(obj + 0xd4, e.r32(bearing))
            e.w32(obj + 0xd8, e.r32(elevation))
            e.w32(TICKS, 7)
            e.call(0x52ea0, eax=unit)
            port_timer = ctypes.c_int(timer)
            port = L.shim_player_lock(y, z, flags, ctypes.byref(port_timer), 7)
            expected = (e.r16(unit + 0x10c), e.rs32(unit + 0x8c))
            rep.check((port, port_timer.value) == expected, 'y %d z %d initial lock %#x/%d: DOS %s port %s' %
                      (y, z, flags, timer, expected, (port, port_timer.value)))

@test
def dos_lock_window(rep, rnd, n):
    """Compare range and azimuth/elevation boundaries of DOS 0x52ea0."""
    e = emu()
    unit, obj, slot = e.alloc(0x170), e.alloc(0x200), e.alloc(104)
    bearing, slant, horizontal, elevation = (e.alloc(4) for _ in range(4))
    e.w32(unit, obj)
    e.w32(unit + 0x54, slot)
    e.w32(obj + 0xdc, 0x201)
    shim = lib('msim').shim_dos_lock_window
    for weapon in (0, 1, 2, 3, 7, 8, 9, 28):
        e.w32(slot + 4, weapon)
        spec = 0x9ee58 + weapon * 88
        lo, hi = e.rs32(spec + 0x3c), e.rs32(spec + 0x40)
        cases = [(1, 0, z) for z in (lo, hi)] + [(0, 0, z) for z in (lo - 1, lo, lo + 1, hi - 1, hi, hi + 1)]
        cases += [(65536, 0, lo), (65536, 0, hi)]
        cases += [tuple(rnd.randint(-hi, hi) for _ in range(3)) for _ in range(n)]
        cases += [(3000, 1200, -hi // 2), (-3000, -1500, -hi // 2), (0, hi // 2, 0)]
        for x, y, z in cases:
            e.call(0x2ab60, eax=x, edx=y, ebx=z, ecx=bearing, stack=(slant, horizontal, elevation))
            az, el = e.rs32(bearing), e.rs32(elevation)
            for ofs, val in ((0xc8, x), (0xcc, y), (0xd0, z), (0xd4, az), (0xd8, el)):
                e.w32(obj + ofs, val)
            for edge in (0, (16 << 16) - 1, 16 << 16, (16 << 16) + 1):
                for yaw, pitch in ((az + edge, el), (az - edge, el), (az, el + edge), (az, el - edge)):
                    e.w32(obj + 0x60, yaw)
                    e.w32(unit + 0x1c, -pitch)
                    e.w16(unit + 0x10c, 0)
                    e.call(0x52ea0, eax=unit)
                    expected = bool(e.r16(unit + 0x10c) & 0x8000)
                    result = bool(shim(weapon, x, y, z, yaw, pitch))
                    rep.check(expected == result, 'weapon %d delta %s aim %s: DOS %s port %s' %
                              (weapon, (x, y, z), (yaw, pitch), expected, result))

@test
def ai_weapon_pick(rep, rnd, n):
    """DOS 0x2e980..0x2ea63 through the real range, heat, slot and step checks, for stock AI weapon groups."""
    L = lib('combat')
    slot = ctypes.c_int * 3
    rolls = ctypes.c_uint * 10
    L.shim_ai_weapon_pick.argtypes = ([ctypes.c_int] * 3 + [ctypes.c_float, ctypes.POINTER(slot)] +
                                     [ctypes.POINTER(ctypes.c_uint)] * 2 + [ctypes.POINTER(ctypes.c_int)])
    for case in range(n * 20):
        e = emu()
        obj, ctrl, table = e.alloc(0x200), e.alloc(0x200), e.alloc(10 * 104, fill=255)
        count = rnd.randrange(1, 11)
        initial = rnd.randrange(count)
        slots = (slot * count)(*(slot(rnd.randrange(28), rnd.choice([-1, 0, 1, 1, 1, 2, 3]),
                                      rnd.choice([-1, 0, 0, 1, 20, 25])) for i in range(count)))
        first = 0x9ee58 + slots[initial][0] * 88
        distance = rnd.choice([rnd.randrange(120000), e.rs32(first + 0x3c) + rnd.randint(-1, 1),
                               e.rs32(first + 0x40) + rnd.randint(-1, 1)])
        heat = rnd.choice([0, rnd.randrange(65 << 16), (65 << 16) - e.rs32(first + 0x34) + rnd.randint(-4, 4)])
        scripted = rolls(*(rnd.randrange(16) for _ in range(10)))
        bounds = []
        def roll(em):
            bound = em.reg('eax')
            result = scripted[len(bounds)] % bound
            bounds.append(bound)
            return result
        e.stub(RAND, roll)
        e.w32(obj + 0x20, ctrl)
        e.w32(obj + 0xc0, distance)
        e.w32(ctrl + 0x54, table)
        e.w32(ctrl + 0x98, heat)
        e.w32(ctrl + 0xa8, count)
        e.w32(ctrl + 0xac, initial)
        for i, (weapon, state, ammo) in enumerate(slots):
            for offset, value in ((0, 1), (4, weapon), (8, state), (12, 0), (32, ammo)):
                e.w32(table + i * 104 + offset, value)
        e.run(0x2e980, 0x2ea63, eax=obj)
        dos_sel = e.rs32(ctrl + 0xac)
        dos = dos_sel if e.reg('esi') else -1
        port_bounds, port_sel = rolls(), ctypes.c_int()
        port = L.shim_ai_weapon_pick(count, initial, distance, heat / 65536.0, slots, scripted, port_bounds,
                                     ctypes.byref(port_sel))
        actual_bounds = [b for b in port_bounds if b]
        rep.check((dos, dos_sel, bounds) == (port, port_sel.value, actual_bounds),
                  '%d cm heat %.5f slots %s start %d: DOS %s port %s' %
                  (distance, heat / 65536.0, [tuple(s) for s in slots], initial, (dos, dos_sel, bounds),
                   (port, port_sel.value, actual_bounds)))

@test
def jump_jet_fuel(rep, rnd, n):
    """DOS jet controller 0x27ba7..0x27c87: the tank, active thrust and engine heat at six ticks per frame."""
    L = lib('combat')
    result = (ctypes.c_int * 5)()
    for fuel in (-2, 0, 1, 5, 6, 7, 12, 1819, 1820):
        for held in (0, 1):
            for power in (0, 1, 2, 3):
                for infinite in (0, 1):
                    e = emu()
                    ctrl, obj, inputs = e.alloc(0x200), e.alloc(0xa0), e.alloc(0x50)
                    e.w32(ctrl, obj)
                    e.w32(obj + 0x4c, inputs)
                    e.w8(inputs + 0x1d, held)
                    e.w32(ctrl + 0xa0, power)
                    e.w32(ctrl + 0xc0, fuel)
                    e.w32(ctrl + 0xc4, 3)
                    e.w32(TICKS, 6)
                    e.w32(0x961ec, infinite)
                    thrust = []
                    e.stub(0x28ae0, lambda em: thrust.append(True) or 0)  # only observe the effect call
                    e.w32(mw2emu.STACK_TOP - 0x808, 1)  # local at EBP-8: the player
                    e.run(0x27ba7, 0x27c87, esi=ctrl)
                    fired = int(bool(thrust))
                    expected = (e.rs32(ctrl + 0xc0), fired, e.rs32(ctrl + 0x94), fired, fired)
                    L.shim_jet_step(fuel, held, power, infinite, result)
                    rep.check(tuple(result) == expected, 'fuel %d held %d power %d infinite %d: DOS %s port %s' %
                              (fuel, held, power, infinite, expected, tuple(result)))

@test
def ai_nav_turn(rep, rnd, n):
    """MW2.EXE 0x229e0 (the nav steering: the leg-turn demand from the heading error) vs ai_nav_turn"""
    e = emu(); L = lib('ai'); L.shim_nav_turn.restype = ctypes.c_float; L.shim_nav_turn.argtypes = [ctypes.c_float]
    errs = [0, 1, -1, 0xffff, -0xffff, 0x10000, -0x10000, 0x2d0000, 0x2e0000, -0x2d0000, -0x2e0000, 0xb40000, -0xb40000] + \
           [rnd.randint(-0xb40000, 0xb40000) for _ in range(n * 20)]
    c = e.alloc(0x100); inp = e.alloc(0x20); e.w32(c + 0x4c, inp)
    for err in errs:
        e.w32(c + 0xd4, err); e.w32(c + 0x60, 0)
        e.call(0x229e0, eax=c)
        dos = s32(e.r32(inp + 0xc)) / 65536.0
        prt = L.shim_nav_turn(err / 65536.0)
        rep.check(abs(dos - prt) < 1e-3, 'err %.4f deg: dos %.5f port %.5f' % (err / 65536.0, dos, prt))

@test
def leg_turn_demand(rep, rnd, n):
    """MW2.EXE 0x27c87..0x27d70 (the leg turn demand c[0xe], walking: speed / 10000, the spot-turn rule, cos(min(80,
    kv)), airborne) vs msim_leg_demand"""
    e = emu(); L = lib('msim')
    L.msim_leg_demand.restype = ctypes.c_float
    L.msim_leg_demand.argtypes = [ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_float, ctypes.c_int, ctypes.c_int, ctypes.c_float]
    spds = [0, 1, 9999, 10000, 10001, -9999, -10000, -10001, 19999, 800000, 810000, 2000000, -300000, -900000, -1300000, -2400000]
    for k in range(n * 40):
        spd = rnd.choice(spds) if k < len(spds) * 6 else rnd.randint(-400000, 900000)
        inp = rnd.choice([0, 0x3333333, -0x3333333, 0x4000000, -0x4000000, 1023, -1023, 1024, -1025, rnd.randint(-0x4000000, 0x4000000)])
        player = rnd.randint(0, 1); gain = rnd.choice([0, 1, 0x6666, 0x10000]); air = int(rnd.random() < 0.15)
        ap = rnd.randint(0, 1); prev = rnd.randint(-0x500000, 0x500000)
        obj = e.alloc(0x100); ib = e.alloc(0x50); c = e.alloc(0x200)
        e.w32(c, obj); e.w32(obj + 0x4c, ib); e.w32(ib + 0xc, inp); e.w8(obj + 0x80, 4 if air else 0)
        e.w32(c + 0x2c, spd); e.w32(c + 0xb0, gain); e.w32(c + 0xbc, ap); e.w32(c + 0x38, prev)
        ebp = mw2emu.STACK_TOP - 0x800
        e.w32(ebp - 4, 0); e.w32(ebp - 8, player)
        e.run(0x27c87, 0x27d70, esi=c, edi=1, ebp=ebp)
        dos = s32(e.r32(c + 0x38)) / 65536.0
        prt = L.msim_leg_demand(inp / 65536.0, spd / 65536.0 * 182.0, player, gain / 65536.0, air, ap, prev / 65536.0)
        rep.check(abs(dos - prt) < 0.01, 'spd %d input %.2f player %d gain %#x air %d ap %d: dos %.4f port %.4f'
                  % (spd, inp / 65536.0, player, gain, air, ap, dos, prt))
        if k % 50 == 49: e.reset_data()

@test
def damage_shade(rep, rnd, n):
    """Original loader packs the shade scales, and 0x26270 updates the part only from the side struck."""
    e = emu()
    L = lib('combat')
    vec = ctypes.c_int * 3
    L.shim_dos_damage_shade.argtypes = [vec, vec] + [ctypes.c_int] * 5
    obj, control, tree, part, locations = e.alloc(0x100), e.alloc(0x200), e.alloc(0x30), e.alloc(0x90), e.alloc(8 * 40)
    e.w32(control, obj)
    e.w32(control + 0x58, locations)
    e.w32(obj + 0x40, tree)
    e.w32(tree + 0x6c, part)
    e.w32(0xf8a40, obj)
    dif_block(e)
    e.stub(RAND)
    for _ in range(n * 24):
        player = rnd.randrange(2)
        loc = rnd.choice([1, 2, 4, 5, 6, 7, 8])
        rear = rnd.randrange(2)
        side = int(rear != 0 and loc in (2, 4))
        divisor = 4 if player else rnd.choice([1, 3, 4])
        factor = 3 if divisor == 1 and rnd.randrange(2) else divisor
        base = [rnd.randint(0, 110), rnd.randint(0, 34) if loc in (2, 4) else 0, rnd.randint(1, 47)]
        remaining = [rnd.randint(0, base[0] * factor * 256) * 256,
                     rnd.randint(0, base[1] * factor * 256) * 256,
                     rnd.randint(1, base[2] * 256) * 256]
        damage = rnd.randint(1, min(2048, (remaining[side] + remaining[2] - 1) >> 7)) * 128
        prev = rnd.choice([0, 0, 0, 3, 14])
        rec = locations + (loc - 1) * 40
        e.w32(PLAYER, 0)
        e.w32(obj + 4, 0 if player else 1)
        e.w32(0x961d8, 4)
        e.w32(0x961d4, divisor)
        e.w32(0x9ee48, -1)
        e.w16(part, 0x8100 | prev << 4)
        e.w16(part + 0x16, loc)
        for j, value in enumerate(base):
            e.w32(rec + j * 4, value)
        e.w16(rec + 0x26, 0)
        e.run(0x4ca25, 0x4ca8c, ebx=rec)
        for j, value in enumerate(remaining):
            e.w32(rec + j * 4, value)
        e.call(0x26270, eax=control, edx=damage, ebx=loc | (rear << 15))
        original = (e.r16(part) >> 4) & 15
        port = L.shim_dos_damage_shade(vec(*base), vec(*remaining), divisor, prev, damage, rear, loc)
        rep.check(original == port, 'location %d rear %d older shade %d: DOS %d port %d' %
                  (loc, rear, prev, original, port))

@test
def projectile_pool(rep, rnd, n):
    """0x52710 picks the first available slot of the weapon kind, and 0x506f0 visits slots in ascending order."""
    e = emu()
    L = lib('msim')
    L.shim_projectile_pool_new.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
    L.shim_projectile_pool_new.restype = ctypes.c_void_p
    L.shim_projectile_pool_insert.argtypes = [ctypes.c_void_p, ctypes.c_int]
    L.shim_projectile_pool_release.argtypes = [ctypes.c_void_p, ctypes.c_int]
    L.shim_projectile_pool_at.argtypes = [ctypes.c_void_p, ctypes.c_int]
    L.shim_projectile_pool_free.argtypes = [ctypes.c_void_p]
    base, stride = 0x153940, 0x50
    weapon = e.alloc(0x58)
    ebp = mw2emu.STACK_TOP - 0x800
    for _ in range(n):
        kinds = [rnd.randrange(8) for _ in range(rnd.randint(8, 175))]
        for slot in range(175):
            entry = base + slot * stride
            e.w32(entry, kinds[slot] if slot < len(kinds) else -1)
            e.w32(entry + 0x2c, 1 if slot < len(kinds) else 0)
            e.w32(entry + 0x38, 0)
        pool = L.shim_projectile_pool_new((ctypes.c_uint8 * len(kinds))(*kinds), len(kinds))
        active = set()
        try:
            for _ in range(96):
                if active and rnd.randrange(4) == 0:
                    slot = rnd.choice(sorted(active))
                    e.w32(base + slot * stride + 0x38, 0)
                    L.shim_projectile_pool_release(pool, slot)
                    active.remove(slot)
                else:
                    kind = rnd.randrange(10)
                    e.w32(weapon, kind)
                    e.w32(ebp - 4, weapon)
                    e.run(0x52709, 0x5274f, ebp=ebp, ebx=0)
                    original = (e.reg('esi') - base) // stride if e.reg('ebx') else -1
                    port = L.shim_projectile_pool_insert(pool, kind)
                    rep.check(port == original, 'kind %d: DOS slot %d port %d' % (kind, original, port))
                    if original >= 0:
                        e.w32(base + original * stride + 0x38, 1)
                        active.add(original)
                state = [L.shim_projectile_pool_at(pool, i) for i in range(len(active) + 1)]
                rep.check(state == sorted(active) + [-1], 'visit order %s, port %s' % (sorted(active), state))
        finally:
            L.shim_projectile_pool_free(pool)

@test
def dos_collision_lods(rep, rnd, n):
    """Compare DOS distance arithmetic and the two-pass collision representation selector."""
    e = emu()
    L = lib('msim')
    vec = ctypes.c_int32 * 3
    ptr = ctypes.POINTER(ctypes.c_int32)
    L.shim_dos_lods.argtypes = [ptr, ctypes.c_int, vec, ctypes.c_int, ctypes.c_int, ptr]
    cam, yaw, slant, level, pitch = e.alloc(0xbc), *(e.alloc(4) for _ in range(4))
    units = [e.alloc(0x100) for _ in range(24)]
    e.w32(0x9701c, cam)
    def replace_hull(em):
        changed.append(em.reg('eax'))
        return 1

    e.stub(0x37060, replace_hull)
    for _ in range(n * 5):
        count = rnd.randint(1, len(units))
        mode = rnd.randrange(3)  # absent, cockpit, external
        e.w32(PLAYER, 0 if mode else -1)
        e.w32(0x96ee8, int(mode == 2))
        e.w32(0x961c8, count)
        eye = [rnd.randint(-50000, 50000) for _ in range(3)]
        scale = rnd.choice([49152, 65536, 131072, 209715, rnd.randint(48000, 210000)])
        for j, v in enumerate(eye):
            e.w32(cam + j * 4, v)
        e.w32(cam + 0xb8, scale)
        values, expected, changed = [], [], []
        for j, unit in enumerate(units[:count]):
            distance = (rnd.choice([3600, 8500, 22500]) * scale + 32768) >> 16
            distance += rnd.randint(-5, 5)
            x, z = rnd.choice([(0, 0), (0, distance), (-distance, 0), (distance, -distance),
                               (rnd.randint(-distance, distance), rnd.randint(-distance, distance))])
            y = rnd.choice([0, 1, rnd.randint(-8000, 8000)])
            dead = int(rnd.randrange(6) == 0)
            e.w32(0xf8a40 + 4 * j, unit)
            e.w32(unit + 4, j)
            e.w16(unit + 20, 2 * dead)
            prev = rnd.randrange(5 if j == 0 and mode else 4)
            e.w32(unit + 28, prev)
            for axis, v in enumerate([x, y, z]):
                e.w32(unit + 0x50 + 4 * axis, v + eye[axis])
                values.append(v + eye[axis])
            values.extend([dead, prev])
            e.call(0x2ab60, eax=x, edx=y, ebx=z, ecx=yaw, stack=(slant, level, pitch))
            expected.append((e.rs32(level), e.rs32(slant)))
        e.call(0x37580)
        order = list(reversed(changed)) + [j for j in reversed(range(count)) if j not in changed]
        out = (ctypes.c_int32 * (count * 4))()
        L.shim_dos_lods((ctypes.c_int32 * len(values))(*values), count, vec(*eye), scale, mode, out)
        for j, unit in enumerate(units[:count]):
            original = (*expected[j], e.rs32(unit + 28), order[j])
            native = tuple(out[4 * j:4 * j + 4])
            rep.check(original == native, 'camera %s scale %d mode %d unit %s: DOS %s port %s' %
                      (eye, scale, mode, values[5 * j:5 * j + 5], original, native))


@test
def dos_animation_frames(rep, rnd, n):
    """Follow the original track player and its changing node pose, including matrix normalization."""
    e = mw2emu.Emu()
    L = lib('msim')
    ptr = ctypes.POINTER(ctypes.c_int32)
    L.shim_dos_anim_frames.argtypes = [ptr, ctypes.c_int, ptr, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ptr]
    holder, task, unit, root, node, track, deltas, keys = (e.alloc(sz) for sz in (4, 40, 0x180, 128, 128, 20, 256, 512))
    e.w32(root + 4, node)
    e.w32(node, root)
    e.w32(holder, task)
    e.w32(track + 12, deltas)
    e.w32(track + 16, keys)
    e.stub(0x17300)
    e.stub(0x17490, ret=holder)
    def move(_uc, _addr, _size, _data):
        applied[0] = s32(applied[0] + e.reg(('edx', 'ebx', 'ecx')[axis]))

    callbacks = [e.uc.hook_add(UC_HOOK_CODE, move, begin=addr, end=addr)
                 for addr in (0x1dfc0, 0x1e140)]
    # Loop walk or branch to run. A run stop returns through a walk transition marker.
    sample_flags = [
        (1, 0), (2, 0), (0x810, 0), (0x240, 1 << 8), (0x804, 0xffff_ff00), (0x400, 0),
        (1, 1), (0x200, 0), (0x112, 0), (0x808, 0),
    ]

    def cases():
        for _ in range(n):
            kind = rnd.randrange(6)
            limit = 25000000 if kind >= 3 else 800
            yield kind, [rnd.randint(-limit, limit) for _ in sample_flags], sample_flags, False
        fixture = os.getenv('MW2_ANIM_RECORD_DIR')
        if fixture:
            record_count = 0
            for asset in sorted(Path(fixture).iterdir()):
                data = asset.read_bytes()
                count, keys = struct.unpack_from('<ii', data)
                if not 1 <= keys <= 64 or not 1 <= count:
                    raise ValueError('invalid animation dimensions in %s' % asset.name)
                stride = 8 + 4*keys
                flags = list(struct.iter_unpack('<II', data[8 + count*stride:8 + count*stride + 8*keys]))
                record_count += 1
                first_track = None
                moving = False
                for index in range(count):
                    offset = 8 + index*stride
                    _, kind = struct.unpack_from('<ii', data, offset)
                    ds = struct.unpack_from(f'<{keys}i', data, offset + 8)
                    track_case = (kind, ds, flags, True)
                    if first_track is None:
                        first_track = track_case
                    if any(ds):
                        moving = True
                        yield track_case
                if not moving:
                    yield first_track
            if record_count == 0:
                raise ValueError('MW2_ANIM_RECORD_DIR contains no gait records')
            rep.notes.append('replayed gait and vehicle task tracks in all %d DOS animation records' % record_count)

    def as_array(values):
        return (ctypes.c_int32 * len(values))(*values)

    for type_id, ds, flags, asset in cases():
        rate, now = rnd.choice([37, 45, 70]), 10000
        axis = type_id % 3
        e.w32(track + 8, type_id)
        timeline = []
        for k in range(240):
            req = 0 if k < 35 or 130 <= k < 210 else 1 if k < 110 or k >= 210 else -1
            if asset:
                req = (0, 1, -1, 2, 0, 2, 1, -1)[k // 30]
            gait = 0 if req < 0 else 3 if req == 1 else rnd.choice([1, 2])
            timeline.append((req, gait, rnd.choice([0, 5, 6, 7, 13, 39, 95]), int(rnd.randrange(10) == 0)))
        out = (ctypes.c_int32 * (22 * len(timeline)))()
        phase = rnd.randint(64, 191)
        native = as_array([v for d, (flags_word, attrs) in zip(ds, flags) for v in (d, flags_word, s32(attrs))])
        L.shim_dos_anim_frames(native, len(ds), as_array([v for frame in timeline for v in frame]),
                              len(timeline), rate, type_id, phase, out)
        e.w32(root + 4, 0)
        e.w32(0x95ae4, phase)
        e.call(0x1de60, eax=root, edx=node)
        e.write(task, bytes(40))
        for off, v in [(0, track), (4, node), (8, unit), (12, rate), (24, now), (28, 1), (32, -1), (36, 1)]:
            e.w32(task + off, v)
        for off, v in [(128, 0), (132, -1), (148, rate)]:
            e.w32(unit + off, v)
        e.w32(track + 4, len(ds))
        for j, (delta, (flags_word, attrs)) in enumerate(zip(ds, flags)):
            e.w32(deltas + j * 4, delta)
            e.w32(keys + j * 8, flags_word)
            e.w32(keys + j * 8 + 4, attrs)
        applied = [0]
        for j, (req, gait, elapsed, hold) in enumerate(timeline):
            now += elapsed
            e.w32(unit + 128, e.r32(unit + 128) & (0 if hold else 1))
            e.w32(unit + 136, req)
            e.w32(unit + 140, gait)
            heading = (j % 37 - 18) * 21 * 16384
            e.call(0x3b88b, stack=(root + 12, 0, heading, 0, -10000 + 82*j, 400, 28000 - 49*j))
            e.call(0x1aad0, eax=1, ebx=now)
            e.call(0x1e400, eax=root)
            fl = e.r32(unit + 128)
            ev = int(bool(fl & 8)) | (2 * int(bool(fl & 2)))
            expected = [fl & 1, e.rs32(task + 32), e.rs32(unit + 132), e.rs32(task + 16),
                        e.rs32(unit + 148), applied[0], e.rs32(task + 20), ev]
            expected.extend(e.rs32(node + 60 + 4*k) for k in range(12))
            expected.extend((e.r32(node + 120) if type_id >= 3 else 0, e.r32(0x95ae4)))
            got = list(out[22 * j:22 * j + 22])
            rep.check(expected == got, 'track %d step %d req %d gait %d elapsed %d hold %d: DOS %s port %s' %
                      (type_id, j, req, gait, elapsed, hold, expected, got))
    for callback in callbacks:
        e.uc.hook_del(callback)


@test
def dos_skeletal_pose(rep, rnd, n):
    """Run a three-piece pose through the DOS node hierarchy and compare its final collision point."""
    e = emu()
    L = lib('msim')
    v3, v4, v9, v39 = (ctypes.c_int32 * k for k in (3, 4, 9, 39))
    L.shim_dos_skeleton.argtypes = [ctypes.c_int32, v4, v3, v9, v3, ctypes.c_int, ctypes.c_int, ctypes.c_int, v39]
    carrier, body, torso, gun = (e.alloc(128) for _ in range(4))
    point_addr = e.alloc(12)
    out = v39()
    for parent, child in [(carrier, body), (body, torso), (torso, gun)]:
        e.w32(parent + 4, child)
        e.w32(child, parent)
    for _ in range(n * 12):
        # The samples fit the float interface without losing 16.16 angle bits.
        angle = lambda: rnd.choice([0, 45 * 65536, -90 * 65536, rnd.randint(-46080, 46080) * 256])
        heading, angles = angle(), [angle() for _ in range(4)]
        origin = [rnd.randint(-150000, 150000) for _ in range(3)]
        offsets = [[rnd.randint(-1200, 1200) for _ in range(3)] for _ in range(3)]
        point = [rnd.randint(-650, 650) for _ in range(3)]
        translation, quarter = rnd.randint(-100, 100) * 4, rnd.randrange(5)
        gun_offset = offsets[2].copy()
        gun_offset[1] += translation * quarter // 4
        e.call(0x3b88b, stack=(carrier + 12, 0, heading, 0, *origin))
        e.call(0x3b88b, stack=(body + 12, 0, 0, angles[3] * quarter // 4, *offsets[0]))
        e.call(0x3b88b, stack=(torso + 12, angles[0], angles[1], 0, *offsets[1]))
        e.call(0x3b88b, stack=(gun + 12, angles[2], 0, 0, *gun_offset))
        e.call(0x1e400, eax=carrier)
        expected = [e.rs32(node + 60 + j * 4) for node in (body, torso, gun) for j in range(12)]
        for j, v in enumerate(point):
            e.w32(point_addr + 4 * j, v)
        e.call(0x3aefc, eax=gun + 60, edx=point_addr, ecx=point_addr + 4, stack=(point_addr + 8,))
        expected.extend(e.rs32(point_addr + 4 * j) for j in range(3))
        for live_sample in (0, 1):
            L.shim_dos_skeleton(heading, v4(*angles), v3(*origin), v9(*(v for row in offsets for v in row)),
                                v3(*point), translation, quarter, live_sample, out)
            rep.check(expected == list(out), 'heading %d angles %s key %d/4 live sample %d: DOS %s port %s' %
                      (heading, angles, quarter, live_sample, expected, list(out)))


@test
def dos_obstacle_probe(rep, rnd, n):
    """DOS 0x2df30 starts a side probe off-centre while aiming at a point relative to the hull."""
    e = emu()
    L = lib('msim')
    v3, v6 = ctypes.c_int32 * 3, ctypes.c_int32 * 6
    out = v6()
    L.shim_dos_probe_points.argtypes = [v3, ctypes.c_float] + [ctypes.c_int] * 4 + [v6]
    unit, control, matrix, segment = e.alloc(0x210), e.alloc(0x200), e.alloc(0x30), e.alloc(0x38)
    e.w32(unit + 0x20, control)
    e.stub(0x1e540, ret=matrix)
    for _ in range(n * 12):
        origin = [rnd.randint(-200000, 200000) for _ in range(3)]
        heading = ctypes.c_float(rnd.choice([0, 45, 90, 180, -90, rnd.uniform(-180, 180)])).value
        e.call(0x3b88b, stack=(matrix, 0, round(heading * 65536), 0, *origin))
        for j, value in enumerate(origin):
            e.w32(unit + 0x50 + 4 * j, value)
        step, side = rnd.randrange(4), rnd.choice([-1, 1])
        length, lateral = rnd.randint(1499, 15000), rnd.choice([0, rnd.randint(50, 1000)])
        e.w32(control + 0xe8, lateral + 1)
        e.call(0x2df30, eax=unit, edx=segment, ebx=side, ecx=step, stack=(length, int(lateral > 0)))
        original = [e.rs32(segment + 4 * j) for j in range(6)]
        L.shim_dos_probe_points(v3(*origin), heading, step, side, length, lateral, out)
        rep.check(list(out) == original,
                  'probe %d side %d length %d lateral %d: DOS %s port %s' %
                  (step, side, length, lateral, original, list(out)))

@test
def dos_hull_bounds(rep, rnd, n):
    """Execute the original collision-sphere builder on integer hull vertices."""
    e = emu()
    e.uc.reg_write(UC_X86_REG_FPCW, e.r16(0xa4520))
    L = lib('msim')
    out = (ctypes.c_int32 * 4)()
    L.shim_dos_hull_bounds.argtypes = [ctypes.POINTER(ctypes.c_int32), ctypes.c_int, type(out)]
    obj, model = e.alloc(0x80), e.alloc(24 + 44 * 64)
    e.w32(obj + 28, model)
    for _ in range(n * 10):
        count = rnd.choice([1, 2, 3, 16, 24, 64])
        points = [rnd.randint(-14000, 14000) for _ in range(count * 3)]
        e.w16(model + 4, count)
        for j in range(count):
            for axis in range(3):
                e.w32(model + 24 + j * 44 + 12 + axis * 4, points[j * 3 + axis])
        e.call(0x37f60, eax=obj)
        original = [e.rs32(obj + 52 + axis * 4) for axis in range(4)]
        L.shim_dos_hull_bounds((ctypes.c_int32 * len(points))(*points), count, out)
        rep.check(list(out) == original, '%d vertices: DOS %s port %s' % (count, original, list(out)))

@test
def projectile_contact_math(rep, rnd, n):
    """DOS 0x1d960/0x1d9f0 prepares the segment, 0x3a745 tests its sphere, 0x1ec60 clips against a box."""
    e = emu()
    e.uc.reg_write(UC_X86_REG_FPCW, e.r16(0xa4520))  # Watcom startup at 0x78159
    L = lib('msim')
    vec, vec4, vec6 = (ctypes.c_int32 * k for k in (3, 4, 6))
    out = (ctypes.c_int32 * 9)()
    L.shim_dos_contact_math.argtypes = [vec, vec, vec4, vec6, type(out)]
    obj, segment, box, frame = e.alloc(0x60), e.alloc(0x38), e.alloc(24), e.alloc(0x80)
    e.w16(obj, 0x200)
    e.w32(obj + 0x44, box)
    for _ in range(n * 30):
        p = [rnd.randint(-25000, 25000) for _ in range(3)]
        delta = [rnd.choice([0, 0, 1, -1, rnd.randint(-8000, 8000)]) for _ in range(3)]
        if not any(delta):
            delta[2] = 1
        end = [p[i] + delta[i] for i in range(3)]
        scale = rnd.choice([-0.5, 0, 0.1, 0.5, 0.9, 1.4])
        radius = rnd.randint(50, 3000)
        center = [p[i] + round(delta[i] * scale) + rnd.randint(-radius, radius) for i in range(3)]
        bounds = [v for c in center for v in (c - radius // 2, c + radius // 2)]
        for i, value in enumerate(center + [radius]):
            e.w32(obj + 0x34 + 4 * i, value)
        for i, value in enumerate(bounds):
            e.w32(box + 4 * i, value)
        e.call(0x1d840, eax=segment, edx=p[0], ebx=p[1], ecx=p[2], stack=end)
        octlen = (3 * max(map(abs, delta)) + sum(map(abs, delta))) // 4
        e.call(0x1d9f0 if octlen > 3000 else 0x1d960, eax=segment)
        length = e.rs32(segment + 0x30)
        direction = [e.rs32(segment + 0x24 + 4 * i) for i in range(3)]
        s = e.call(0x3a745, eax=obj, edx=segment)
        hit = e.call(0x1ec60, eax=obj, edx=segment)
        d = e.rs32(segment + 0x30) if hit else 0x7fffffff
        for i, offset in enumerate((0x76, 0x7a, 0x72)):
            e.w32(frame + offset, p[i])
            e.w32(frame + 0x1e + 4 * i, direction[i])
        e.run(0x5093c, 0x50984, ebp=frame, ebx=direction[0])
        # Only the unit bullet effect uses the backed X and Z (0x510c1).
        backed = [e.rs32(frame + 0x66), p[1], e.rs32(frame + 0x62)]
        L.shim_dos_contact_math(vec(*p), vec(*delta), vec4(*center, radius), vec6(*bounds), out)
        dos = [length] + direction + [s, d] + backed
        rep.check(list(out) == dos, 'start %s step %s, sphere %s radius %d: DOS %s port %s' %
                  (p, delta, center, radius, dos, list(out)))

@test
def projectile_obstruction(rep, rnd, n):
    """DOS 0x1c3f0 keeps the first list entry at equal hit distances, and the nearest box otherwise."""
    e = emu()
    L = lib('msim')
    L.shim_dos_projectile_obstruction.argtypes = [ctypes.c_int] * 3
    L.shim_dos_projectile_terrain.argtypes = [ctypes.c_int]
    head, segment, hit = e.alloc(32), e.alloc(0x38), e.alloc(4)
    obj = [e.alloc(0x60), e.alloc(0x60)]
    box = [e.alloc(24), e.alloc(24)]
    e.w32(0x95b44, head)
    for i in range(2):
        e.w16(obj[i], 0x200)  # the box bounds are already cached
        e.w32(obj[i] + 0x44, box[i])
    for _ in range(n * 8):
        first = rnd.randint(100, 2900)
        second = first if rnd.randrange(2) else rnd.randint(100, 2900)
        order = rnd.randrange(2)
        for i, z in enumerate((first, second)):
            for j, value in enumerate((-100, 100, 100, 300, z, z + 200)):
                e.w32(box[i] + 4 * j, value)
            for j, value in enumerate((0, 200, z + 100, 174)):
                e.w32(obj[i] + 0x34 + 4 * j, value)
        e.w32(head + 0x10, obj[order])
        e.w32(obj[order] + 0x10, obj[1 - order])
        e.w32(obj[1 - order] + 0x10, 0)
        e.call(0x1d840, eax=segment, edx=0, ebx=200, ecx=0, stack=(0, 200, 2400))
        e.call(0x1c3f0, eax=segment, edx=hit, ebx=0)
        target = e.r32(hit)
        original = obj.index(target) if target else -1
        port = L.shim_dos_projectile_obstruction(first, second, order)
        rep.check(original == port, 'boxes at %d and %d, newest %d: DOS %d port %d' %
                  (first, second, order, original, port))
    rep.check(L.shim_dos_projectile_terrain(0) == 1 and L.shim_dos_projectile_terrain(1) == 0,
              'a terrain face above zero blocks the shot even when its endpoint is beyond the face')

@test
def dos_mixed_contact(rep, rnd, n):
    """The DOS list resolves scenery and hulls together, with the first eligible member retaining a tie."""
    e = emu()
    L = lib('msim')
    out = (ctypes.c_int32 * 4)()
    L.shim_dos_mixed_contact.argtypes = [ctypes.c_int] * 4 + [type(out)]
    head, segment, hit = e.alloc(32), e.alloc(0x38), e.alloc(4)
    world, leg, arm = [e.alloc(0x60) for _ in range(3)]
    box = e.alloc(24)
    e.w32(0x95b44, head)
    e.w16(world, 0x200)
    e.w32(world + 0x44, box)
    for part, group in ((leg, 4), (arm, 6)):
        e.w16(part + 2, 0x100)
        e.w16(part + 0x14, 7)
        e.w16(part + 0x16, group)
        e.w32(part + 0x24, 6)
    for _ in range(n * 8):
        z = rnd.randint(-150, 2800)
        unit_z = z + 100 + rnd.choice([0, 0, -30, 30, -150, 150, rnd.randint(-1000, 1000)])
        world_first, skip_unit = rnd.randrange(2), rnd.randrange(2)
        for j, value in enumerate((-100, 100, 100, 300, z, z + 200)):
            e.w32(box + 4 * j, value)
        for obj, centre, radius in ((world, z + 100, 174), (leg, unit_z, 100), (arm, unit_z, 100)):
            for j, value in enumerate((0, 200, centre, radius)):
                e.w32(obj + 0x34 + 4 * j, value)
        order = [world, arm, leg] if world_first else [arm, leg, world]
        for a, b in zip([head] + order, order + [0]):
            e.w32(a + 0x10, b)
        e.call(0x1d840, eax=segment, edx=0, ebx=200, ecx=0, stack=(0, 200, 2400))
        original_hit = e.call(0x1c3f0, eax=segment, edx=hit, ebx=7 if skip_unit else 0)
        obj = e.r32(hit) if original_hit else 0
        original = [1 if obj == world else 2 if obj else 0,
                    e.r16(obj + 0x16) if obj in (leg, arm) else 0,
                    e.rs32(segment + 16) if obj else 0,
                    e.rs32(segment + 20) if obj else 0]
        L.shim_dos_mixed_contact(z, unit_z, world_first, skip_unit, out)
        rep.check(list(out) == original, 'box %d sphere %d first %d skip %d: DOS %s port %s' %
                  (z, unit_z, world_first, skip_unit, original, list(out)))

@test
def projectile_launch(rep, rnd, n):
    """DOS 0x527dc launch direction, 0x52806 gravity and 0x53410 missile muzzle offset, including random calls."""
    e = emu()
    L = lib('msim')
    vec = ctypes.c_int32 * 3
    out = (ctypes.c_int32 * 7)()
    L.shim_dos_projectile_launch.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_int32, vec, vec, type(out)]
    L.shim_dos_projectile_launch.restype = ctypes.c_uint
    work = e.alloc(12)
    controller = e.alloc(0x100)
    model = e.alloc(0x80)
    planet, shot = e.alloc(32), e.alloc(0x50)
    e.w32(model + 0x6c, e.alloc(0x80))
    calls, muzzle = [], []
    def mount_position(em):
        for reg, value in zip(('edx', 'ebx', 'ecx'), start):
            em.w32(em.reg(reg), value)
    def move_model(em):
        muzzle[:] = [s32(em.reg(r)) for r in ('edx', 'ebx', 'ecx')]
    def roll(em):
        calls.append(em.reg('eax'))
        return sample % 200
    e.stub(RAND, roll)
    e.stub(0x1e100, pops=4)
    e.stub(0x1df60, mount_position)
    e.stub(0x1dfa0, move_model)
    e.stub(0x1dfc0)
    for _ in range(n * 10):
        weapon, speed, missile = rnd.choice([(0, 100, 1), (7, 70, 1), (22, 280, 0), (29, 600, 0)])
        start = [rnd.randint(-100000, 100000) for _ in range(3)]
        aim = [pos + rnd.randint(-10000, 10000) for pos in start]
        if aim == start:
            aim[2] += 1
        sample = rnd.randrange(32768)
        for i in range(3):
            e.w32(work + 4 * i, aim[i] - start[i])
        e.call(0x3fa50, eax=work, edx=work + 4, ebx=work + 8)
        d = [e.rs32(work + 4 * i) for i in range(3)]
        calls.clear()
        e.call(0x53410, eax=controller, edx=model, ebx=d[0], ecx=d[1], stack=(d[2], missile))
        gravity = rnd.randint(20000, 200000)
        e.w32(planet + 8, gravity)
        e.run(0x4e62e, 0x4e644, ecx=planet)
        e.run(0x52806, 0x5282c, ebx=0x9ee58 + weapon * 0x58, esi=shot)
        port_draws = L.shim_dos_projectile_launch(weapon, sample, gravity, vec(*start), vec(*aim), out)
        rep.check(list(out[:3]) == muzzle and list(out[3:6]) == [v * speed for v in d] and
                  out[6] == e.rs32(shot + 0x18) and
                  calls == ([200] if missile else []) and port_draws == missile,
                  'weapon %d: DOS muzzle %s gravity %d, port %s gravity %d' %
                  (weapon, muzzle, e.rs32(shot + 0x18), out[:3], out[6]))

@test
def projectile_guidance(rep, rnd, n):
    """DOS 0x50d50 guidance toward a unit and its 100 cm proximity test."""
    e = emu()
    L = lib('msim')
    vec = ctypes.c_int32 * 3
    acc = vec()
    L.shim_dos_projectile_guide.argtypes = [vec, vec, vec]
    shot, unit = e.alloc(0x50), e.alloc(0x100)
    e.w32(shot + 0x34, 0x200)
    e.w32(0xf8a40, unit)
    e.stub(0x1e100, pops=4)
    for _ in range(n * 10):
        vel = [rnd.randint(-100 * 65536, 100 * 65536) for _ in range(3)]
        span = rnd.choice([40, 100, 180, 100000])
        delta = [rnd.randint(-span, span) for _ in range(3)]
        if not any(delta):
            delta[2] = 1
        for i in range(3):
            e.w32(shot + 8 + i * 4, vel[i])
            e.w32(unit + 0x50 + i * 4, delta[i])
        e.w32(shot + 0x38, 0)
        e.call(0x50d50, eax=shot)
        dos = [e.rs32(shot + 0x14 + i * 4) for i in range(3)]
        prox = L.shim_dos_projectile_guide(vec(*vel), vec(*delta), acc)
        rep.check(list(acc) == dos and bool(e.r32(shot + 0x38) & 0x8000) == bool(prox),
                  'delta %s: DOS %s port %s' % (delta, dos, list(acc)))

@test
def projectile_frame(rep, rnd, n):
    """0x50750 flies one fixed-point segment over all frame ticks, checks collisions, then retires on expiry."""
    e = emu()
    L = lib('msim')
    vec = ctypes.c_int32 * 3
    out = (ctypes.c_int32 * 6)()
    L.shim_dos_projectile_frame.argtypes = [ctypes.c_int, ctypes.c_int, vec, vec, vec, type(out)]
    slot = 0x153940
    model = e.alloc(16)
    end = []
    def model_position(em):
        for reg, i in (('edx', 0), ('ebx', 1), ('ecx', 2)):
            em.w32(em.reg(reg), pos[i])
    def move_model(em):
        end[:] = [s32(em.reg(r)) for r in ('edx', 'ebx', 'ecx')]
    def expire(em):
        end[:] = [s32(em.arg(i)) for i in (1, 2, 3)]
    e.stub(0x1df60, model_position)
    e.stub(0x1d840, pops=12)
    e.stub(0x1d960)
    e.stub(0x1d9f0)
    e.stub(0x1c3f0)
    e.stub(0x1dfa0, move_model)
    e.stub(0x1e480)
    e.stub(0x50f20, expire, pops=16)
    for _ in range(n * 16):
        ticks = rnd.choice([1, 3, 6, 7, 12, 24, 48])
        life = ticks + rnd.choice([-ticks, -1, 0, 1, 180])
        pos = [rnd.randint(-250000, 250000), 50000, rnd.randint(-250000, 250000)]
        vel = [rnd.randint(-600 * 65536, 600 * 65536) for _ in range(3)]
        acc = [rnd.randint(-16000, 16000) for _ in range(3)]
        for i in range(3):
            e.w32(slot + 8 + i * 4, vel[i])
            e.w32(slot + 20 + i * 4, acc[i])
        e.w32(slot + 0x24, life - ticks)
        e.w32(slot + 0x2c, model)
        e.w32(TICKS, ticks)
        end.clear()
        e.call(0x50750)
        alive = L.shim_dos_projectile_frame(ticks, life, vec(*pos), vec(*vel), vec(*acc), out)
        rep.check(alive == (life > ticks) and list(out[:3]) == end and
                  (not alive or list(out[3:]) == [e.rs32(slot + 8 + i * 4) for i in range(3)]),
                  'ticks %d life %d: DOS end %s, port end %s alive %d' % (ticks, life, end, out[:3], alive))

@test
def servo_step(rep, rnd, n):
    """MW2.EXE 0x49b80 (the integer servo: value += (target - value) x elapsed / T, the target once reached or passed)
    vs the DOS aim servo over random frame sequences, T 36 / 54 / 109 / 145"""
    L = lib('msim')
    L.msim_dos_aim_servo.restype = ctypes.c_float
    L.msim_dos_aim_servo.argtypes = [ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_int]
    for case in range(n * 4):
        e = emu()
        T = rnd.choice([36, 54, 109, 145])
        sv = e.alloc(16); now = 1000
        v0 = rnd.randint(-0x5a0000, 0x5a0000)
        e.w32(sv, now); e.w32(sv + 8, v0); e.w32(sv + 12, T)
        pv = v0 / 65536.0; worst = 0
        for f in range(60):
            if f % 15 == 0: tgt = rnd.randint(-0x5a0000, 0x5a0000); e.w32(sv + 4, tgt)
            t = rnd.choice([0, 1, 3, 4, 6, 7, 9, 14, 20, T])
            now += t; e.w32(NOW, now)
            e.call(0x49b80, eax=sv)
            pv = L.msim_dos_aim_servo(pv, tgt / 65536.0, T, t)
            worst = max(worst, abs(e.rs32(sv + 8) / 65536.0 - pv))
        rep.check(worst == 0, 'T %d: worst |dos - port| %.8f' % (T, worst))

@test
def dos_leg_heading(rep, rnd, n):
    """Compare a multi-frame leg servo and the signed whole-tick heading increment."""
    e = emu()
    L = lib('msim')
    fp = ctypes.POINTER(ctypes.c_float)
    L.msim_dos_leg_step.argtypes = [fp, ctypes.c_float, fp, ctypes.c_int]
    L.msim_dos_leg_step.restype = ctypes.c_float
    control, unit = e.alloc(0x100), e.alloc(0xa0)
    frame = mw2emu.STACK_TOP - 0x800
    e.w32(frame + 126, control)
    e.w32(control, unit)
    for _ in range(n):
        now = 10000
        initial = rnd.randrange(-180 * 65536, 180 * 65536)
        e.w32(unit + 0x60, initial)
        e.w32(control + 52, now)
        e.w32(control + 60, 0)
        e.w32(control + 64, 54)
        angle, servo = ctypes.c_float(initial / 65536), ctypes.c_float()
        for i in range(90):
            if i % 10 == 0:
                target = rnd.randint(-180 * 65536, 180 * 65536)
                e.w32(control + 56, target)
            ticks = rnd.choice([0, 1, 3, 6, 7, 11, 20, 54, 90])
            now += ticks
            e.w32(NOW, now)
            e.w32(TICKS, ticks)
            e.call(0x49b80, eax=control + 52)
            e.run(0x27558, 0x27592)
            orig_angle = e.rs32(unit + 0x60)
            if orig_angle > 180 * 65536:
                orig_angle -= 360 * 65536
            if orig_angle < -180 * 65536:
                orig_angle += 360 * 65536
            L.msim_dos_leg_step(ctypes.byref(servo), target / 65536, ctypes.byref(angle), ticks)
            want = (e.rs32(control + 60), orig_angle)
            got = (round(servo.value * 65536), round(angle.value * 65536))
            rep.check(want == got, 'frame %d %d ticks, target %d: DOS %s port %s' % (i, ticks, target, want, got))


@test
def dos_random_stream(rep, rnd, n):
    """Check the initialized 127-entry table and its wrap with the executable's own rand."""
    e = emu()
    e.call(0x49fe0, eax=0)
    count = 2 * 127 + 16
    words = ctypes.c_uint * count
    limits = words(*(rnd.choice((1, 2, 6, 10, 364, 32768)) for _ in range(count)))
    got = words()
    shim = lib('msim').shim_dos_random
    shim.argtypes = [ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint), ctypes.c_int]
    shim.restype = None
    shim(limits, got, count)
    for i, limit in enumerate(limits):
        want = e.call(RAND, eax=limit)
        rep.check(want == got[i], 'draw %d, limit %d: DOS %d port %d' % (i, limit, want, got[i]))


@test
def dos_contact_controller_state(rep, rnd, n):
    """The movement contact scan runs only for the online or wreck controller states."""
    e = emu()
    unit, ctrl, other, hit = (e.alloc(0x200) for _ in range(4))
    position = tuple(e.alloc(4) for _ in range(3))
    e.w32(ctrl, unit)
    reached = []

    def overlap(em):
        reached.append(True)
        em.w32(em.arg(0), other)
        return 1

    e.stub(0x200d0, overlap, pops=4)
    shim = lib('msim').shim_dos_contact_state
    for state in range(6):
        reached.clear()
        e.w32(ctrl + 0xa0, state)
        e.call(0x1fca0, eax=ctrl, edx=hit + 4, ebx=hit, ecx=100, stack=(0, 0, *position))
        for idle_target in (0, 1):
            actual = bool(shim(state, idle_target))
            rep.check(bool(reached) == actual,
                      'controller %d, target idle %d: DOS scan %s, port hit %s' %
                      (state, idle_target, bool(reached), actual))

@test
def dos_simultaneous_unit_contacts(rep, rnd, n):
    """Overlapping movement spheres resolve in DOS unit-list order, including reciprocal hits."""
    # Keep the complete scan's translated blocks apart from the bounce subroutine test.
    e = mw2emu.Emu()
    objects = [e.alloc(0x100) for _ in range(3)]
    units = [e.alloc(0x180) for _ in range(3)]
    target = e.alloc(4)
    attempted = [e.alloc(4) for _ in range(3)]
    e.w32(0x961c8, 5)
    for slot, (obj, unit) in enumerate(zip(objects, units), 2):
        e.w32(0xf8a40 + slot*4, obj)
        e.w32(obj + 4, slot)
        e.w32(obj + 32, unit)
        e.w32(unit, obj)
    shim = lib('msim').shim_dos_contact_order
    for _ in range(n * 8):
        mover = rnd.randrange(3)
        candidates = [i for i in range(3) if i != mover]
        bodies = [[0, 550, 0, 0, 0, rnd.randint(100, 450),
                   *[rnd.randint(-7*65536, 7*65536) for _ in range(3)]] for _ in range(3)]
        bodies[candidates[0]][0] = rnd.choice((0, 300, 800))
        bodies[candidates[1]][2] = rnd.choice((0, 50, 350))
        if rnd.randrange(3) == 0:
            bodies[candidates[0]][4] = mover + 2
        if rnd.randrange(3) == 0:
            bodies[candidates[0]][3] = 1
        position = [rnd.randint(-80, 80), 550, rnd.randint(-80, 80)]
        for body, obj, unit in zip(bodies, objects, units):
            for k in range(3):
                e.w32(obj + 80 + 4*k, body[k])
                e.w32(unit + 256 + 4*k, body[6 + k])
            e.w32(obj + 124, body[4])
            e.w32(unit + 232, body[5])
            e.w32(unit + 268, 0x100 if body[3] else 0)
        for ptr, value in zip(attempted, position):
            e.w32(ptr, value)
        e.w32(target, 0)
        hit = e.call(0x200d0, eax=units[mover], edx=attempted[0], ebx=attempted[1],
                     ecx=attempted[2], stack=(target,))
        native = [objects.index(e.r32(target)) + 2 if hit else 0]
        if hit:
            native += [e.rs32(ptr) for ptr in attempted]
            native += [e.rs32(units[mover] + 244 + 4*k) for k in range(3)]
        out = (ctypes.c_int32 * 7)()
        shim(mover, (ctypes.c_int32 * 27)(*(value for body in bodies for value in body)),
             (ctypes.c_int32 * 3)(*position), out)
        rep.check(native == list(out)[:len(native)],
                  'mover %d, bodies %s: DOS %s, port %s' % (mover, bodies, native, list(out)))


@test
def dos_positional_sound_draws(rep, rnd, n):
    """The one-shot pitch draw precedes sample allocation and the loop uses the returned rate."""
    e = emu()
    e.call(0x49fe0, eax=0)
    listener = e.alloc(16)
    e.w32(0x9701c, listener)
    e.w32(0x957ac, 0x168)
    played = []
    # 0x40c90 draws the pitch even when the mixer cannot allocate a channel.
    def play(em):
        played.append((em.arg(0), em.reg('edx'), em.reg('eax')))
        return -1
    e.stub(0x409b0, play, pops=24)
    shim = lib('msim')
    index = ctypes.c_uint()
    examples = [(0, 0, z) for z in (0, 1, 49899, 49900, 49999, 50000, 50001)]
    examples += [(0, y, 0) for y in (-25000, -1, 1, 25000)]
    examples += [tuple(rnd.randint(-55000, 55000) for _ in range(3)) for _ in range(n * 12)]
    for x, y, z in examples:
        yaw, half = rnd.randint(-180 << 16, 180 << 16), rnd.randrange(2)
        e.w32(listener + 12, yaw)
        played.clear()
        e.call(0x40bc0, eax=x, edx=y, ebx=z, ecx=0xdb, stack=(half,))
        out = (ctypes.c_int * 3)()
        shim.shim_dos_sound((ctypes.c_int32 * 5)(x, y, z, yaw, half), ctypes.byref(index), out)
        native_index = e.r32(0x9e8f4)
        native = (played[0][0], e.call(0x40d10, eax=played[0][1]), played[0][2]) if played else (0, 0, 0)
        port = tuple(out)
        rep.check(native == port and native_index == index.value,
                  'position %s half %d: DOS volume/pan/delay %s cursor %d port %s cursor %d' %
                  ((x, y, z), half, native, native_index, port, index.value))
        native_rate = e.call(0x40d50)
        port_rate = shim.shim_dos_sound_pitch(ctypes.byref(index))
        rep.check((native_rate, e.r32(0x9e8f4)) == (port_rate, index.value),
                  'rate %d vs %d after position %s' % (native_rate, port_rate, (x, y, z)))

@test
def dos_startup_sound_order(rep, rnd, n):
    """The startup timer draw is followed by a pitch draw only within hearing range."""
    e = emu()
    e.call(0x49fe0, eax=0)
    control, unit, listener = e.alloc(0x170), e.alloc(0x180), e.alloc(16)
    e.w32(control, unit)
    e.w32(0x9701c, listener)
    e.w32(0x957ac, 0x168)
    e.stub(0x409b0, lambda em: -1, pops=24)
    port = lib('msim').shim_dos_startup
    port.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint)]
    index = ctypes.c_uint()
    distances = [0, 49999, 50000, 50001] * n
    for tick, distance in enumerate(distances, 171):
        e.w32(NOW, tick)
        e.run(0x2817c, 0x281aa, esi=control)
        delay = e.r32(control + 0x8c) - tick + 1
        e.call(0x40cf0, eax=0, edx=0, ebx=distance, ecx=0xf7, stack=(0,))
        ours = port(distance, ctypes.byref(index))
        cursor = e.r32(0x9e8f4)
        rep.check((ours, index.value) == (delay, cursor),
                  'range %d: original delay/cursor %s, port %s' % (distance, (delay, cursor), (ours, index.value)))

@test
def dos_detached_part_uses_current_pose(rep, rnd, n):
    """A limb detaches after the root has moved and turned in the same update."""
    detach = lib('msim').shim_dos_detached_pose
    for _ in range(n):
        x, y, z = (rnd.randint(-20000, 20000) for _ in range(3))
        out = (ctypes.c_int32 * 5)()
        count = detach((ctypes.c_int32 * 3)(x, y, z), out)
        rep.check(count == 1 and list(out[:3]) == [x + 200, y, z] and
                  abs(out[3] - (1 << 29)) <= 2 and abs(out[4]) <= 2,
                  'origin %s: %d parts, transform %s' % ((x, y, z), count, list(out)))


@test
def dos_debris_random_and_motion(rep, rnd, n):
    """The separate Gaussian cursor and the debris body's flight and landings on flat and sloped terrain."""
    e = mw2emu.Emu()
    L = lib('msim')
    e.call(0x49fe0, eax=0)
    gaussian = (ctypes.c_int32 * 270)()
    L.shim_dos_gaussian(gaussian, len(gaussian))
    for i, got in enumerate(gaussian):
        want = s32(e.call(0x4a0b0))
        rep.check(want == got, 'Gaussian draw %d: DOS %d port %d' % (i, want, got))
    node = e.alloc(128)
    body = 0xfc60c
    for _ in range(n * 4):
        index = rnd.randrange(127)
        gravity = rnd.randrange(16384, 131072)
        e.w32(0x9e8f8, index)
        e.w32(0x957b4, (((gravity * 0x794 + 32768) >> 16) << 16) // 0x794)
        e.w32(body + 1, node)
        e.call(0x2b3f0, eax=0)
        want = [e.rs32(body + 5 + 4*i) for i in range(6)] + [e.r32(0x9e8f8)]
        out = (ctypes.c_int32 * 7)()
        L.shim_dos_debris_launch(index, gravity, out)
        rep.check(list(out) == want, 'launch %d at gravity %d: DOS %s port %s' % (index, gravity, want, list(out)))
    initial = []
    applied = []
    def position(em):
        for reg, value in zip(('edx', 'ebx', 'ecx'), initial):
            em.w32(em.reg(reg), value)
    def translation(em):
        applied[:3] = [s32(em.reg(r)) for r in ('edx', 'ebx', 'ecx')]
    def rotation(em):
        applied[3:] = [s32(em.reg(r)) for r in ('edx', 'ebx', 'ecx')]
    e.stub(0x1df00, position)
    e.stub(0x1dfc0, translation)
    e.stub(0x1e140, rotation, pops=4)
    def floor_at(x):
        return 120 + slope * (x - 1000) // 2 if slope else 0

    e.stub(0x1c120, lambda em: floor_at(s32(em.reg('eax'))))
    e.stub(0x1e1b0)
    e.stub(0x1e240)
    e.stub(0x1e480)
    for _ in range(n * 14):
        radius = rnd.randrange(10, 190, 2)
        e.stub(0x38770, ret=radius)
        gravity, ticks = rnd.choice([32768, 65536, 78643]), rnd.choice([1, 5, 6, 7, 20])
        e.w32(0x957b0, (gravity * 0x794 + 32768) >> 16)
        e.w32(0x95820, ticks)
        index = rnd.randrange(127)
        e.w32(0x9e8f4, index)
        slope = rnd.choice((0, 1, -1))
        x = rnd.randrange(900, 1102, 2)
        initial[:] = [x, floor_at(x) + radius // 2 + rnd.randint(0, 125), 1500]
        velocity = [rnd.randint(-7 * 65536, 7 * 65536) for _ in range(3)]
        spin = [rnd.randint(-100000, 100000) for _ in range(3)]
        e.w32(body + 1, node)
        for i, value in enumerate(velocity + spin):
            e.w32(body + 5 + i * 4, value)
        e.w32(body + 29, -e.r32(0x957b0))
        applied.clear()
        e.call(0x2b930, eax=0)
        if len(applied) != 6:
            raise RuntimeError('debris routine did not run')
        active = bool(e.r32(body + 1))
        want = [initial[j] + applied[j] for j in range(3)]
        want += [e.rs32(body + 5 + 4*j) for j in range(3)]
        want += [int(active), *applied[3:], e.r32(0x9e8f4)]
        inputs = (ctypes.c_int32 * 14)(*initial, *velocity, *spin, radius // 2, index, gravity, ticks, slope)
        out = (ctypes.c_int32 * 11)()
        L.shim_dos_debris(inputs, out)
        got = list(out)
        if not active:
            got[3:6] = [0, 0, 0]
        rep.check(want == got, 'debris input %s: DOS %s port %s' % (list(inputs), want, got))


@test
def dos_damage_random_order(rep, rnd, n):
    """Hits on intact armor and penetrations consume the executable's shared draws in order."""
    e = mw2emu.Emu()
    e.call(0x49fe0, eax=0)
    obj, control, locations = e.alloc(0x100), e.alloc(0x200), e.alloc(8 * 40)
    e.w32(control, obj)
    e.w32(control + 0x58, locations)
    e.w32(obj + 4, 1)
    e.w32(PLAYER, 0)
    e.w32(0x9ee48, -1)
    e.w32(0x982dc, 0)
    e.stub(0x25740)
    e.stub(0x1e7e0)
    L = lib('msim')
    for _ in range(n * 18):
        armor, damage = rnd.randrange(1, 20), rnd.randrange(1, 20)
        location = rnd.choice(range(1, 9))
        index = rnd.randrange(127)
        e.w32(0x9e8f4, index)
        for j in range(8):
            rec = locations + 40*j
            e.w32(rec, armor * 65536)
            e.w32(rec + 8, 100 * 65536)
            e.w16(rec + 0x24, 0)
            e.w16(rec + 0x26, 0)
        e.call(0x26270, eax=control, edx=damage * 65536, ebx=location)
        want = e.r32(0x9e8f4)
        got = L.shim_dos_damage_rolls(index, location, damage, armor)
        rep.check(want == got, 'location %d armor %d damage %d draw %d: DOS %d port %d' %
                  (location, armor, damage, index, want, got))


@test
def dos_shared_rotations(rep, rnd, n):
    """Mech joints, multiple tasks on one scenery node, and a debris slot share one normalization counter."""
    e = mw2emu.Emu()
    matrix = ctypes.c_int32 * 9
    rotate = lib('msim').mech3d_dos_rotate
    rotate.argtypes = [matrix, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)] + [ctypes.c_int32] * 3
    rotate.restype = None
    nodes = [e.alloc(128) for _ in range(5)]
    identity = [1 << 29, 0, 0, 0, 1 << 29, 0, 0, 0, 1 << 29]
    matrices = [matrix(*identity) for _ in nodes]
    countdowns = [ctypes.c_uint(rnd.randint(1, 12)) for _ in nodes]
    phase = ctypes.c_uint(rnd.randint(64, 191))
    e.w32(0x95ae4, phase.value)
    for node, left in zip(nodes, countdowns):
        for i, v in enumerate(identity):
            e.w32(node + 12 + 4 * i, v)
        e.w32(node + 120, left.value)
    for _ in range(n * 5):
        for i in (0, 1, 2, 2, 2, 3, 4):
            node, mat, left = nodes[i], matrices[i], countdowns[i]
            pitch, yaw, roll = [rnd.randint(-327680, 327680) for _ in range(3)]
            if i < 4:
                pitch, roll = 0, 0
            e.call(0x1e140, eax=node, edx=pitch, ebx=yaw, ecx=roll, stack=(0,))
            rotate(mat, ctypes.byref(left), ctypes.byref(phase), pitch, yaw, roll)
            orig = [e.rs32(node + 12 + 4 * k) for k in range(9)]
            rep.check(orig == list(mat) and e.r32(node + 120) == left.value and e.r32(0x95ae4) == phase.value,
                      'node %d axes %d,%d,%d: DOS %s (%d/%d), port %s (%d/%d)' %
                      (i, pitch, yaw, roll, orig, e.r32(node + 120), e.r32(0x95ae4), list(mat), left.value, phase.value))


@test
def player_scenery_shots(rep, rnd, n):
    """Solid scenery stops a player shot and its aim ray with the camera looking either way."""
    out = (ctypes.c_int * 4)()
    lib('msim').shim_player_scenery_shots(out)
    rep.check(list(out) == [4000, 2000, 4000, 2000], 'wall shot and aim distances: %s' % list(out))


@test
def dos_scenery_collision_lods(rep, rnd, n):
    """Compare the camera cull and the persistent model selection at their boundaries."""
    e = emu()
    obj, camera = e.alloc(76), e.alloc(196)
    models = [e.alloc(24) for _ in range(3)]
    e.w32(obj + 28, models[0])
    for i in range(2):
        e.w32(models[i] + 12, models[i + 1])
    e.w32(618524, camera)
    for address in (1375872, 1375860, 1375952):
        e.w32(address, 1 << 29)
    I = ctypes.c_int32
    port = lib('msim').shim_dos_scenery
    port.argtypes = [ctypes.POINTER(I), I, I, I, ctypes.POINTER(I), ctypes.POINTER(I), I, ctypes.POINTER(I), I]
    focal = (I * 2)(65536, 87380)
    normal = (I * 2)(92681, 109222)
    near, far = 161, 75000
    for off, value in ((24, focal[0]), (168, normal[0]), (172, normal[1]), (176, focal[1]), (180, far)):
        e.w32(camera + off, value)
    e.w32(1375924, near)
    e.w32(1375908, far)

    for scale in (65536, 131072, 163840):
        e.w32(camera + 184, scale)
        for distances in ((1, 8500, 25000), (0, 8500, 25000), (200, 200, 1800)):
            for model, distance in zip(models, distances):
                e.w32(model, distance)
            cases = [(x, y, z, r) for x, y, z, r in ((0, 0, near - 1, 0), (0, 0, near, 0),
                     (0, 0, far, 0), (0, 0, far + 1, 0), (20000, 0, 3000, 100),
                     (0, -20000, 3000, 100), (3000, 2200, 3000, 100))]
            for threshold in distances[1:]:
                depth = (threshold * scale + 32768) >> 16
                cases.extend((0, 0, depth + offset, 1) for offset in (-1, 0, 1))
            cases.extend(tuple(rnd.randint(-55000, 55000) for _ in range(3)) + (rnd.randint(0, 1500),)
                         for _ in range(n))
            e.w32(obj + 32, models[-1])
            actual = len(models) - 1
            for x, y, z, radius in cases:
                e.write(obj + 52, struct.pack('<4i', x, y, z, radius))
                if e.call(0x3f500, eax=obj) == 0:
                    e.call(0x3cda0, eax=obj, edx=e.rs32(1375404))
                expected = models.index(e.r32(obj + 32))
                actual = port((I * 3)(x, y, z), radius, near, far, focal, normal, scale, (I * 3)(*distances), actual)
                rep.check(actual == expected, f'{(x, y, z, radius)} scale {scale} thresholds {distances}: DOS {expected}, port {actual}')


@test
def dos_volley_warning_order(rep, rnd, n):
    """Track each volley-start draw when the shooter's aim reference changes or has no recipient."""
    e = mw2emu.Emu()
    e.call(0x49fe0, eax=0)
    shooter, control, shot, a, b = (e.alloc(0x200) for _ in range(5))
    e.w32(control, shooter)
    e.w32(shooter + 0x10, 2)
    for i, target in enumerate((a, b)):
        e.w32(0xf8a40 + 4 * (i + 1), target)
        e.w32(target + 0x16e, 4)  # already jumping, so only the launch's first draw is relevant
    decision = []
    def recipient(_uc, _addr, _size, _data):
        decision.append(e.reg('eax'))
    e.uc.hook_add(UC_HOOK_CODE, recipient, begin=0x2e4cb, end=0x2e4cb)
    shim = lib('msim').shim_dos_warning_roll
    shim.argtypes = [ctypes.POINTER(ctypes.c_uint), ctypes.c_int, ctypes.c_int]
    i = ctypes.c_uint()
    for _ in range(n * 15):
        w, lv, ref = rnd.choice((0x15, 0x2, 0x10)), rnd.randint(1, 4), rnd.choice((0, 0x201, 0x202))
        e.w32(shot + 4, w)
        e.w8(shooter + 0x198, 4 if lv <= 2 else 0)
        e.w32(shooter + 0xdc, ref)
        e.w32(0x9ee70 + w * 0x58, int(w == 2))
        decision.clear()
        e.call(0x2e450, eax=shot, edx=control)
        react = shim(ctypes.byref(i), w, lv)
        rep.check(decision == ([ref] if react else []) and i.value == e.r32(0x9e8f4),
                  'weapon %d level %d target %#x: DOS %s draw %d port %d draw %d' %
                  (w, lv, ref, decision, e.r32(0x9e8f4), react, i.value))


@test
def dos_weapon_selection_effects(rep, rnd, n):
    """Follow a successful selection into the forced-lock roll and ENEMYFIR cue."""
    e = emu()
    e.call(0x49fe0, eax=0)
    obj, ctrl, inputs = (e.alloc(0x200) for _ in range(3))
    e.w32(obj + 0x4c, inputs)
    e.w32(PLAYER, 5)
    sounded = []
    e.stub(SND, lambda em: sounded.append(em.reg('eax')) or 0, pops=4)
    shim = lib('msim').shim_dos_selection_effects
    index, sound = ctypes.c_uint(), ctypes.c_int()
    for _ in range(n * 25):
        weapon = rnd.randrange(31)
        skill = rnd.randrange(7)
        ref, target = rnd.choice(((0x205, -1), (0x201, 0), (0x405, -3), (0x105, -3), (0, -2)))
        prior_lock = rnd.choice((0, 0x80))
        e.w8(obj + 0x154, max(1, skill) + 1)
        e.w16(obj + 0x14a, ref)
        e.w16(ctrl + 0x10c, prior_lock)
        sounded.clear()
        e.run(0x2ea63, 0x2eae8, ebx=ctrl, ecx=obj, esi=1, edi=weapon)
        locked = shim(ctypes.byref(index), weapon, skill, target, ctypes.byref(sound))
        observed = e.r16(ctrl + 0x10c), sounded, e.r32(0x9e8f4)
        expected = prior_lock | (locked << 7), [sound.value] if sound.value else [], index.value
        rep.check(observed == expected, 'weapon %d skill %d target %#x: DOS %s port %s' %
                  (weapon, skill, ref, observed, expected))

@test
def dos_motion_plane(rep, rnd, n):
    """Compare the original world-space approach, jet drag, and signed position integrator."""
    e = emu()
    L = lib('msim')
    vec = ctypes.c_int32 * 2
    L.shim_dos_motion_plane.argtypes = [vec] + [ctypes.c_int32] * 7 + [vec]
    unit, control, keys = (e.alloc(sz) for sz in (0x200, 0x120, 0x60))
    frame = mw2emu.STACK_TOP - 0x800
    e.w32(frame + 126, control)
    e.w32(control, unit)
    e.w32(control + 160, 2)
    e.w32(unit + 76, keys)
    e.w32(0x957b0, 1940)
    e.w32(0x961e4, 100000)
    for _ in range(n * 16):
        angle = rnd.randint(-180 * 65536, 180 * 65536)
        ticks = rnd.choice([0, 1, 3, 6, 7, 13, 45, 46, 96])
        drive = rnd.choice([-32, 31, 0, rnd.randint(-20 * 65536, 20 * 65536)])
        top, thrust = rnd.randint(225, 1000), rnd.randint(1400, 12000)
        freev, direction = rnd.randrange(2), rnd.randrange(5)
        if not freev: direction = 0
        vx, vz = [rnd.choice([-4096, -1, 0, 4095, rnd.randint(-36 * 65536, 36 * 65536)]) for _ in range(2)]
        parent = e.alloc(48)
        e.call(0x3b88b, stack=(parent, 0, angle, 0, 0, 0, 0))
        for off, value in ((184, e.rs32(parent + 32) >> 13), (188, e.rs32(parent + 8) >> 13),
                           (120, int(not freev))):
            e.w32(unit + off, value)
        for off, value in ((44, drive), (136, top), (236, thrust), (192, 500 if direction else 0),
                           (244, vx), (248, 0), (252, vz)):
            e.w32(control + off, value)
        e.write(keys, bytes(0x60))
        if direction:
            e.w8(keys + 29, 1)
            e.w8(keys + 29 + direction, 1)
        for off, value in ((26, 0), (74, 0), (30, 0), (46, vx), (50, 0), (54, vz), (118, 0), (122, -1940)):
            e.w32(frame + off, value)
        e.w32(TICKS, ticks)
        e.run(0x26ba2 if freev else 0x26ada, 0x2701c, esi=0)
        expected = [e.rs32(frame + off) for off in (46, 54, 26, 30)]
        vel, move = vec(vx, vz), vec()
        L.shim_dos_motion_plane(vel, drive, angle, thrust, top, freev, direction, ticks, move)
        rep.check(expected == [*vel, *move],
                  'angle %d ticks %d airborne %d dir %d v %s: DOS %s port %s' %
                  (angle, ticks, freev, direction, (vx, vz), expected, [*vel, *move]))


@test
def dos_motion_lift(rep, rnd, n):
    """Compare vertical integration for walking, falling, lifting and directional thrust."""
    e = emu()
    L = lib('msim')
    L.shim_dos_motion_lift.argtypes = [ctypes.POINTER(ctypes.c_int32)] + [ctypes.c_int32] * 6
    L.shim_dos_motion_lift.restype = ctypes.c_int32
    unit, control, keys = (e.alloc(sz) for sz in (0x200, 0x120, 0x60))
    frame = mw2emu.STACK_TOP - 0x800
    e.w32(frame + 126, control)
    e.w32(control, unit)
    e.w32(control + 136, 630)
    e.w32(control + 160, 2)
    e.w32(unit + 76, keys)
    for _ in range(n * 16):
        grounded, mode = rnd.randrange(2), rnd.randrange(3)
        ticks = rnd.choice([0, 1, 3, 6, 7, 13, 45, 46, 96])
        gravity, thrust = rnd.randint(800, 3500), rnd.randint(1400, 12000)
        damping = rnd.randint(85000, 140000)
        vy = 0 if grounded else rnd.choice([0, -1, 1, rnd.randint(-18 * 65536, 18 * 65536)])
        e.w32(0x957b0, gravity)
        e.w32(0x961e4, damping)
        e.w32(unit + 120, grounded)
        e.w32(control + 192, 500)
        e.w32(control + 236, thrust)
        e.w32(control + 248, vy)
        e.write(keys, bytes(0x60))
        if mode:
            e.w8(keys + 29, 1)
        if mode == 2:
            e.w8(keys + 32, 1)
        for off, value in ((26, 0), (74, 0), (30, 0), (46, 0), (50, vy), (54, 0), (118, 0), (122, -gravity)):
            e.w32(frame + off, value)
        e.w32(TICKS, ticks)
        e.run(0x26ba2 if mode or not grounded else 0x26ada, 0x2701c, esi=0)
        expected = (e.rs32(frame + 50), e.rs32(frame + 74))
        vel = ctypes.c_int32(vy)
        move = L.shim_dos_motion_lift(ctypes.byref(vel), gravity, thrust, damping, grounded, mode, ticks)
        rep.check(expected == (vel.value, move),
                  'ground %d mode %d ticks %d v %d: DOS %s port %s' %
                  (grounded, mode, ticks, vy, expected, (vel.value, move)))


@test
def dos_unit_bounce(rep, rnd, n):
    """Compare the DOS contact projection and bounce with the original unit response."""
    e = emu()
    L = lib('msim')
    vec = ctypes.c_int32 * 3
    result = ctypes.c_int32 * 6
    L.shim_dos_unit_bounce.argtypes = [vec, vec, ctypes.c_int32, vec, vec, result]
    control, other, centre, end = (e.alloc(sz) for sz in (0x120, 0x120, 12, 12))
    frame = mw2emu.STACK_TOP - 0x800
    e.w32(frame - 8, centre)
    for k, off in enumerate((24, 20, 28)):
        e.w32(frame - off, end + 4*k)
    for _ in range(n * 8):
        pos = [rnd.randint(-360000, 360000) for _ in range(3)]
        normal = [rnd.randint(-65536, 65536) for _ in range(3)]
        velocities = [[rnd.randint(-900000, 900000) for _ in range(3)] for _ in range(2)]
        radius = rnd.randint(580, 1300)
        e.w32(frame - 16, radius)
        for k in range(3):
            e.w32(0x95ad4 + 4*k, normal[k])
            e.w32(centre + 4*k, pos[k])
            e.w32(control + 256 + 4*k, velocities[0][k])
            e.w32(other + 256 + 4*k, velocities[1][k])
        e.run(0x2043a, 0x20540, esi=control, edi=other)
        expected = [e.rs32(end + 4*k) for k in range(3)] + [e.rs32(control + 244 + 4*k) for k in range(3)]
        got = result()
        L.shim_dos_unit_bounce(vec(*pos), vec(*normal), radius, vec(*velocities[0]), vec(*velocities[1]), got)
        rep.check(list(got) == expected, 'radius %d normal %s: DOS %s port %s' %
                  (radius, normal, expected, list(got)))


@test
def dos_face_response(rep, rnd, n):
    """Compare the fixed-point terrain face reflection and residual displacement."""
    e = emu()
    L = lib('msim')
    vec = ctypes.c_int32 * 3
    L.shim_dos_face_response.argtypes = [vec, vec, vec, ctypes.c_int, vec]
    control = e.alloc(0x120)
    frame = mw2emu.STACK_TOP - 0x800
    for _ in range(n * 12):
        direction, normal = ([rnd.randint(-65536, 65536) for _ in range(3)] for _ in range(2))
        velocity = [rnd.randint(-35 * 65536, 35 * 65536) for _ in range(3)]
        remaining = rnd.randint(1, 1200)
        for k in range(3):
            e.w32(frame - 22 + 4*k, direction[k])
            e.w32(0x95ad4 + 4*k, normal[k])
            e.w32(control + 256 + 4*k, velocity[k])
        e.run(0x1fee6, 0x1ffee, esi=control)
        e.run(0x20013, 0x20052, ecx=remaining)
        got_vel, got_move = vec(*velocity), vec()
        L.shim_dos_face_response(vec(*direction), vec(*normal), got_vel, remaining, got_move)
        want_vel = [e.rs32(control + v) for v in (244, 248, 252)]
        want_move = [e.rs32(frame + v) for v in (106, 94, 102)]
        rep.check(want_vel == list(got_vel) and want_move == list(got_move),
                  'direction %s normal %s, DOS %s %s port %s %s' %
                  (direction, normal, want_vel, want_move, list(got_vel), list(got_move)))


@test
def dos_drive_input_order(rep, rnd, n):
    """The drive target reads the previous throttle before either servo advances."""
    e = mw2emu.Emu()
    ctl, unit = e.alloc(0x160), e.alloc(0x90)
    e.w32(ctl, unit)
    shim = lib('msim').shim_dos_drive_frame
    got = (ctypes.c_int32 * 2)()
    for _ in range(n):
        throttle, speed, now = 0, 0, 1000
        factor = rnd.choice((301, 585, 674, 1055, 1337))
        for i in range(100):
            target = (1017, 791, 0, 339)[i // 25]
            ticks = rnd.choice((0, 1, 5, 6, 7, 10, 18, 29))
            reverse, slow = i >= 75, i >= 45
            tau = 91 if slow else 36
            e.w32(ctl + 0x88, factor)
            e.w32(unit + 0x84, 2 if reverse else 0)
            for ofs, raw in ((0x24, (now, 0, speed, tau)), (0x44, (now, target+1024, throttle+1024, 36))):
                for k, value in enumerate(raw):
                    e.w32(ctl + ofs + 4*k, value)
            e.run(0x27ecf, 0x27f0e, esi=ctl, edi=1)
            e.w32(NOW, now + ticks)
            speed_after = e.call(0x49b80, eax=ctl+0x24)
            throttle_after = e.call(0x49b80, eax=ctl+0x44) - 1024
            shim(throttle, target, speed, factor, ticks, reverse, slow, got)
            rep.check((throttle_after, speed_after) == tuple(got),
                      'tick %d: DOS %s port %s' % (now, (throttle_after, speed_after), tuple(got)))
            throttle, speed, now = throttle_after, speed_after, now + ticks

@test
def dos_drive_servo(rep, rnd, n):
    """DOS drive servo's exact residual, including the sign of a stopped mech's speed readout."""
    L = lib('msim')
    L.shim_dos_drive_servo.argtypes = [ctypes.c_int32] * 4
    L.shim_dos_drive_servo.restype = ctypes.c_int32
    for case in range(n * 4):
        e = emu()
        sv = e.alloc(16)
        tau = rnd.choice([36, 91])
        value = rnd.choice([-32, -1, 0, 1, 32, rnd.randint(-0x240000, 0x240000)])
        target = rnd.choice([0, rnd.randint(-0x240000, 0x240000)])
        e.w32(sv + 4, target)
        e.w32(sv + 8, value)
        e.w32(sv + 12, tau)
        now = 0
        for frame in range(12):
            ticks = rnd.choice([0, 1, 3, 6, 14, 24, tau - 1, tau, tau + 1])
            now += ticks
            e.w32(NOW, now)
            dos = s32(e.call(0x49b80, eax=sv))
            port = L.shim_dos_drive_servo(value, target, ticks, tau)
            rep.check(dos == port, '%d -> %d over %d / %d ticks: DOS %d port %d' %
                      (value, target, ticks, tau, dos, port))
            value = dos
    e = emu()
    sv = e.alloc(16)
    for value in (-180 * 65536, 180 * 65536):
        target, tau, ticks = -value, 54, 183
        for offset, v in enumerate((0, target, value, tau)):
            e.w32(sv + 4*offset, v)
        e.w32(NOW, ticks)
        dos = e.call(0x49b80, eax=sv)
        port = L.shim_dos_drive_servo(value, target, ticks, tau)
        rep.check(dos == port, 'low-word multiply %d -> %d: DOS %d port %d' % (value, target, dos, port))

@test
def servo_setup(rep, rnd, n):
    """MW2.EXE 0x49b40 (a servo's set-up: T = chop(seconds x 182)) for the times the controller uses vs the port's T"""
    e = emu()
    want = {0.2: 36, 0.3: 54, 0.6: 109, 0.8: 145, 0.5: 91}
    for s, port_T in want.items():
        sv = e.alloc(16)
        lo, hi = mw2emu.struct.unpack('<II', mw2emu.struct.pack('<d', s))
        e.call(0x49b40, eax=sv, edx=0, ebx=0, stack=(lo, hi))
        rep.check(e.rs32(sv + 12) == port_T, '%.1f s: dos T %d port %d' % (s, e.rs32(sv + 12), port_T))

@test
def turret_twist(rep, rnd, n):
    """MW2.EXE 0x2edc3..0x2ee2a (a turret's twist demand, limit >= 360: the servo value taken round to within 180 of the
    demand) + the servo step 0x49b80 (T 145) vs msim_twist_round"""
    e = emu(); L = lib('msim')
    L.msim_twist_round.restype = ctypes.c_float
    L.msim_twist_round.argtypes = [ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_int]
    for case in range(n * 10):
        obj = e.alloc(0x100); ib = e.alloc(0x50); c = e.alloc(0x200)
        e.w32(c, obj); e.w32(obj + 0x4c, ib); e.w32(c + 0xe0, 0x1680000); e.w32(c + 0x10, 145)
        cur = rnd.randint(-0xb40000, 0xb40000); now = 5000
        e.w32(c + 0xc, cur); e.w32(c + 4, now)
        pc = cur / 65536.0
        for f in range(20):
            dem = rnd.choice([0xb30000, -0xb30000, 0xb40000, -0xb40000, rnd.randint(-0xb40000, 0xb40000)])
            e.w32(ib + 4, dem); e.w32(ib, 0)
            e.run(0x2edc3, 0x2ee2a, edx=c)
            t = rnd.choice([3, 4, 6, 12]); now += t; e.w32(NOW, now)
            e.call(0x49b80, eax=c + 4)
            dv = s32(e.r32(c + 0xc)) / 65536.0
            pc = L.msim_twist_round(pc, dem / 65536.0, 145, t)
            d = (dv - pc + 180.0) % 360.0 - 180.0
            rep.check(abs(d) < 0.02, 'case %d frame %d demand %.2f: dos %.3f port %.3f' % (case, f, dem / 65536.0, dv, pc))
        if case % 20 == 19: e.reset_data()

@test
def collision_damage(rep, rnd, n):
    """MW2.EXE 0x20a50 (a wall: the unit's own velocity) and 0x20890 (a unit: the velocity difference) -> 0x20930 (the
    location by the contact normal and bearing, the head x max(1, 4 x tonnage ratio)) vs combat.c collide() fed the
    octagonal length of the same velocity; damage per location (0x26270 stubbed)"""
    L = lib('combat')
    L.shim_collide.argtypes = [ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_float)]
    out = (ctypes.c_float * 8)()
    def dlen(x, y, z):
        a = sorted([abs(x), abs(y), abs(z)], reverse=True)
        return (a[0] * 4 + a[1] + a[2]) >> 2
    e = emu()
    for k in range(n * 30):
        if k % 50 == 0: e = emu()
        hits = []
        e.stub(0x26270, lambda em: hits.append((s32(em.reg('ebx')), s32(em.reg('edx')))) or 0)
        e.stub(0x51b70, ret=0, pops=4)
        dif_block(e)
        obj = e.alloc(0x100); c = e.alloc(0x200); e.w32(c, obj)
        other = e.alloc(0x200)
        own_t, oth_t = rnd.choice([20, 35, 50, 75, 100]), rnd.choice([20, 35, 50, 75, 100])
        e.w32(c + 0xe4, own_t); e.w32(other + 0xe4, oth_t)
        v = [rnd.randint(-0x1800000 // 16, 0x1800000 // 16) for _ in range(3)]
        ov = [rnd.randint(-0x100000, 0x100000) for _ in range(3)] if rnd.random() < 0.5 else [0, 0, 0]
        for j in range(3): e.w32(c + 0x100 + 4 * j, v[j]); e.w32(other + 0x100 + 4 * j, ov[j])
        heading = rnd.randint(-0xb40000, 0xb40000); twist = rnd.randint(-0x3c0000, 0x3c0000)
        e.w32(obj + 0x60, heading); e.w32(obj + 0x6c, twist)
        ang = rnd.uniform(-math.pi, math.pi); ny = rnd.choice([-0.95, -0.87, -0.8, 0.0, 0.3, 0.7, 0.72, 0.95, rnd.uniform(-1, 1)])
        hl = math.sqrt(max(0.0, 1 - ny * ny))
        nx, nz = hl * math.sin(ang), hl * math.cos(ang)
        e.w32(0x95ad4, int(nx * 65536)); e.w32(0x95ad8, int(ny * 65536)); e.w32(0x95adc, int(nz * 65536))
        unit = rnd.random() < 0.5
        if unit: e.call(0x20890, eax=c, edx=other)
        else: wall = e.alloc(0x40); e.call(0x20a50, eax=c, edx=wall)
        dos = [0.0] * 8
        for loc, dmg in hits: dos[loc - 1] += dmg / 65536.0
        S = dlen(v[0] - ov[0], v[1] - ov[1], v[2] - ov[2]) if unit else dlen(*v)
        rel = math.degrees(math.atan2(-int(nx * 65536), -int(nz * 65536))) - (heading + twist) / 65536.0
        while rel > 180: rel -= 360
        while rel < -180: rel += 360
        L.shim_collide(S / 65536.0, int(ny * 65536) / 65536.0, rel, oth_t if unit else 0, own_t, out)
        prt = list(out)
        ok = all(abs(a - b) < 0.02 + 0.002 * abs(a) for a, b in zip(dos, prt))
        rep.check(ok, '%s S %.3f ny %.3f rel %.1f tons %d/%d: dos %s port %s' % ('unit' if unit else 'wall', S / 65536.0, ny, rel,
                  oth_t, own_t, [round(x, 3) for x in dos], [round(x, 3) for x in prt]))

@test
def manoeuvre_closing(rep, rnd, n):
    """DOS compares closing distance through the fixed-point factor and skips range updates on frames with zero ticks."""
    e = emu()
    e.uc.reg_write(UC_X86_REG_FPCW, e.r16(0xa4520))
    L = lib('ai')
    out = (ctypes.c_int * 3)()
    L.shim_dos_close.argtypes = [ctypes.c_int] * 3 + [type(out)]
    unit, frame = e.alloc(0x210), e.alloc(0x40) + 0x20
    for _ in range(n * 12):
        ticks = rnd.choice([0, 1, 3, 6, 7, 19])
        distance = rnd.choice([7999, 8000, 8001, rnd.randint(5500, 50000)])
        speed = rnd.randint(-50, 50)
        previous = distance + speed * ticks + rnd.randint(-3, 3)
        e.w32(unit + 0xc0, distance)
        e.w32(unit + 0x194, previous)
        e.w32(TICKS, ticks)
        e.run(0x2d400, 0x2d482, ecx=unit, ebp=frame, esi=0)
        done, kept = e.reg('esi'), e.rs32(unit + 0x194)
        e.w32(unit + 0x194, previous)
        e.run(0x2d840, 0x2d871, ebx=unit)
        closing_speed = s32(e.reg('edx'))
        L.shim_dos_close(previous, distance, ticks, out)
        dos = [done, kept, closing_speed]
        rep.check(list(out) == dos, '%d → %d over %d ticks: DOS %s port %s' %
                  (previous, distance, ticks, dos, list(out)))

@test
def manoeuvre_step(rep, rnd, n):
    """DOS 0x2c210 starts or updates a manoeuvre in one pass, with whole ticks through the ending frame."""
    e = emu()
    L = lib('ai')
    out = (ctypes.c_int * 7)()
    L.shim_dos_attack_step.argtypes = [ctypes.c_int] * 9 + [ctypes.c_uint, type(out)]
    unit, control, inputs = e.alloc(0x210), e.alloc(0x200), e.alloc(0x60)
    e.w32(unit + 0x20, control)
    e.w32(unit + 0x4c, inputs)
    e.w16(unit + 0x146, 3)
    # Supply a fixed target geometry and suppress weapon selection in the original step.
    for addr in (0x2dac0, 0x22850, 0x2e860, 0x2e300, 0x2db80):
        e.stub(addr)
    e.stub(0x2c880, lambda _em: chosen)
    for _ in range(n * 12):
        chosen = rnd.choice([0, 4, 8, 10, 11])
        man = rnd.choice([-1, chosen, chosen])
        ticks = rnd.choice([0, 3, 6, 7, 19])
        now = 10000
        left = rnd.choice([ticks, ticks + 1, 90])
        distance = rnd.choice([4499, 4500, 14999, 15000, 30000, 30001, 40000])
        bearing = rnd.randint(-90, 90)
        throttle = rnd.choice([0, 0x333, 0x400])
        target_height = rnd.randint(-2000, 2000)
        height = rnd.choice([0, target_height + 1999, target_height + 2000])
        seed = rnd.getrandbits(32)
        e.stub(RAND, Lcg(seed))
        e.w32(NOW, now + ticks)
        e.w32(TICKS, ticks)
        e.w8(unit + 0x16c, man & 255)
        e.w8(unit + 0x16d, 255)
        e.w32(unit + 0x16e, 0)
        e.w32(unit + 0x172, now + left)
        e.w32(unit + 0x190, int(man in (4, 11)))
        e.w32(unit + 0xc0, distance)
        e.w32(unit + 0xcc, target_height)
        e.w32(unit + 0xd4, bearing * 65536)
        e.w32(unit + 0x54, height)
        e.w32(inputs + 8, throttle)
        e.w32(inputs + 0xc, 15 * 65536)
        e.w8(inputs + 0x1d, int(man in (4, 11)))
        e.w8(inputs + 0x1e, int(man == 11))
        e.w8(inputs + 0x2f, int(man in (8, 10)))
        e.call(0x2c210, eax=unit)
        active = e.r8(unit + 0x16c)
        dos = [active if active != 255 else -1, e.rs32(inputs + 8), e.rs32(inputs + 0xc),
               e.r8(inputs + 0x2f), e.r8(inputs + 0x1d), e.r8(inputs + 0x1e),
               0 if active == 255 else e.rs32(unit + 0x172) - now - ticks]
        L.shim_dos_attack_step(man, chosen, ticks, left, distance, bearing, throttle, height, target_height, seed, out)
        rep.check(dos[:2] == list(out[:2]) and abs(dos[2] - out[2]) <= 4 and dos[3:] == list(out[3:]),
                  'man %d chosen %d ticks %d left %d bearing %d distance %d: DOS %s port %s' %
                  (man, chosen, ticks, left, bearing, distance, dos, list(out)))

@test
def manoeuvre_approach(rep, rnd, n):
    """Exercise DOS approach, weaving and charges on clear and obstructed paths."""
    e = emu()
    L = lib('ai')
    out = (ctypes.c_int * 6)()
    L.shim_dos_approach.argtypes = [ctypes.c_int] * 8 + [type(out)]
    unit, control, inputs = e.alloc(0x210), e.alloc(0x200), e.alloc(0x60)
    e.w32(unit + 0x20, control)
    e.w32(unit + 0x4c, inputs)
    e.w16(unit + 0x146, 3)
    for addr in (0x22850, 0x2e860, 0x2e300):
        e.stub(addr)

    def avoid(em):
        if blocked:
            em.w32(inputs + 8, 256)
            em.w32(inputs + 0xc, round(-204.8 * 65536))
        return int(bool(blocked))

    e.stub(0x2db80, avoid)
    e.stub(0x2e400, ret=12)
    for _ in range(n * 12):
        man = rnd.choice([2, 3, 6])
        distance = rnd.choice([5499, 5500, 7999, 8000, 8001, 11000, 11001, 40000])
        if man == 6:
            distance = rnd.choice([0, 1999, 2000, 2001, 9000])
        bearing = rnd.choice([-45, -2, 0, 2, 45, rnd.randint(-90, 90)])
        ticks, until_flip = rnd.choice([0, 3, 7, 19]), rnd.choice([0, 3, 7, 90])
        previous = distance + rnd.randint(-200, 1800)
        sign, blocked = rnd.choice([-1, 1]), rnd.randrange(3 if man == 6 else 2)
        now = 10000
        e.w32(NOW, now + ticks)
        e.w32(TICKS, ticks)
        e.w8(unit + 0x16c, man)
        e.w8(unit + 0x16d, 255)
        e.w32(unit + 0x16e, 0)
        e.w32(unit + 0x172, now + 900)
        e.w32(unit + 0x176, now + until_flip)
        e.w32(unit + 0x17a, sign)
        e.w32(unit + 0xc0, distance)
        e.w32(unit + 0xc4, distance)
        e.w32(unit + 0x182, 12 if blocked == 2 else 13)
        e.w32(unit + 0xd4, bearing * 65536)
        e.w32(unit + 0x194, previous)
        e.w32(inputs + 8, 512)
        e.w32(inputs + 0xc, 15 * 65536)
        e.w8(inputs + 0x43, 0)
        e.call(0x2c210, eax=unit)
        active = e.r8(unit + 0x16c) != 255
        dos = [man if active else -1, e.rs32(inputs + 0xc), e.rs32(inputs + 8),
               e.rs32(unit + 0x17a), e.rs32(unit + 0x176) - now - ticks, e.r8(inputs + 0x43)]
        L.shim_dos_approach(man, previous, distance, bearing, ticks, sign, until_flip, blocked, out)
        rep.check(dos[0] == out[0] and abs(dos[1] - out[1]) <= 5 and dos[2] == out[2] and
                  dos[5] == out[5] and (man != 3 or not active or dos[3:5] == list(out[3:5])),
                  'man %d prev %d dist %d bearing %d ticks %d deadline %d blocked %d: DOS %s port %s' %
                  (man, previous, distance, bearing, ticks, until_flip, blocked, dos, list(out)))


@test
def dos_flank_waypoints(rep, rnd, n):
    """Use the original 0x2d070 to place a waypoint on each side of a turning target."""
    e = emu()
    L = lib('ai')
    out = (ctypes.c_int32 * 2)()
    L.shim_dos_flank_point.argtypes = [ctypes.c_int32] * 5 + [type(out)]
    unit, matrix, x, y, z = e.alloc(0x100), e.alloc(48), *(e.alloc(4) for _ in range(3))
    e.w32(0xf8a40, unit)
    e.stub(0x1e540, ret=matrix)
    for _ in range(n):
        origin = [rnd.randint(-250000, 250000) for _ in range(3)]
        yaw = rnd.randint(-46080, 46080) * 256
        e.call(0x3b88b, stack=(matrix, 0, yaw, 0, *origin))
        for slot in range(16):
            dist = rnd.choice([10000, 15000, rnd.randint(1, 15000)])
            e.w32(x, 0)
            e.w32(z, 0)
            e.call(0x2d070, eax=0x200, edx=slot, ebx=x, ecx=z, stack=(y, dist))
            L.shim_dos_flank_point(origin[0], origin[2], yaw, slot, dist, out)
            original = [e.rs32(x), e.rs32(z)]
            rep.check(original == list(out), 'heading %d slot %d distance %d: DOS %s port %s' %
                      (yaw, slot, dist, original, list(out)))


@test
def manoeuvre_flank(rep, rnd, n):
    """Compare DOS waypoint approach and target-facing controls on both sides of the transition."""
    e = emu()
    L = lib('ai')
    out = (ctypes.c_int * 3)()
    L.shim_dos_flank.argtypes = [ctypes.c_int] * 7 + [type(out)]
    unit, control, inputs = e.alloc(0x210), e.alloc(0x200), e.alloc(0x60)
    e.w32(unit + 0x20, control)
    e.w32(unit + 0x4c, inputs)
    e.w16(unit + 328, 0x101)
    e.w16(unit + 330, 0x201)
    e.stub(0x2e860)
    e.stub(0x2e300)

    def select_ref(em):
        goal = em.reg('edx') == 0x101
        em.w32(unit + 0xc0, level if goal else 12000)
        em.w32(unit + 0xd4, yaw if goal else target * 65536)
        return 1

    def avoid(em):
        if blocked:
            em.w32(inputs + 8, 256)
            em.w32(inputs + 0xc, round(-204.8 * 65536))
        return blocked

    e.stub(0x22850, select_ref)
    e.stub(0x2db80, avoid)
    yaw_ptr, slant, level_ptr, pitch = (e.alloc(4) for _ in range(4))
    for _ in range(n * 12):
        x, z = rnd.choice([(0, 3999), (0, 4000), (3000, 0), (4500, 0), (4501, 0), (4500, 30),
                           (rnd.randint(-12000, 12000), rnd.randint(-12000, 12000))])
        e.call(0x2ab60, eax=x, ebx=z, edx=0, ecx=yaw_ptr, stack=(slant, level_ptr, pitch))
        level, yaw = e.rs32(level_ptr), e.rs32(yaw_ptr)
        target = rnd.choice([-6, -5, 0, 5, 6, rnd.randint(-120, 120)])
        face, blocked = rnd.randrange(2), rnd.randrange(2)
        throttle = rnd.choice([0, 102, 512, 1024])
        e.w32(unit + 0x17e, face)
        e.w32(inputs + 8, throttle)
        e.w32(inputs + 0xc, 15 * 65536)
        e.call(0x2d290, eax=unit)
        original = [e.rs32(unit + 0x17e), e.rs32(inputs + 0xc), e.rs32(inputs + 8)]
        L.shim_dos_flank(1, face, x, z, target, throttle, blocked, out)
        rep.check(original[0] == out[0] and abs(original[1] - out[1]) <= 4 and original[2] == out[2],
                  'phase %d waypoint %d,%d range %d bearing %.2f target %d throttle %d blocked %d: DOS %s port %s' %
                  (face, x, z, level, yaw / 65536, target, throttle, blocked, original, list(out)))


    for _ in range(n * 12):
        turn = rnd.uniform(-math.pi, math.pi)
        radius = rnd.choice([2999, 3000, 3000.5, 3001, 3002])
        x, z = round(radius * math.sin(turn)), round(radius * math.cos(turn))
        e.call(0x2ab60, eax=x, ebx=z, edx=0, ecx=yaw_ptr, stack=(slant, level_ptr, pitch))
        level, yaw = e.rs32(level_ptr), e.rs32(yaw_ptr)
        target, blocked = 0, rnd.randrange(2)
        done = e.call(0x2d550, eax=unit)
        L.shim_dos_flank(7, 0, x, z, target, 512, blocked, out)
        rep.check(done == out[0], 'waypoint %d,%d range %d: DOS done %d port %d' % (x, z, level, done, out[0]))


@test
def dos_empty_weapons_manoeuvre(rep, rnd, n):
    """Aircraft and vehicles with empty weapons fall back to retreat instead of a mech charge."""
    e = mw2emu.Emu()
    unit, ctrl, inputs = e.alloc(0x240), e.alloc(0x220), e.alloc(0x60)
    e.w32(unit + 0x20, ctrl)
    e.w32(unit + 0x4c, inputs)
    e.w32(ctrl, unit)
    e.w32(ctrl + 0x88, 1)
    e.stub(0x2e240, ret=1)
    shim = lib('ai').shim_dos_empty_weapons
    out = (ctypes.c_int * 3)()
    frame = mw2emu.STACK_TOP - 0x800
    e.w32(frame - 8, ctrl)
    for cls in (1, 2, 4, 5, 8):
        for level in range(1, 5):
            for jets, first, second in ((0, 0, 1), (0, 3, 1), (0, 3, 0), (1, 3, 1)):
                e.w32(unit, cls)
                e.w8(unit + 0x155, level)
                e.call(0x2c5f0, eax=unit)  # sets the class and pilot level flags
                e.w8(unit + 0x16c, 0)
                e.w8(unit + 0x16d, 0xff)
                e.w16(unit + 0x146, 3)
                e.w32(unit + 0x17e, 0)
                e.w32(ctrl + 0xc0, 100 if jets else 0)
                e.w32(ctrl + 0xec, jets)
                rolls = [first, second]
                e.stub(RAND, lambda em: rolls.pop(0) % em.reg('eax'))
                e.run(0x2c4d0, 0x2c587, esi=unit, ebp=frame)
                native = [e.rs32(unit + 0x16e), e.r32(unit + 0x17e), 2 - len(rolls)]
                shim(cls, level, jets, first, second, out)
                rep.check(native == list(out), 'class %d level %d jets %d rolls %s: DOS %s port %s' %
                          (cls, level, jets, (first, second), native, list(out)))

@test
def manoeuvre_circle(rep, rnd, n):
    """Check that the aircraft steers for the current waypoint on the update that advances it."""
    e = emu()
    L = lib('ai')
    out = (ctypes.c_int * 4)()
    L.shim_dos_circle.argtypes = [ctypes.c_int] * 5 + [type(out)]
    unit, control, inputs = e.alloc(0x210), e.alloc(0x200), e.alloc(0x60)
    e.w32(unit + 0x20, control)
    e.w32(unit + 0x4c, inputs)
    e.w16(unit + 328, 0x101)  # assigned waypoint
    e.w16(unit + 330, 0x201)  # attack target
    e.stub(0x2e860)
    e.stub(0x2e300)

    def select_ref(em):
        goal = em.reg('edx') == 0x101
        em.w32(unit + 0xc0, level if goal else 12000)
        em.w32(unit + 0xd4, yaw if goal else 0)
        return 1

    def avoid(em):
        if blocked:
            em.w32(inputs + 8, 256)
            em.w32(inputs + 0xc, round(-204.8 * 65536))
        return blocked

    def next_point(_em):
        visited.append(1)

    e.stub(0x22850, select_ref)
    e.stub(0x2db80, avoid)
    e.stub(0x23590, next_point)
    yaw_ptr, slant, level_ptr, pitch = (e.alloc(4) for _ in range(4))
    for _ in range(n * 12):
        x, z = rnd.choice([(0, 2999), (0, 3000), (0, 3001), (3000, 0), (2100, 2100), (2100, 2200),
                           (rnd.randint(-9000, 9000), rnd.randint(-9000, 9000))])
        e.call(0x2ab60, eax=x, ebx=z, edx=0, ecx=yaw_ptr, stack=(slant, level_ptr, pitch))
        level, yaw = e.rs32(level_ptr), e.rs32(yaw_ptr)
        visited = []
        blocked = rnd.randrange(2)
        ticks = rnd.choice([0, 3, 7, 19])
        due = rnd.choice([0, 3, 7, 40])
        now = 10000
        e.w32(NOW, now + ticks)
        e.w32(unit + 0x176, now + due)
        e.w32(inputs + 8, 512)
        e.w32(inputs + 0xc, 15 * 65536)
        e.call(0x2d990, eax=unit)
        dos = [len(visited), e.rs32(unit + 0x176) - now - ticks,
               e.rs32(inputs + 0xc), e.rs32(inputs + 8)]
        L.shim_dos_circle(x, z, due, ticks, blocked, out)
        rep.check(dos[:2] == list(out[:2]) and abs(dos[2] - out[2]) <= 4 and dos[3] == out[3],
                  'waypoint %d,%d distance %d bearing %.2f blocked %d step %d due %d: DOS %s port %s' %
                  (x, z, level, yaw / 65536, blocked, ticks, due, dos, list(out)))


@test
def manoeuvre_circle_replay(rep, rnd, n):
    """Use the original nav add, remove, and selection routines as the aircraft's target moves."""
    e = emu()
    L = lib('ai')
    ptr = ctypes.POINTER(ctypes.c_int32)
    L.shim_dos_circle_replay.argtypes = [ptr, ctypes.c_int, ptr]
    nav, nav_count, units, unit_count = 0xf8b30, 0x96288, 0xf8a40, 0x961c8
    unit, target, control, inputs = (e.alloc(sz) for sz in (0x210, 0x210, 0x200, 0x60))
    for i, obj in enumerate((unit, target), 1):
        e.w32(units + 4*i, obj)
        e.w32(obj + 4, i)
        e.w32(obj + 8, 3)
    e.w32(unit_count, 3)
    e.w32(unit + 0x20, control)
    e.w32(unit + 0x4c, inputs)
    e.stub(0x2db80)
    e.stub(0x2e860)
    e.stub(0x2e300)

    def set_range(em):
        ref = em.reg('edx') & 0xffff
        xz = target + 80 if ref == 0x202 else nav + 84*(ref & 255) + 24
        dx, dz = sorted((abs(em.rs32(xz) - x), abs(em.rs32(xz + 8) - z)), reverse=True)
        em.w32(unit + 0xc0, dx + dz // 4)
        return 1

    e.stub(0x22850, set_range)
    for _ in range(n):
        offset = 10000
        now = offset
        e.w32(nav_count, 1)  # mission nav at index zero
        e.w16(unit + 328, 0x202)
        e.w16(unit + 330, 0x202)
        e.w32(unit + 374, offset)
        e.w32(target + 80, 50000)
        e.w32(target + 88, 60000)
        e.call(0x23420, eax=unit)
        frames, expected = [], []
        for i in range(36):
            goal = nav + 84*(e.r16(unit + 328) & 255)
            x = e.rs32(goal + 24) + rnd.choice([2999, 3000, 3001, 7000])
            z = e.rs32(goal + 32)
            tx = rnd.choice([x + 14999, x + 15000, x + 15001, x + 22000])
            tz = z
            ticks = rnd.choice([0, 7, 30, 420])
            frames.extend((x, z, tx, tz, ticks))
            e.w32(unit + 80, x)
            e.w32(unit + 88, z)
            e.w32(target + 80, tx)
            e.w32(target + 88, tz)
            now += ticks
            e.w32(NOW, now)
            e.call(0x2d990, eax=unit)
            points = [e.rs32(nav + 84*j + axis) for j in range(1, e.r32(nav_count)) for axis in (24, 32)]
            expected.append([(e.r16(unit + 328) & 255) - 1, e.r32(nav_count) - 1,
                             e.rs32(unit + 374) - now, *points])
        out = (ctypes.c_int32 * (35 * len(expected)))()
        L.shim_dos_circle_replay((ctypes.c_int32 * len(frames))(*frames), len(expected), out)
        for i, row in enumerate(expected):
            got = list(out[35*i:35*i+len(row)])
            rep.check(row == got, 'step %d frame %s: DOS %s port %s' % (i, frames[5*i:5*i+5], row, got))


@test
def manoeuvre_glide(rep, rnd, n):
    """Exercise the DOS glide dispatcher, including fuel and closing-rate boundaries."""
    e = emu()
    L = lib('ai')
    out = (ctypes.c_int * 8)()
    L.shim_dos_glide.argtypes = [ctypes.c_int] * 14 + [type(out)]
    unit, control, inputs = e.alloc(0x210), e.alloc(0x200), e.alloc(0x60)
    e.w32(unit + 0x20, control)
    e.w32(unit + 0x4c, inputs)
    e.w16(unit + 0x146, 3)
    for addr in (0x22850, 0x2e860):
        e.stub(addr)
    for _ in range(n * 12):
        phase = rnd.randrange(3)
        d = rnd.choice([999, 1000, 1001, 5999, 6000, 6001, 10000])
        lim = rnd.choice([10, 30, 45])
        bear = rnd.choice([0, lim, -lim - 1, 3 * lim - 1, -3 * lim, 180])
        fuel = rnd.choice([-1, 0, 1, 5, 6, 100])
        heat = rnd.choice([0, 39, 40])
        installed, grounded, contact = (rnd.randrange(2) for _ in range(3))
        ticks = rnd.choice([0, 3, 7, 19])
        previous = d + ticks * rnd.choice([-5, 8, 9, 10, 35, 36, 37])
        held_jet, held_forward, held_back = (rnd.randrange(2) for _ in range(3))
        now = 10000
        e.w32(NOW, now + ticks)
        e.w32(TICKS, ticks)
        e.w8(unit + 0x16c, 5)
        e.w8(unit + 0x16d, 255)
        e.w32(unit + 0x16e, 0)
        e.w32(unit + 0x172, now + 900)
        e.w32(unit + 0x17a, phase)
        e.w32(unit + 0x190, 1)
        e.w32(unit + 0x194, previous)
        e.w32(unit + 0xc0, d)
        e.w32(unit + 0xd4, bear * 65536)
        e.w32(unit + 0x78, grounded)
        e.w32(control + 0xe0, lim << 16)
        e.w32(control + 0xec, installed)
        e.w32(control + 0x98, heat << 16)
        e.w32(control + 0xa4, contact)
        e.w32(control + 0xc0, fuel)
        e.w32(inputs + 8, 512)
        e.w32(inputs + 0xc, 15 * 65536)
        for off, value in [(0x1d, held_jet), (0x20, held_forward), (0x21, held_back)]:
            e.w8(inputs + off, value)
        e.call(0x2c210, eax=unit)
        active = e.r8(unit + 0x16c) != 255
        original = [5 if active else -1, e.rs32(unit + 0x17a) if active else -1, e.rs32(unit + 0x194),
                    e.r8(inputs + 0x1d), e.r8(inputs + 0x20), e.r8(inputs + 0x21),
                    e.rs32(inputs + 0xc), e.rs32(inputs + 8)]
        L.shim_dos_glide(phase, previous, d, bear, lim, fuel, heat, installed, grounded, contact, ticks,
                         held_jet, held_forward, held_back, out)
        rep.check(list(out) == original, 'phase %d previous %d distance %d bearing %d limit %d fuel %d heat %d '
                  'installed %d ground %d contact %d step %d: DOS %s port %s' %
                  (phase, previous, d, bear, lim, fuel, heat, installed, grounded, contact, ticks, original, list(out)))


@test
def manoeuvre_jump_climb(rep, rnd, n):
    """DOS jump attack keeps separate manoeuvre and jet phase deadlines."""
    e = emu()
    L = lib('ai')
    out = (ctypes.c_int * 6)()
    L.shim_dos_jump_climb.argtypes = [ctypes.c_int] * 13 + [type(out)]
    unit, control, inputs = e.alloc(0x210), e.alloc(0x200), e.alloc(0x60)
    e.w32(unit + 0x20, control)
    e.w32(unit + 0x4c, inputs)
    e.w16(unit + 0x146, 3)
    e.w32(unit + 0xc0, 20000)
    for addr in (0x22850, 0x2e860, 0x2e300, 0x5914e, 0x49fa0):
        e.stub(addr)
    # Supply LOS. Descent braking and the touchdown flag are read by 0x2dac0.
    e.stub(0x2da50, lambda _em: blocked)
    for _ in range(n * 12):
        phase = rnd.randrange(3)
        elapsed = rnd.choice([0, 175, 182])
        ticks = rnd.choice([0, 3, 7, 19])
        left = rnd.choice([ticks, ticks + 1, 900])
        fuel = rnd.choice([-1, 0, 5, 6, 300])
        height = rnd.choice([500, 19999, 20000, 30000])
        vy = rnd.choice([-1000000, -899860, -899859, -793993, -793992, 0, 1000000])
        jets, held_jet = rnd.randrange(2), rnd.randrange(2)
        heat = rnd.choice([20, 39, 40, 80])
        blocked, grounded = rnd.randrange(2), rnd.randrange(2)
        bearing = rnd.randint(-90, 90)
        now = 10000
        e.w32(NOW, now + ticks)
        e.w32(TICKS, ticks)
        e.w8(unit + 0x16c, 9)
        e.w8(unit + 0x16d, 255)
        e.w32(unit + 0x16e, 0)
        e.w32(unit + 0x172, now + left)
        e.w32(unit + 0x176, now + 182 - elapsed)
        e.w32(unit + 0x17a, phase)
        e.w32(unit + 0x190, 1)
        e.w32(unit + 0xd4, bearing * 65536)
        e.w32(control + 0xc0, fuel)
        e.w32(control + 0x98, heat << 16)
        e.w32(control + 0xec, jets)
        e.w32(control + 0xf8, vy)
        e.w32(unit + 0x54, height)
        e.w32(unit + 0x78, grounded)
        e.w8(inputs + 0x1d, held_jet)
        e.w32(inputs + 8, 512)
        e.w32(inputs + 0xc, 15 * 65536)
        e.call(0x2c210, eax=unit)
        active = e.r8(unit + 0x16c) != 255
        dos = [9 if active else -1, e.rs32(unit + 0x17a) if active else -1,
               e.rs32(unit + 0x172) - now - ticks if active else 0,
               e.r8(inputs + 0x1d), e.rs32(inputs + 0xc), e.rs32(inputs + 8)]
        L.shim_dos_jump_climb(phase, elapsed, left, ticks, bearing, fuel, heat, blocked, grounded, height, vy, jets, held_jet, out)
        rep.check(dos[:4] == list(out[:4]) and abs(dos[4] - out[4]) <= 4 and dos[5] == out[5],
                  'phase %d elapsed %d left %d step %d fuel %d heat %d blocked %d grounded %d height %d vy %d jets %d held %d: DOS %s port %s' %
                  (phase, elapsed, left, ticks, fuel, heat, blocked, grounded, height, vy, jets, held_jet, dos, list(out)))


@test
def manoeuvre_start(rep, rnd, n):
    """MW2.EXE 0x2cbf0 (a manoeuvre's start: its time limit, reverse input +0x2f, by the jump table 0x2cbb8; rand
    0x4a070 fed the port's own draw) vs ai.c start_manoeuvre (DOS), manoeuvres 0 1 4 8 10 11 12 with contact cases; 11's jet with the warning's +0x17a
    left set, and the 4-then-5 flag +0x17e after the start"""
    L = lib('ai'); L.shim_rand.restype = ctypes.c_uint
    L.shim_man_limit.restype = ctypes.c_float
    rv, js, jf = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
    for man in (0, 1, 4, 8, 10, 11, 12):
        for case in range(n):
            e = emu()
            seed = rnd.randint(1, 1 << 30)
            prev = rnd.choice([0, 10, 3]); contact = rnd.randint(0, 1); cu = rnd.randint(0, 1)
            e.stub(RAND, lambda em, s=seed: L.shim_rand(s, em.reg('eax')))
            e.stub(0x23420, ret=0)
            u = e.alloc(0x200); ib = e.alloc(0x60); ctl = e.alloc(0x200)
            e.w32(u + 0x4c, ib); e.w32(u + 0x20, ctl); e.w32(ctl + 0xa4, contact)
            e.w8(u + 0x16c, man); e.w8(u + 0x16d, prev); e.w32(u + 0x7c, 3 if cu else 0xffffffff)
            fwd = rnd.randint(0, 1)   # the missile warning's dodge quadrant left in +0x17a (4: forward), the 4-then-5 flag set
            e.w32(u + 0x17a, 4 if fwd else 0); e.w32(u + 0x17e, 1)
            e.w32(NOW, 1000); e.w32(TICKS, 6)
            e.call(0x2cbf0, eax=u)
            dl = e.rs32(u + 0x172) - 1000
            dr = e.r8(ib + 0x2f)
            dj = (e.r8(ib + 0x1e), e.r8(ib + 0x20))
            dq = e.rs32(u + 0x17e)
            pl = L.shim_man_limit(man, prev, contact, cu, seed, ctypes.byref(rv), ctypes.byref(js), ctypes.byref(jf), fwd)
            pj = (1 if js.value < 0 else 0, jf.value)
            pq = L.shim_start_q5(man)
            rep.check(abs(dl - pl) < 0.5 and (man not in (8, 10) or dr == rv.value) and (man != 11 or dj == pj) and (dq == 1) == (pq == 1),
                      'man %d prev %d contact %d unit %d fwd %d: dos limit %d reverse %d jets %s flag %d, port %.0f %d %s %d'
                      % (man, prev, contact, cu, fwd, dl, dr, dj, dq, pl, rv.value, pj, pq))

class Lcg:
    """the port's ai_rand sequence (ai.c) as MW2.EXE's rand 0x4a070 stub, so both sides draw the same numbers"""
    def __init__(self, seed): self.s = seed & 0xffffffff
    def __call__(self, em):
        n = em.reg('eax') & 0xffffffff
        self.s = (self.s * 1103515245 + 12345) & 0xffffffff
        return ((self.s >> 16) & 0x7fff) % n if n else 0

@test
def manoeuvre_choose(rep, rnd, n):
    """MW2.EXE 0x2c880 (the manoeuvre chooser: class 1's contact / non-unit / jump pre-check - 0x2da50 and the view test
    0x2e150 stubbed - then the class record 0xfdf10 built by 0x2c778: the first pick, the row of the last manoeuvre, 4 after
    4 with +0x17e, 4's jets / level / signed-bearing test, 3 -> 2, 5 on the ground) vs ai.c choose_manoeuvre (DOS); every
    class, the Elemental table, random states; the answer and the rand draws used"""
    L = lib('ai'); L.shim_choose2.restype = ctypes.c_int
    U = ctypes.c_uint(); F = ctypes.c_float
    import struct
    # the class records as 0x2c778..0x2c861 (inline in the unit set-up) builds them: {rows, first, table}
    recs = b''.join(struct.pack('<hhI', *({1: (13, 7, 0x963f4), 5: (1, 0, 0x964f0), 8: (1, 0, 0x96502)}.get(k, (1, 0, 0x964de))))
                    for k in range(1, 9)) + struct.pack('<hhI', 13, 7, 0x96514)
    flag = 1
    for case in range(n * 60):
        e = emu(); e.write(0xfdf10, recs); e.w32(0x96600, flag)
        cls = rnd.choice([1, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8]); tons1 = rnd.random() < 0.2
        prev = rnd.choice([-1] + list(range(13))); man = rnd.choice([0, 1, 3, 4, 10, 12])
        contact = rnd.random() < 0.3; reverse = rnd.random() < 0.3; jump_man = rnd.random() < 0.3
        tnu = rnd.random() < 0.15; can_jump = rnd.random() < 0.7; heat = rnd.choice([0, 10, 19, 20, 35])
        level = rnd.randint(1, 4); above = rnd.random() < 0.3; grounded = rnd.random() < 0.5
        twist = rnd.choice([5, 9, 10, 60, 90, 120, 180]); off = rnd.choice([rnd.uniform(-180, 180), 75.0, -75.0, 0.0])
        q5 = rnd.random() < 0.4; seed = rnd.randint(1, 1 << 30)
        u = e.alloc(0x200); ctl = e.alloc(0x200); ib = e.alloc(0x60)
        e.w32(u, cls); e.w32(u + 0x20, ctl); e.w32(ctl, u); e.w32(u + 0x4c, ib); e.w32(ctl + 0xe4, 1 if tons1 else 50)
        e.w8(ib + 0x2f, 1 if reverse else 0); e.w32(u + 0x190, 1 if jump_man else 0); e.w8(u + 0x16c, man)
        e.w32(ctl + 0xa4, 1 if contact else 0); e.w16(u + 0x14a, 0x100 if tnu else 0x205)
        e.w32(ctl + 0xc0, 100 if can_jump else 0); e.w32(ctl + 0xec, 1); e.w32(ctl + 0x98, heat << 16)
        e.w8(u + 0x198, 2 if level < 4 else 0); e.w8(u + 0x16d, prev & 0xff); e.w32(u + 0x17e, 1 if q5 else 0)
        e.w32(u + 0x16e, 0); e.w32(ctl + 0xe0, twist << 16); e.w32(u + 0x78, 1 if grounded else 0)
        e.stub(0x22850, ret=0); e.stub(0x2da50, ret=1); e.stub(0x2e150, ret=1 if above else 0)
        e.stub(0x230d0, ret=int(round(off * 65536)) & 0xffffffff)
        g = Lcg(seed); e.stub(RAND, g)
        dos = e.call(0x2c880, eax=u) & 0xffff; dos = dos - 0x10000 if dos & 0x8000 else dos
        prt = L.shim_choose2(cls, 1 if tons1 else 50, prev, contact, reverse, jump_man, man, tnu, can_jump, F(heat), level,
                             above, grounded, F(twist), F(off), q5, seed, ctypes.byref(U))
        rep.check(dos == prt and U.value == g.s,
                  'cls %d el %d prev %d man %d contact %d rev %d jm %d tnu %d jets %d heat %d lvl %d above %d gnd %d twist %d off %.1f q5 %d: dos %d port %d%s'
                  % (cls, tons1, prev, man, contact, reverse, jump_man, tnu, can_jump, heat, level, above, grounded, twist, off, q5,
                     dos, prt, '' if U.value == g.s else ' (rand draws differ)'))

@test
def missile_warning(rep, rnd, n):
    """MW2.EXE 0x2e450 (a guided launch or a PPC warns its target): the shooter's +0x198 bit 4 and rand(3); the target's
    +0x16e already 4; jets and heat < 65 (0x2e3d0 run); rand(3); the quadrant of the shooter off the target's heading
    (0x230d0 stubbed); rand(2) for the first side, the 50 m probes (0x2df30 / 0x1c3f0 stubbed, either side blocked) -> 11;
    else a level <= 3 pilot with nothing forced jumps (4) - vs ai.c ai_missile_warning (DOS). Type 0 tests the
    convergence point and target projection (stubbed) and the angle limits. Type 2 skips the window tests."""
    L = lib('ai'); L.shim_warn.restype = ctypes.c_int
    U = ctypes.c_uint(); O = (ctypes.c_int * 2)(); F = ctypes.c_float
    for case in range(n * 40):
        e = emu()
        lv_s = rnd.randint(1, 4); lv_t = rnd.randint(1, 4); can_jump = rnd.random() < 0.75
        heat = rnd.choice([0, 30, 64, 65, 80]); forced = rnd.choice([0, 0, 0, 4, 10])
        rel = rnd.choice([rnd.uniform(-180, 180), rnd.randint(-4, 4) * 90.0, 89.999, -0.001])
        shooter_type = rnd.choice([0, 2])
        aim_visible, target_visible = rnd.randrange(2), rnd.randrange(2)
        bearing = int(rnd.choice([-15, -14.01, -14, 0, 14.99, 15, rnd.uniform(-20, 20)]) * 65536)
        elevation = int(rnd.choice([-11521, -11520, -89, 0, 89, 11520, 11521]) * 65536)
        in_view = shooter_type == 2 or (aim_visible and target_visible and -15 < bearing >> 16 < 15
                                        and -0x30000 < int(elevation / 0xf00) < 0x30000)
        fp, fm = rnd.random() < 0.6, rnd.random() < 0.6; seed = rnd.randint(1, 1 << 30); t = 5
        shot = e.alloc(0x40); sh = e.alloc(0x200); sp = e.alloc(8); tg = e.alloc(0x200); tc = e.alloc(0x200)
        e.w32(shot + 4, 0x15); e.w32(sp, sh); e.w32(sh + 0x10, shooter_type); e.w8(sh + 0x198, 4 if lv_s <= 2 else 0)
        e.w32(sh + 0xd8, elevation)
        e.w32(sh + 0xdc, 0x200 | t); e.w16(sh + 4, 0); e.w32(0xf8a40 + 4 * t, tg)
        e.w32(tg + 0x16e, forced); e.w32(tg + 0x20, tc); e.w32(tg + 0xdc, 0x200)
        e.w32(tc + 0xc0, 100 if can_jump else 0); e.w32(tc + 0xec, 1); e.w32(tc + 0x98, heat << 16)
        e.w8(tg + 0x198, 2 if lv_t <= 3 else 0)
        g = Lcg(seed); e.stub(RAND, g)
        e.stub(0x22850, ret=0)
        e.stub(0x230d0, lambda em: bearing if em.reg('eax') == sh else math.floor(rel * 65536))
        e.stub(0x29860, ret=aim_visible); e.stub(0x3f320, ret=target_visible)
        probed = []
        def p0(em):
            probed.append(s32(em.reg('ebx')) if em.reg('ebx') & 0x8000 == 0 else s32(em.reg('ebx') | 0xffff0000)); return 0
        e.stub(0x2df30, p0, pops=8)
        e.stub(0x1c3f0, lambda em: 0 if (fp if probed[-1] > 0 else fm) else 1)
        e.call(0x2e450, eax=shot, edx=sp)
        df = s32(e.r32(tg + 0x16e))
        pf = L.shim_warn(seed, lv_s, in_view, F(rel), can_jump, F(heat), lv_t, -1 if forced == 0 else forced, fp, fm, O, ctypes.byref(U))
        pf = 0 if pf == -1 else pf
        po = [x for x in O if x]
        rep.check(df == pf and probed == po and U.value == g.s,
                  'shooter lv %d target lv %d jets %d heat %d forced %d rel %.3f free +%d -%d: dos %d probes %s, port %d %s%s'
                  % (lv_s, lv_t, can_jump, heat, forced, rel, fp, fm, df, probed, pf, po, '' if U.value == g.s else ' (rand draws differ)'))

@test
def torso_inputs(rep, rnd, n):
    """MW2.EXE 0x2e930..0x2e971 (the fire decision's torso inputs: 0x22970 the bearing off the legs / 0x2d00 and 0x229a0
    the elevation / 0xf00, each through 0x23a40's +-0x400 clamp, written x 0x2d00 / x 0xf00 to input +4 / +0) vs ai.c
    dos_twist_q / dos_pitch_q - the turret and dropship aim demand as much as a mech's"""
    L = lib('ai'); L.shim_twist_q.restype = ctypes.c_float; L.shim_pitch_q.restype = ctypes.c_float
    F = ctypes.c_float
    for case in range(n * 50):
        e = emu()
        aim = rnd.choice([rnd.uniform(-180, 180), rnd.randint(-1024, 1024) * 180 / 1024.0, rnd.uniform(-1, 1)])
        el = rnd.choice([rnd.uniform(-90, 90), rnd.randint(-1100, 1100) * 60 / 1024.0, rnd.uniform(-0.2, 0.2)])
        af = int(aim * 65536) & 0xffffffff; ef = int(el * 65536)
        u = e.alloc(0x200); e.w32(u + 0xd8, ef & 0xffffffff)
        tw = s32(e.call(0x22970, eax=u, edx=af, ebx=0)) * 0x2d00 / 65536.0
        pt = s32(e.call(0x229a0, eax=u, edx=0)) * 0xf00 / 65536.0
        pw = L.shim_twist_q(F(aim)); pp = L.shim_pitch_q(F(el))
        rep.check(abs(tw - pw) < 1e-4 and abs(pt - pp) < 1e-4,
                  'aim %.5f el %.5f: dos %.5f %.5f port %.5f %.5f' % (aim, el, tw, pt, pw, pp))

@test
def patrol_sweep(rep, rnd, n):
    """MW2.EXE 0x24710 (the patrol torso sweep of states 5-8, class 1: 5 degrees every 0x40 / 0x20 ticks, turning past 45,
    the due time +0x172 pulled in when the interval shortens) over runs of frames vs ai.c patrol_sweep_dos"""
    e = emu(); L = lib('ai'); F = ctypes.c_float
    for run in range(n):
        frames = 300
        ticks = [rnd.choice([1, 3, 6, 6, 6, 9, 12, 30, 70]) for _ in range(frames)]
        thr = [rnd.choice([0, 0, 0x333, 0x400]) if rnd.random() < 0.1 or k == 0 else None for k in range(frames)]
        for k in range(1, frames):
            if thr[k] is None: thr[k] = thr[k - 1]
        torso0 = rnd.choice([0, 5, -45, 45, 40]) * 0x10000
        dir0 = rnd.randint(0, 1)
        now = 1000; due = now + rnd.randint(-20, 64)
        u = e.alloc(0x200); inp = e.alloc(0x40)
        e.w32(u, 1); e.w32(u + 0x4c, inp); e.w32(inp + 4, torso0); e.w32(u + 0x172, due); e.w32(u + 0x17a, dir0)
        dos = []
        for k in range(frames):
            now += ticks[k]
            e.w32(NOW, now); e.w32(inp + 8, thr[k])
            e.call(0x24710, eax=u)
            dos.append(s32(e.r32(inp + 4)) / 65536.0)
        out = (F * frames)()
        L.shim_patrol_seq(frames, (F * frames)(*ticks), (F * frames)(*[t / 1024.0 for t in thr]), out,
                          1 if dir0 else -1, F(torso0 / 65536.0), F(float(due - 1000)))
        bad = [k for k in range(frames) if abs(out[k] - dos[k]) > 1e-3]
        rep.check(not bad, 'run %d: first differing frame %s dos %s port %s' %
                  (run, bad[0] if bad else '-', dos[bad[0]] if bad else '-', out[bad[0]] if bad else '-'))
        e.reset_data()

@test
def flank_pick(rep, rnd, n):
    """MW2.EXE 0x2e6f0 (manoeuvre 1's slot: the sector of the attacker's bearing off the target's facing - 0x230d0 stubbed
    with the angle - and the counters +0x19c: the front case on the attacker's own, the others on the target's) vs ai.c
    flank_pick, random and sector-edge angles, random counters, both rand(2) results"""
    L = lib('ai'); L.shim_rand.restype = ctypes.c_uint
    I8 = ctypes.c_int * 8
    for case in range(n * 40):
        e = emu()
        rel = rnd.choice([rnd.uniform(-180, 180), rnd.randint(-8, 8) * 22.5, rnd.randint(-4, 4) * 45.0, 180.0])
        err = int(round(rel * 65536))
        seed = rnd.randint(1, 1 << 30)
        own = [rnd.choice([0, 0, 1]) for _ in range(8)]
        tgt = [rnd.choice([0, 0, 1, 2]) for _ in range(8)]
        me, t = 3, 7
        u = e.alloc(0x200); tr = e.alloc(0x200)
        e.w32(u + 4, me); e.w16(u + 0x14a, 0x200 | t); e.w32(0xf8a40 + 4 * t, tr)
        for k in range(8): e.w8(u + 0x19c + k, own[k]); e.w8(tr + 0x19c + k, tgt[k])
        e.stub(0x22850, ret=0)
        e.stub(0x230d0, ret=err & 0xffffffff)
        e.stub(RAND, lambda em, s=seed: L.shim_rand(s, em.reg('eax')))
        dos = e.call(0x2e6f0, eax=u) & 0xffff
        prt = L.shim_flank_pick(ctypes.c_float(rel), seed, I8(*own), I8(*tgt)) * 2
        rep.check(dos == prt, 'rel %.3f own %s tgt %s: dos slot %d port %d' % (rel, own, tgt, dos, prt))

@test
def seg_poly(rep, rnd, n):
    """GBL 0x1c7a0 (a segment against one face: the plane crossing in 2.29 x cm integers, t = (-f << 16) / (n . d) on the
    octagonal-length direction 0x1d960, the crossing point, then 0x1cbc0 -> 0x1cc50 / 0x1cfb0 / 0x1ce00 point in polygon
    on the plane of the normal's largest component, and the segment cut there, 0x1de20) vs eseg_poly + cpoly_in (msim.c,
    floats): random convex faces (3-6 integer vertices round a centre, any orientation) and segments crossing near them.
    Hit / miss must agree, the cut point within 2 cm + 3.5 cm / cos (the DOS plane distance's rounding along a grazing segment); a case whose crossing lies within 2 cm of an edge (either side's
    rounding can decide it) is counted, not judged."""
    L = lib('msim')
    F3 = ctypes.c_float * 3
    L.shim_dos_shot_face.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_int, F3, F3, F3, ctypes.c_int, F3]
    edge = 0
    for case in range(n * 40):
        e = emu()
        e.uc.reg_write(UC_X86_REG_FPCW, e.r16(0xa4520))
        nv = rnd.randint(3, 6)
        # the face's plane: a random unit normal, a centre, two axes in the plane
        while True:
            nx, ny, nz = rnd.gauss(0, 1), rnd.gauss(0, 1), rnd.gauss(0, 1)
            if rnd.random() < 0.3: (nx, ny, nz) = rnd.choice([(0, 1, 0), (1, 0, 0), (0, 0, 1), (0, -1, 0), (1, 1, 0), (0, 1, 1)])
            l = math.sqrt(nx * nx + ny * ny + nz * nz)
            if l > 1e-3: break
        nrm = (nx / l, ny / l, nz / l)
        ax = (1, 0, 0) if abs(nrm[0]) < 0.9 else (0, 1, 0)
        u = (nrm[1] * ax[2] - nrm[2] * ax[1], nrm[2] * ax[0] - nrm[0] * ax[2], nrm[0] * ax[1] - nrm[1] * ax[0])
        lu = math.sqrt(sum(c * c for c in u)); u = tuple(c / lu for c in u)
        w = (nrm[1] * u[2] - nrm[2] * u[1], nrm[2] * u[0] - nrm[0] * u[2], nrm[0] * u[1] - nrm[1] * u[0])
        cen = [rnd.randint(-30000, 30000) for _ in range(3)]
        R = rnd.choice([200, 1000, 5000, 20000])
        angs = sorted(rnd.uniform(0, 2 * math.pi) for _ in range(nv))
        if rnd.random() < 0.5: angs.reverse()
        verts = [[int(round(cen[k] + R * (math.cos(a) * u[k] + math.sin(a) * w[k]))) for k in range(3)] for a in angs]
        # DOS records: the face (+2 count, +4 offset of the index bytes, +0x14 normal 2.29), the vertices (0x2c each,
        # world x / y / z at +0xc), the segment (0x38: start, end, delta, direction 16.16, length, +0x34 state)
        normal = F3(*nrm)
        N = [int(round(c * (1 << 29))) for c in normal]
        poly = e.alloc(0x60); vb = e.alloc(0x2c * nv); seg = e.alloc(0x40)
        e.w16(poly + 2, nv); e.w32(poly + 4, 0x40)
        for k in range(3): e.w32(poly + 0x14 + 4 * k, N[k])
        for j in range(nv):
            e.w8(poly + 0x40 + j, j)
            for k in range(3): e.w32(vb + 0x2c * j + 0xc + 4 * k, verts[j][k])
        # a segment from in front of the plane to behind it (now and then parallel, from behind, or short of it)
        a, b = rnd.uniform(0, 1.3), rnd.uniform(0, 1.3)
        q = [cen[k] + R * (a * math.cos(b * 6.28) * u[k] + a * math.sin(b * 6.28) * w[k]) for k in range(3)]
        d0 = rnd.choice([rnd.uniform(1, 3000), rnd.uniform(1, 50), rnd.uniform(-500, -1)])
        d1 = rnd.choice([rnd.uniform(-3000, -1), rnd.uniform(1, 400), -rnd.uniform(0, 30)])
        tilt = [rnd.uniform(-1500, 1500) for _ in range(3)]
        p0 = [int(round(q[k] + nrm[k] * d0 + tilt[k])) for k in range(3)]
        p1 = [int(round(q[k] + nrm[k] * d1 - tilt[k] * rnd.uniform(0, 1))) for k in range(3)]
        if p0 == p1: continue
        deltas = [abs(b - a) for a, b in zip(p0, p1)]
        long_step = case % 2 and (3 * max(deltas) + sum(deltas)) // 4 > 3000
        for k in range(3):
            e.w32(seg + 4 * k, p0[k]); e.w32(seg + 12 + 4 * k, p1[k]); e.w32(seg + 24 + 4 * k, p1[k] - p0[k])
        e.w32(seg + 0x34, 0)
        e.call(0x1d9f0 if long_step else 0x1d960, eax=seg)
        if e.rs32(seg + 0x30) <= 0: continue
        dh = e.call(0x1c7a0, eax=poly, edx=vb, ebx=seg) != 0
        dpt = [e.rs32(seg + 12 + 4 * k) for k in range(3)]
        out = F3()
        vv = (ctypes.c_float * (3 * nv))(*[float(c) for vtx in verts for c in vtx])
        fixed_point = F3()
        length = L.shim_dos_shot_face(vv, nv, normal, F3(*p0), F3(*p1), long_step, fixed_point)
        dos_length = e.rs32(seg + 0x30) if dh else 0x7fffffff
        rep.check(length == dos_length and (not dh or list(fixed_point) == dpt),
                  'fixed segment %d: DOS %d %s port %d %s' % (case, dos_length, dpt, length, list(fixed_point)))
        ph = L.shim_seg_poly(vv, nv, F3(*nrm), F3(*map(float, p0)), F3(*map(float, p1)), out) != 0
        # how near the plane crossing is to the face's edges (in the plane), for the edge class
        dn = sum(nrm[k] * (p1[k] - p0[k]) for k in range(3))
        near = False
        if abs(dn) > 1e-9:
            t = sum(nrm[k] * (verts[0][k] - p0[k]) for k in range(3)) / dn
            X = [p0[k] + (p1[k] - p0[k]) * t for k in range(3)]
            for j in range(nv):
                A, B = verts[j], verts[(j + 1) % nv]
                AB = [B[k] - A[k] for k in range(3)]; AX = [X[k] - A[k] for k in range(3)]
                L2 = sum(c * c for c in AB) or 1
                s = max(0, min(1, sum(AB[k] * AX[k] for k in range(3)) / L2))
                if math.sqrt(sum((AX[k] - AB[k] * s) ** 2 for k in range(3))) < 2.0: near = True
            if abs(t) * math.sqrt(sum((p1[k] - p0[k]) ** 2 for k in range(3))) < 2.0 or abs(1 - t) * math.sqrt(sum((p1[k] - p0[k]) ** 2 for k in range(3))) < 2.0: near = True
            if abs(sum(nrm[k] * (p0[k] - verts[0][k]) for k in range(3))) < 2.0: near = True
        if dh != ph and near: edge += 1; continue
        # the DOS plane distance is a sum of six rounded 2.29 x cm products (up to 3 cm) and t a whole number: along the
        # segment that is up to ~3 cm / cos(angle to the normal) + 1
        sl = math.sqrt(sum((p1[k] - p0[k]) ** 2 for k in range(3)))
        cs = abs(dn) / sl if sl else 1
        tol = 2.0 + 3.5 / max(cs, 1e-3)
        ok = dh == ph and (not dh or max(abs(dpt[k] - out[k]) for k in range(3)) <= tol)
        rep.check(ok, 'case %d nv %d R %d: dos %s %s, port %s %s (p0 %s p1 %s)' % (case, nv, R, dh, dpt if dh else '',
                  ph, [round(c, 1) for c in out] if ph else '', p0, p1))
    rep.notes.append('%d cases within 2 cm of an edge / the plane / an end disagreed (not judged)' % edge)
    # edge inputs: axis-aligned faces (a wall, a floor) with integer corners, hit straight on at a corner, on an edge, just
    # inside / outside them (the DOS crossing rules count a point on an edge or a corner as inside)
    for case in range(n * 20):
        e = emu()
        axis = rnd.randint(0, 2); sgn = rnd.choice([1, -1])
        a1, a2 = [k for k in range(3) if k != axis]
        x0, z0 = rnd.randint(-5000, 5000), rnd.randint(-5000, 5000)
        wdt, hgt = rnd.randint(1, 3000), rnd.randint(1, 3000)
        lvl = rnd.randint(-3000, 3000)
        corners = [(x0, z0), (x0 + wdt, z0), (x0 + wdt, z0 + hgt), (x0, z0 + hgt)]
        if rnd.random() < 0.5: corners = [(x0, z0), (x0 + wdt, z0 + hgt // 2), (x0, z0 + hgt)]
        verts = []
        for (c1, c2) in corners:
            v = [0, 0, 0]; v[axis] = lvl; v[a1] = c1; v[a2] = c2; verts.append(v)
        nv = len(verts)
        nrm = [0.0, 0.0, 0.0]; nrm[axis] = float(sgn)
        pick = rnd.choice(['corner', 'edge', 'in1', 'out1', 'mid'])
        j = rnd.randrange(nv); A, B = verts[j], verts[(j + 1) % nv]
        if pick == 'corner': h = list(A)
        elif pick == 'mid': h = [sum(v[k] for v in verts) // nv for k in range(3)]
        else:
            f = rnd.uniform(0, 1); h = [int(round(A[k] + (B[k] - A[k]) * f)) for k in range(3)]
            if pick != 'edge':
                c = [sum(v[k] for v in verts) / nv for k in range(3)]
                for k in (a1, a2):
                    dd = 1 if c[k] > h[k] else -1 if c[k] < h[k] else 0
                    h[k] += dd if pick == 'in1' else -dd
        d0, d1 = rnd.randint(1, 2000), rnd.randint(1, 2000)
        p0 = list(h); p0[axis] = lvl + sgn * d0
        p1 = list(h); p1[axis] = lvl - sgn * d1
        N = [int(round(c * (1 << 29))) for c in nrm]
        poly = e.alloc(0x60); vb = e.alloc(0x2c * nv); seg = e.alloc(0x40)
        e.w16(poly + 2, nv); e.w32(poly + 4, 0x40)
        for k in range(3): e.w32(poly + 0x14 + 4 * k, N[k])
        for jj in range(nv):
            e.w8(poly + 0x40 + jj, jj)
            for k in range(3): e.w32(vb + 0x2c * jj + 0xc + 4 * k, verts[jj][k])
        for k in range(3):
            e.w32(seg + 4 * k, p0[k]); e.w32(seg + 12 + 4 * k, p1[k]); e.w32(seg + 24 + 4 * k, p1[k] - p0[k])
        e.w32(seg + 0x34, 0)
        e.call(0x1d960, eax=seg)
        dh = e.call(0x1c7a0, eax=poly, edx=vb, ebx=seg) != 0
        out = F3()
        vv = (ctypes.c_float * (3 * nv))(*[float(c) for vtx in verts for c in vtx])
        ph = L.shim_seg_poly(vv, nv, F3(*nrm), F3(*map(float, p0)), F3(*map(float, p1)), out) != 0
        rep.check(dh == ph, 'edge case %d (%s, axis %d): dos %s port %s, face %s, hit at %s' % (case, pick, axis, dh, ph, verts, h))


def main():
    args = sys.argv[1:]
    n, seed = 30, 1
    while args and args[0].startswith('-'):
        o = args.pop(0)
        if o == '-n': n = int(args.pop(0))
        elif o == '-s': seed = int(args.pop(0))
    names = args or list(TESTS)
    ok = True
    for nm in names:
        rep = Report(nm)
        try: TESTS[nm](rep, random.Random(seed), n)
        except Exception as ex: rep.check(False, 'error: %s' % ex)
        ok &= rep.done()
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()

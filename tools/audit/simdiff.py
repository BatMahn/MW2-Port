#!/usr/bin/env python3
"""simdiff - differential tests: the original DOS MW2.EXE (1.1) functions (run under unicorn by mw2emu.py from the ELF
image of the LE executable, NOT in the repo: MW2_ELF=/path/mw2dos.elf, made by le2elf.py) against the port's C in its
DOS edition mode (tools/audit/build.sh builds the shims into SIMDIFF_BUILD, default /tmp/simdiff_build).

  tools/audit/build.sh && MW2_ELF=mw2dos.elf python3 tools/audit/simdiff.py [-n cases] [-s seed] [test ...]

Each test drives both sides with the same random / edge inputs and prints PASS / FAIL with the first differences.
Modelled on the GBL port's tools/audit/simdiff.py; MW2.EXE's controller offsets are mostly GBL's minus 4."""
import ctypes, os, random, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mw2emu
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
    """MW2.EXE 0x250f0..0x2511c: an overridden unit above 100 heat rolls rand(500 x frame ticks) == 0 a frame; the port
    takes it at its 30 fps reference (1 / 18000 a tick). Checks the argument, and the port's mean time to a cook-off at
    6-tick frames against DOS's (within 15 %)"""
    L = lib('combat')
    for t in range(1, 21):
        e = emu()
        args = []
        e.stub(RAND, lambda em: args.append(em.reg('eax')) or 1)
        e.stub(BLOW, ret=0); e.stub(MSG, ret=0); e.stub(SND, ret=0, pops=4)
        dif_block(e)
        obj = e.alloc(0x40); c = e.alloc(0x200); e.w32(c, obj); e.w32(c + 0xa0, 2)
        e.w32(c + 0x98, 105 << 16); e.w16(c + 0x10c, 4 | 8); e.w32(c + 0x8c, 0)
        e.w32(PLAYER, 0); e.w32(NOW, 100); e.w32(TICKS, t)
        e.call(0x24f90, eax=c)
        rep.check(args == [500 * t], 'ticks %d: rand args %s' % (t, args))
    fr = [L.shim_override_frames(6, k + 1, 10 ** 7) for k in range(max(n, 200))]
    mean_s = sum(fr) / len(fr) * 6 / 182.0; dos_s = 500.0 * 36 / 182.0
    rep.notes.append('6-tick frames: port mean %.0f s, DOS %.0f s' % (mean_s, dos_s))
    rep.check(abs(mean_s / dos_s - 1) < 0.15, 'port mean %.1f s vs DOS %.1f s' % (mean_s, dos_s))

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
def servo_step(rep, rnd, n):
    """MW2.EXE 0x49b80 (the integer servo: value += (target - value) x elapsed / T, the target once reached or passed)
    vs msim_servo over random frame sequences, T 36 / 54 / 109 / 145"""
    L = lib('msim')
    L.msim_servo.restype = ctypes.c_float; L.msim_servo.argtypes = [ctypes.c_float] * 4
    for case in range(n * 4):
        e = emu()
        T = rnd.choice([36, 54, 109, 145])
        sv = e.alloc(16); now = 1000
        v0 = rnd.randint(-0x5a0000, 0x5a0000)
        e.w32(sv, now); e.w32(sv + 8, v0); e.w32(sv + 12, T)
        pv = v0 / 65536.0; worst = 0
        for f in range(60):
            if f % 15 == 0: tgt = rnd.randint(-0x5a0000, 0x5a0000); e.w32(sv + 4, tgt)
            t = rnd.choice([1, 3, 4, 6, 9, 14, 20])
            now += t; e.w32(NOW, now)
            e.call(0x49b80, eax=sv)
            pv = L.msim_servo(pv, tgt / 65536.0, float(T), float(t))
            worst = max(worst, abs(e.rs32(sv + 8) / 65536.0 - pv))
        rep.check(worst < 0.01, 'T %d: worst |dos - port| %.5f' % (T, worst))

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
    L.msim_twist_round.restype = ctypes.c_float; L.msim_twist_round.argtypes = [ctypes.c_float] * 4
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
            pc = L.msim_twist_round(pc, dem / 65536.0, 145.0, float(t))
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
    else a level <= 3 pilot with nothing forced jumps (4) - vs ai.c ai_missile_warning (DOS). The type 2 shooter
    skips the screen projection and angle checks (see AUDIT_DOS.md)."""
    L = lib('ai'); L.shim_warn.restype = ctypes.c_int
    U = ctypes.c_uint(); O = (ctypes.c_int * 2)(); F = ctypes.c_float
    for case in range(n * 40):
        e = emu()
        lv_s = rnd.randint(1, 4); lv_t = rnd.randint(1, 4); can_jump = rnd.random() < 0.75
        heat = rnd.choice([0, 30, 64, 65, 80]); forced = rnd.choice([0, 0, 0, 4, 10])
        rel = rnd.choice([rnd.uniform(-180, 180), rnd.randint(-4, 4) * 90.0, 89.999, -0.001])
        fp, fm = rnd.random() < 0.6, rnd.random() < 0.6; seed = rnd.randint(1, 1 << 30); t = 5
        shot = e.alloc(0x40); sh = e.alloc(0x200); sp = e.alloc(8); tg = e.alloc(0x200); tc = e.alloc(0x200)
        e.w32(shot + 4, 0x15); e.w32(sp, sh); e.w32(sh + 0x10, 2); e.w8(sh + 0x198, 4 if lv_s <= 2 else 0)
        e.w32(sh + 0xdc, 0x200 | t); e.w16(sh + 4, 0); e.w32(0xf8a40 + 4 * t, tg)
        e.w32(tg + 0x16e, forced); e.w32(tg + 0x20, tc); e.w32(tg + 0xdc, 0x200)
        e.w32(tc + 0xc0, 100 if can_jump else 0); e.w32(tc + 0xec, 1); e.w32(tc + 0x98, heat << 16)
        e.w8(tg + 0x198, 2 if lv_t <= 3 else 0)
        g = Lcg(seed); e.stub(RAND, g)
        e.stub(0x22850, ret=0); e.stub(0x230d0, ret=int(math.floor(rel * 65536)) & 0xffffffff)
        probed = []
        def p0(em):
            probed.append(s32(em.reg('ebx')) if em.reg('ebx') & 0x8000 == 0 else s32(em.reg('ebx') | 0xffff0000)); return 0
        e.stub(0x2df30, p0, pops=8)
        e.stub(0x1c3f0, lambda em: 0 if (fp if probed[-1] > 0 else fm) else 1)
        e.call(0x2e450, eax=shot, edx=sp)
        df = s32(e.r32(tg + 0x16e))
        pf = L.shim_warn(seed, lv_s, F(rel), can_jump, F(heat), lv_t, -1 if forced == 0 else forced, fp, fm, O, ctypes.byref(U))
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
    edge = 0
    for case in range(n * 40):
        e = emu()
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
        N = [int(round(c * (1 << 29))) for c in nrm]
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
        for k in range(3):
            e.w32(seg + 4 * k, p0[k]); e.w32(seg + 12 + 4 * k, p1[k]); e.w32(seg + 24 + 4 * k, p1[k] - p0[k])
        e.w32(seg + 0x34, 0)
        e.call(0x1d960, eax=seg)
        if e.rs32(seg + 0x30) <= 0: continue
        dh = e.call(0x1c7a0, eax=poly, edx=vb, ebx=seg) != 0
        dpt = [e.rs32(seg + 12 + 4 * k) for k in range(3)]
        out = F3()
        vv = (ctypes.c_float * (3 * nv))(*[float(c) for vtx in verts for c in vtx])
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

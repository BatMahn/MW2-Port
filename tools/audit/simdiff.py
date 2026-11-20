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
    0x4a070 fed the port's own draw) vs ai.c start_manoeuvre (DOS), manoeuvres 0 1 4 8 10 12 with contact cases"""
    L = lib('ai'); L.shim_rand.restype = ctypes.c_uint
    L.shim_man_limit.restype = ctypes.c_float
    rv, js, jf = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
    for man in (0, 1, 4, 8, 10, 12):
        for case in range(n):
            e = emu()
            seed = rnd.randint(1, 1 << 30)
            prev = rnd.choice([0, 10, 3]); contact = rnd.randint(0, 1); cu = rnd.randint(0, 1)
            e.stub(RAND, lambda em, s=seed: L.shim_rand(s, em.reg('eax')))
            e.stub(0x23420, ret=0)
            u = e.alloc(0x200); ib = e.alloc(0x60); ctl = e.alloc(0x200)
            e.w32(u + 0x4c, ib); e.w32(u + 0x20, ctl); e.w32(ctl + 0xa4, contact)
            e.w8(u + 0x16c, man); e.w8(u + 0x16d, prev); e.w32(u + 0x7c, 3 if cu else 0xffffffff)
            e.w32(NOW, 1000); e.w32(TICKS, 6)
            e.call(0x2cbf0, eax=u)
            dl = e.rs32(u + 0x172) - 1000
            dr = e.r8(ib + 0x2f)
            pl = L.shim_man_limit(man, prev, contact, cu, seed, ctypes.byref(rv), ctypes.byref(js), ctypes.byref(jf), 0)
            rep.check(abs(dl - pl) < 0.5 and (man not in (8, 10) or dr == rv.value),
                      'man %d prev %d contact %d unit %d: dos limit %d reverse %d, port %.0f %d' % (man, prev, contact, cu, dl, dr, pl, rv.value))

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

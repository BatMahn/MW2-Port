#!/usr/bin/env python3
"""Sequence tests: drive the real in-mission program (glview) headless through scripted action sequences and check
the end states it dumps (the TEST ONLY hooks in tools/glview.c: MW2_STATE_DUMP, MW2_HOLD, MW2_SHOT, MW2_TEST_*).

    python3 tests/sequences/run_sequences.py [-j N] [--glview PATH] [--only NAME[,NAME]] [-v] [--list]

Environment: MW2_GAME_DIR (default ../MW2-game: 3d/models.prj, 3d/textures.prj, 3d/skygnd.par), MW2_GTEST (an
installed game directory with the CFG files, default /tmp/gtest), SEQ_OUT (work directory, default /tmp/mw2seq),
SEQ_TIME_SCALE (timeout multiplier, e.g. 4 under ASAN).
Prints PASS / FAIL per sequence; exits 1 if any failed (2 if the setup is missing).
"""
import argparse
import concurrent.futures as cf
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
GAME = os.environ.get('MW2_GAME_DIR', os.path.abspath(os.path.join(REPO, '..', 'MW2-game')))
GTEST = os.environ.get('MW2_GTEST', '/tmp/gtest')
OUT = os.environ.get('SEQ_OUT', '/tmp/mw2seq')
TSCALE = float(os.environ.get('SEQ_TIME_SCALE', '1'))
GLVIEW = os.path.join(REPO, 'glview')
VERBOSE = False

ASAN_RE = re.compile(r'ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|SUMMARY: UndefinedBehaviorSanitizer')


# ---------------------------------------------------------------- running the program

class Result:
    def __init__(self, name, out, rc, wall, shots):
        self.name, self.out, self.rc, self.wall, self.shots = name, out, rc, wall, shots
        self.states, self.msgs = [], []
        for line in out.splitlines():
            if line.startswith('STATE '):
                f = line.split()
                d = {'tag': f[1]}
                for kv in f[2:]:
                    k, _, v = kv.partition('=')
                    d[k] = v
                for k, v in list(d.items()):
                    if k in ('tag', 'weapons'):
                        continue
                    try:
                        d[k] = int(v, 0)
                    except ValueError:
                        try:
                            d[k] = float(v)
                        except ValueError:
                            pass
                d['w'] = [tuple(int(x) for x in s.split(':')) for s in d.get('weapons', '').split(';') if s]   # (id, state, ammo, loc)
                self.states.append(d)
            elif line.startswith('MSG ') or line.startswith('MSGV '):
                m = re.match(r'MSGV? now=(-?\d+)(?: voice=-?\d+)? (.*)$', line)
                if m:
                    self.msgs.append((int(m.group(1)), m.group(2).strip()))
        self.sanitizer = [l for l in out.splitlines() if ASAN_RE.search(l)]

    def at(self, t):
        """the first periodic dump at or after autopilot time t (else the last state)"""
        for s in self.states:
            if s['t'] >= t - 1e-3:
                return s
        return self.states[-1] if self.states else {}

    def before(self, t):
        """the last periodic dump at or before autopilot time t"""
        prev = self.states[0] if self.states else {}
        for s in self.states:
            if s['t'] > t + 1e-3:
                break
            prev = s
        return prev

    def span(self, t0, t1):
        return [s for s in self.states if t0 - 1e-3 <= s['t'] <= t1 + 1e-3]

    @property
    def last(self):
        return self.states[-1] if self.states else {}

    def tag(self, tag):
        for s in self.states:
            if s['tag'] == tag:
                return s
        return None

    def msg_time(self, text):
        """sim ms of the first message containing text (None if never)"""
        for now, m in self.msgs:
            if text.lower() in m.lower():
                return now
        return None


def write_userstar(path, skel, loadout):
    src = os.path.join(GTEST, 'USERSTAR.BWD')
    d = bytearray(open(src, 'rb').read())
    gps = d.find(b'GPS\0')
    # GPS chunk (shell layout): skeleton name at chunk +36, loadout at +45, 9 bytes each
    for off, s in ((gps + 36, skel), (gps + 45, loadout)):
        b = s.lower().encode()[:8]
        d[off:off + 9] = b + b'\0' * (9 - len(b))
    open(path, 'wb').write(d)


def install_dir(name, mech):
    d = os.path.join(OUT, name, 'game')
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    for e in os.listdir(GTEST):
        p = os.path.join(GTEST, e)
        if os.path.isdir(p) or os.path.islink(p):
            os.symlink(os.path.realpath(p), os.path.join(d, e))
        else:
            shutil.copy(p, os.path.join(d, e))
    if mech:
        write_userstar(os.path.join(d, 'USERSTAR.BWD'), *mech.split('/'))
    with open(os.path.join(OUT, name, 'mw2port.cfg'), 'w') as f:
        # hud_layout=original: the picture checks measure the original's 4:3 HUD (hud_layout_wide checks the port's wide one)
        f.write('game=%s\nrenderer=enhanced\ntextures=original\nhud_layout=original\n' % d)
    return d


def run(name, secs, throttle=-9, twist=-999, fire=0, jet=0, keys=(), hold=(), env=None, mech='timbrwlf/tbr00std',
        mission='YELLSCN1', shots=(), dump='/0.25', nostart=False, size=None, edition='enhanced', files=None, dos_look=False,
        look3dfx=False):
    """One headless mission run of `secs` autopilot seconds (1/30 s frames). throttle -9 / twist -999: the keys drive
    them; keys: [(t, 'key')] presses; hold: [(t0, t1, 'key')]; shots: [(t, 'label')] -> Result.shots[label] = ppm path"""
    os.makedirs(os.path.join(OUT, name), exist_ok=True)
    size = size or ('640x400' if shots else '320x200')   # the renderer is most of the cost: small unless pictures are checked
    gd = install_dir(name, mech)
    for fn, data in (files or {}).items():   # extra install files (e.g. MW2DIF.CFG)
        with open(os.path.join(gd, fn), 'wb') as f:
            f.write(data)
    e = dict(os.environ)
    for k in list(e):
        if k.startswith('MW2_'):
            del e[k]
    e.update({'MW2_CONFIG': os.path.join(OUT, name, 'mw2port.cfg'), 'MW2_INSTALL_DIR': gd, 'SDL_VIDEODRIVER': 'offscreen',
              'SDL_AUDIODRIVER': 'dummy', 'MW2_SIZE': size, 'MW2_SKYGND': os.path.join(GAME, '3d', 'skygnd.par'),
              'MW2_EDITION': edition, 'MW2_STATE_DUMP': dump,
              'MW2_AUTOPILOT': '%g,%g,%g,%d,%s,1,%d' % (secs, throttle, twist, fire, os.path.join(OUT, name, 'final.ppm'), jet)})
    e.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    if nostart:
        e['MW2_NO_STARTUP'] = '1'
    if keys:
        e['MW2_KEYS'] = ','.join('%g:%s' % (t, k) for t, k in sorted(keys, key=lambda x: x[0]))
    if hold:
        e['MW2_HOLD'] = ','.join('%g-%g:%s' % h for h in hold)
    sp = {}
    if shots:
        for t, label in shots:
            sp[label] = os.path.join(OUT, name, label + '.ppm')
            if os.path.exists(sp[label]):
                os.remove(sp[label])
        e['MW2_SHOT'] = ','.join('%g:%s' % (t, sp[l]) for t, l in shots)
    sp['final'] = os.path.join(OUT, name, 'final.ppm')
    if env:
        e.update({k: str(v) for k, v in env.items()})
    cmd = [GLVIEW, os.path.join(GAME, '3d', 'models.prj'), os.path.join(GAME, '3d', 'textures.prj'), 'ati', '@', mission]
    if dos_look:   # the DOS edition's look: the DOS MW2.PRJ as models and textures (scratch/s11/mtdos.sh)
        dprj = os.path.join(os.path.dirname(GAME.rstrip('/')), 'MECH2', 'MW2.PRJ')
        cmd = [GLVIEW, dprj, dprj, 'dos', '@', mission]
        e['MW2_EDITION'] = 'dos'
    if look3dfx:   # RENDERER = 3DFX: the 3Dfx archive's own textures (portcfg_edition: textures = models, kind 3dfx)
        cmd = [GLVIEW, os.path.join(GAME, '3d', 'models.prj'), os.path.join(GAME, '3d', 'models.prj'), '3dfx', '@', mission]
        e['MW2_EDITION'] = '3dfx'
    t0 = time.time()
    try:
        p = subprocess.run(cmd, env=e, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=(60 + secs * 3) * TSCALE)
        out, rc = p.stdout.decode('latin-1'), p.returncode
    except subprocess.TimeoutExpired as x:
        out, rc = (x.stdout or b'').decode('latin-1') + '\n[TIMEOUT]', -999
    with open(os.path.join(OUT, name, 'log.txt'), 'w') as f:
        f.write(' '.join('%s=%r' % (k, v) for k, v in sorted(e.items()) if k.startswith('MW2_')) + '\n' + out)
    return Result(name, out, rc, time.time() - t0, sp)


# ---------------------------------------------------------------- picture checks

def ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def count_px(path, pred, box=None):
    """pixels satisfying pred(r, g, b); box = (x0, y0, x1, y1) in fractions of the frame"""
    if not path or not os.path.exists(path):
        return -1
    w, h, px = ppm(path)
    x0, y0, x1, y1 = box or (0, 0, 1, 1)
    X0, X1, Y0, Y1 = int(x0 * w), int(x1 * w), int(y0 * h), int(y1 * h)
    try:
        import numpy as np   # optional: fast path
        a = np.frombuffer(px, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)[Y0:Y1, X0:X1].astype(np.int32)
        return int(np.count_nonzero(pred(a[..., 0], a[..., 1], a[..., 2])))
    except ImportError:
        pass
    n = 0
    for y in range(Y0, Y1):
        row = px[(y * w + X0) * 3:(y * w + X1) * 3]
        r, g, b = row[0::3], row[1::3], row[2::3]
        for i in range(len(r)):
            if pred(r[i], g[i], b[i]):
                n += 1
    return n


def hud_green(path):
    """HUD instrument pixels: the HUD's green (text, compass, bars)"""
    return count_px(path, lambda r, g, b: (g > 140) & (r < 120) & (b < 120))


# ---------------------------------------------------------------- sequences

SEQUENCES = []


def sequence(fn):
    SEQUENCES.append(fn)
    return fn


class Check:
    def __init__(self, name):
        self.name, self.fails, self.notes, self.results = name, [], [], []

    def __call__(self, cond, what, detail=''):
        if not cond:
            self.fails.append(what + (' [' + detail + ']' if detail else ''))
        return cond

    def note(self, s):
        self.notes.append(s)

    def run(self, suffix='', **kw):
        r = run(self.name + suffix, **kw)
        self.results.append(r)
        if r.rc == -999:
            self.fails.append('run %s timed out' % (self.name + suffix))
        elif r.rc not in (0, 42):
            self.fails.append('run %s exited with %d' % (self.name + suffix, r.rc))
        if r.sanitizer:
            self.fails.append('sanitizer: ' + ' | '.join(r.sanitizer[:3]))
        if not r.states:
            self.fails.append('run %s: no state dump (hooks missing or crash)' % (self.name + suffix))
            raise Abort()
        return r


class Abort(Exception):
    pass


UP = 9.0   # the mission's start-up is over (online 5.97-7.95 s)


def pulses(t0, t1, key='space', every=0.3, length=0.1):
    out, t = [], t0
    while t < t1:
        out.append((round(t, 3), round(t + length, 3), key))
        t += every
    return out


def walk_and_fire_after(c, r, t, what):
    """after time t: the full-throttle key, a held right turn and a trigger press were scheduled; check they acted"""
    a, b = r.at(t), r.at(t + 2.0)
    c(b.get('kmh', 0) > 30, what + ': throttle works', 'kmh %.1f -> %.1f' % (a.get('kmh', 0), b.get('kmh', 0)))
    c(abs(r.at(t + 3.2)['heading'] - r.at(t + 2.0)['heading']) > 10, what + ': leg turn works',
      'heading %.1f -> %.1f' % (r.at(t + 2.0)['heading'], r.at(t + 3.2)['heading']))
    c(r.at(t + 4.0)['fired'] > r.before(t + 3.25)['fired'], what + ': trigger fires',
      'volleys %d -> %d' % (r.before(t + 3.25)['fired'], r.at(t + 4.0)['fired']))


def controls_after(t):
    """keys / holds: throttle full at t, right turn t+2..t+3, trigger t+3.3..t+3.45"""
    return [(t, '0')], [(t + 2.0, t + 3.0, 'right'), (t + 3.3, t + 3.45, 'space')]


@sequence
def shutdown_s_s(c):
    """s shuts down (HUD dark, no turning, coasts to a stop); a second s powers up: HUD, throttle, turn and fire back"""
    k, h = controls_after(22)
    r = c.run(secs=27, keys=[(UP, '0'), (11, 's'), (14, 's')] + k, hold=[(12, 13, 'right')] + h,
              shots=[(10.5, 'up'), (13.5, 'down'), (21.5, 'back')])
    c(r.at(11.2)['shut'] == 1 and r.at(11.2)['pow'] == 3, 'shut down after s', str(r.at(11.2).get('pow')))
    c(r.at(13.5)['kmh'] < 3, 'coasts to a stop while shut down', 'kmh %.1f' % r.at(13.5)['kmh'])
    c(abs(r.at(13.2)['heading'] - r.at(11.9)['heading']) < 0.5, 'no leg turn while shut down')
    g_up, g_down, g_back = hud_green(r.shots['up']), hud_green(r.shots['down']), hud_green(r.shots['back'])
    c(g_up > 500 and g_down < g_up * 0.1, 'HUD dark while shut down', 'green px up %d down %d' % (g_up, g_down))
    c(r.at(14.2)['shut'] == 0, 'second s powers up', 'shut %d' % r.at(14.2)['shut'])
    c(r.at(21.5)['pow'] == 2, 'online again within 8 s', 'pow %d' % r.at(21.5)['pow'])
    c(g_back > g_up * 0.8, 'HUD back after restart', 'green px %d vs %d' % (g_back, g_up))
    c(r.at(21.5)['throttle'] == 0 and r.at(21.5)['kmh'] < 1, 'throttle at 0 after the restart (engine: cleared while down)',
      'throttle %.2f kmh %.1f' % (r.at(21.5)['throttle'], r.at(21.5)['kmh']))
    walk_and_fire_after(c, r, 22, 'after s/s restart')


@sequence
def shutdown_s_ctrl_s(c):
    """s, then Ctrl+s (STARTUP_MECH) powers up: HUD and controls back"""
    k, h = controls_after(22)
    r = c.run(secs=27, keys=[(11, 's'), (14, 'ctrl+s')] + k, hold=h, shots=[(10.5, 'up'), (21.5, 'back')])
    c(r.at(11.2)['shut'] == 1, 'shut down after s')
    c(r.at(14.2)['shut'] == 0 and r.msg_time('Powering up') is not None, 'Ctrl+s powers up')
    c(r.at(21.5)['pow'] == 2, 'online again', 'pow %d' % r.at(21.5)['pow'])
    g_up, g_back = hud_green(r.shots['up']), hud_green(r.shots['back'])
    c(g_back > g_up * 0.8, 'HUD back after restart', 'green px %d vs %d' % (g_back, g_up))
    walk_and_fire_after(c, r, 22, 'after s/Ctrl+s restart')


@sequence
def ctrl_s_during_powerdown(c):
    """Ctrl+s 0.3 s after s (the power-down animation still running): powers up, everything back"""
    k, h = controls_after(20)
    r = c.run(secs=25, keys=[(11, 's'), (11.3, 'ctrl+s')] + k, hold=h, shots=[(10.5, 'up'), (19.5, 'back')])
    c(r.at(11.2)['shut'] == 1, 'shut down after s')
    c(r.at(11.5)['shut'] == 0, 'Ctrl+s during the animation powers up', 'shut %d' % r.at(11.5)['shut'])
    c(r.at(19.5)['pow'] == 2, 'online again', 'pow %d' % r.at(19.5)['pow'])
    g_up, g_back = hud_green(r.shots['up']), hud_green(r.shots['back'])
    c(g_back > g_up * 0.8, 'HUD back', 'green px %d vs %d' % (g_back, g_up))
    walk_and_fire_after(c, r, 20, 'after a quick restart')


@sequence
def s_toggle_rapid(c):
    """s pressed 4 times in 1 s (down, up, down, up): ends up online with HUD and controls"""
    k, h = controls_after(21)
    r = c.run(secs=26, keys=[(11, 's'), (11.3, 's'), (11.6, 's'), (11.9, 's')] + k, hold=h, shots=[(10.5, 'up'), (20.5, 'back')])
    c(r.at(12.1)['shut'] == 0, 'up after an even number of s', 'shut %d' % r.at(12.1)['shut'])
    c(r.at(20.5)['pow'] == 2, 'online again', 'pow %d' % r.at(20.5)['pow'])
    g_up, g_back = hud_green(r.shots['up']), hud_green(r.shots['back'])
    c(g_back > g_up * 0.8, 'HUD back', 'green px %d vs %d' % (g_back, g_up))
    walk_and_fire_after(c, r, 21, 'after rapid toggling')


@sequence
def s_during_startup(c):
    """s during the mission's own start-up (t=3): shuts down; s again powers up; then online with controls"""
    k, h = controls_after(16)
    r = c.run(secs=21, keys=[(3, 's'), (6, 's')] + k, hold=h, shots=[(15.5, 'back')])
    c(r.at(3.2)['shut'] == 1, 's during start-up shuts down', 'shut %d pow %d' % (r.at(3.2)['shut'], r.at(3.2)['pow']))
    c(r.at(6.2)['shut'] == 0, 's again powers up')
    c(r.at(15.5)['pow'] == 2, 'online', 'pow %d' % r.at(15.5)['pow'])
    c(hud_green(r.shots['back']) > 500, 'HUD drawn', 'green px %d' % hud_green(r.shots['back']))
    walk_and_fire_after(c, r, 16, 'after a start-up interrupted')


def heat_pulses(t0, t1):
    return pulses(t0, t1, 'space', 0.25, 0.08)


@sequence
def heat_shutdown_cycle(c):
    """group fire everything repeatedly: 'Heat level critical' -> 'Shutdown sequence initiated' -> auto shutdown ~6 s
    later -> auto restart after cooling (heat <= 65 + 724 ticks) -> online; controls back"""
    r = c.run(secs=95, keys=[(8.5, 'backslash')], hold=heat_pulses(9, 13) + [(91, 92, 'right'), (92.3, 92.45, 'space')])
    t_warn, t_seq, t_down = r.msg_time('Heat level critical'), r.msg_time('Shutdown sequence initiated'), r.msg_time('Shutting down')
    c(t_warn is not None, '"Heat level critical" message')
    c(t_seq is not None, '"Shutdown sequence initiated" message')
    c(t_down is not None, '"Shutting down" message (automatic)')
    if t_warn and t_seq:
        c(t_warn <= t_seq, 'warning before the sequence', '%d / %d' % (t_warn, t_seq))
    if t_seq and t_down:
        c(5000 < t_down - t_seq < 7000, 'auto shutdown 1086 ticks (5.97 s) after the sequence starts', '%d ms' % (t_down - t_seq))
    downs = [s for s in r.states if s['shut'] == 1]
    c(bool(downs), 'shut down by heat')
    c(not any(s['dead'] for s in r.states), 'not destroyed')
    if downs:
        t_restart = next((s['t'] for s in r.states if s['t'] > downs[0]['t'] and s['shut'] == 0), None)
        c(t_restart is not None, 'restarts by itself after cooling')
        if t_restart:
            c(r.at(t_restart)['heat'] <= 66, 'restart only at heat <= 65', 'heat %.1f' % r.at(t_restart)['heat'])
            c(r.at(t_restart + 9)['pow'] == 2, 'online after the restart', 'pow %d' % r.at(t_restart + 9)['pow'])
            c(r.at(t_restart)['pend'] == 0, 'sequence cleared')
    c(abs(r.at(92.2)['heading'] - r.at(90.9)['heading']) > 5, 'leg turn works after the heat restart')
    c(r.at(93)['fired'] > r.before(92.25)['fired'], 'trigger works after the heat restart')


@sequence
def heat_manual_shutdown_in_sequence(c):
    """s during a heat shutdown sequence, then Ctrl+s: refused while the sequence runs (engine 0x1001adf0), but the mech
    restarts by itself after cooling"""
    r = c.run(secs=80, keys=[(8.5, 'backslash'), (14.5, 's'), (15.5, 'ctrl+s')], hold=heat_pulses(9, 13))
    c(r.at(14.7)['shut'] == 1, 's shuts down during the sequence')
    c(r.at(15.7)['shut'] == 1, 'Ctrl+s refused while the sequence is pending', 'shut %d pend %d' % (r.at(15.7)['shut'], r.at(15.7)['pend']))
    c(r.last['shut'] == 0 and r.last['pend'] == 0, 'restarts by itself after cooling', 'shut %d heat %.1f' % (r.last['shut'], r.last['heat']))
    c(not any(s['dead'] for s in r.states), 'not destroyed')


@sequence
def override_keep_firing(c):
    """override the sequence and keep firing everything: past 100 heat the ammo cooks off and the mech is destroyed
    within a bounded time; the mission then ends (player lost)"""
    r = c.run(secs=130, fire=1, keys=[(10, 'o')])
    t_ovr = r.msg_time('overridden')
    c(t_ovr is not None, '"Shutdown sequence overridden" message')
    c(not any(s['shut'] for s in r.states), 'never shuts down once overridden')
    dead = next((s for s in r.states if s['dead']), None)
    c(dead is not None, 'destroyed by the overheat')
    if dead:
        c(dead['t'] < 10 + 90, 'destroyed within 90 s of the override', 't %.1f' % dead['t'])
    ov = r.tag('over')
    c(ov is not None, 'mission over after the death (program left)')
    if ov:
        c(ov['lost'] == 1 or ov['outcome'] == 3, 'result: player lost / failed', 'outcome %d lost %d' % (ov['outcome'], ov['lost']))
        if dead:
            c(ov['t'] - dead['t'] < 15, 'end sequence ~10 s', '%.1f s' % (ov['t'] - dead['t']))


@sequence
def override_then_cool(c):
    """override, stop firing: no shutdown; the override clears once cool; heat back near 0; still alive"""
    r = c.run(secs=75, keys=[(8.5, 'backslash'), (11.5, 'o')], hold=heat_pulses(9, 11.4))
    c(r.at(11.4)['pend'] == 1, 'sequence pending before the override', 'pend %d heat %.1f' % (r.at(11.4)['pend'], r.at(11.4)['heat']))
    c(r.at(11.7)['ovr'] == 1, 'override set')
    c(not any(s['shut'] for s in r.states), 'no shutdown after the override')
    c(r.last['ovr'] == 0 and r.last['pend'] == 0, 'override and sequence cleared once cool', 'ovr %d pend %d heat %.1f' % (r.last['ovr'], r.last['pend'], r.last['heat']))
    c(not r.last['dead'], 'alive')


@sequence
def override_without_sequence(c):
    """o with no shutdown sequence: nothing (no override flag, no message)"""
    r = c.run(secs=12, keys=[(10, 'o')])
    c(r.last['ovr'] == 0 and r.msg_time('overridden') is None, 'override ignored with no sequence', 'ovr %d' % r.last['ovr'])


@sequence
def jets_until_empty(c):
    """Summoner: hold J until the tank is empty: climbs, then no more lift (falls back with the key held), the tank
    refills once released"""
    r = c.run(secs=60, mech='summoner/smn00std', hold=[(UP, 30, 'j')])
    s0 = r.at(UP - 0.3)
    c(s0['jets'] > 0 and s0['fuel'] > 1000, 'Summoner has jets and fuel', 'jets %d fuel %.0f' % (s0['jets'], s0['fuel']))
    peak = max(s['y'] for s in r.span(UP, 30))
    c(peak - s0['y'] > 500, 'climbs with J held', 'peak %+.0f cm' % (peak - s0['y']))
    empty = next((s for s in r.span(UP, 30) if s['fuel'] <= 0), None)
    c(empty is not None, 'tank empties while held')
    if empty:
        land = r.at(min(empty['t'] + 8, 29.5))
        c(land['y'] - s0['y'] < 100, 'back on the ground with J still held (no flight on an empty tank)', 'height %+.0f cm' % (land['y'] - s0['y']))
        flying = [s for s in r.span(empty['t'] + 8, 30) if s['vy'] > 0.5]
        c(not flying, 'no climbing on an empty tank')
    c(r.last['fuel'] > r.at(30.5)['fuel'] + 100, 'refuels after release', 'fuel %.0f -> %.0f' % (r.at(30.5)['fuel'], r.last['fuel']))


@sequence
def jets_blocked_when_down(c):
    """J held while shut down and during the start-up: no lift, no fuel used"""
    r = c.run(secs=20, mech='summoner/smn00std', keys=[(10, 's')], hold=[(2, 5, 'j'), (11, 14, 'j')])
    c(max(s['y'] for s in r.span(2, 6)) - r.at(1)['y'] < 10, 'no jets during the start-up')
    c(max(s['y'] for s in r.span(11, 15)) - r.at(10)['y'] < 10, 'no jets while shut down')
    c(r.at(14)['fuel'] >= r.at(11)['fuel'], 'no fuel burnt while shut down', '%.0f -> %.0f' % (r.at(11)['fuel'], r.at(14)['fuel']))


@sequence
def leg_lost(c):
    """left leg shot off at full speed: immobile, slows to a stop and stays put; no jets moving it; still turns its torso"""
    r = c.run(secs=22, keys=[(UP, '0')], env={'MW2_TEST_PHIT': '12:8:11:80'})
    s = r.at(12.2)
    c(s['gone'] & 0x80 and s['immobile'] == 1, 'left leg gone, immobile', 'gone 0x%x immobile %d' % (s['gone'], s['immobile']))
    c(not s['dead'], 'one leg: not destroyed')
    c(r.at(16)['kmh'] < 2, 'slows to a stop', 'kmh %.1f' % r.at(16)['kmh'])
    a, b = r.at(17), r.at(21)
    c(abs(a['x'] - b['x']) + abs(a['z'] - b['z']) < 50, 'stays put with full throttle', 'moved %d cm' % (abs(a['x'] - b['x']) + abs(a['z'] - b['z'])))


@sequence
def leg_lost_turning(c):
    """a lost leg: the leg-turn keys do not walk it round (12.5 % creep) either"""
    r = c.run(secs=22, env={'MW2_TEST_PHIT': '10:7:11:80'}, hold=[(13, 20, 'right')])
    a, b = r.at(13), r.at(20)
    moved = abs(a['x'] - b['x']) + abs(a['z'] - b['z'])
    c(r.at(10.2)['immobile'] == 1, 'right leg gone: immobile')
    c(moved < 50, 'no creeping forward while turning on one leg', 'moved %d cm, heading %.1f -> %.1f' % (moved, a['heading'], b['heading']))
    c(b['kmh'] < 1, 'speed 0', 'kmh %.1f' % b['kmh'])


@sequence
def both_legs_lost(c):
    """both legs: destroyed, mission over"""
    r = c.run(secs=30, env={'MW2_TEST_PHIT': '10:8:11:80,11:7:11:80'})
    c(r.at(10.2)['immobile'] == 1 and not r.at(10.2)['dead'], 'first leg: immobile')
    c(r.at(11.2)['dead'] == 1, 'second leg: destroyed', 'dead %d gone 0x%x' % (r.at(11.2)['dead'], r.at(11.2)['gone']))
    c(r.tag('over') is not None, 'mission over')


def weapon_ids(s, loc_mask):
    return [i for i, w in enumerate(s['w']) if loc_mask & (1 << w[3])]


@sequence
def arm_lost(c):
    """right arm shot off: its weapons are gone (state -1) and never fire again; the others still fire"""
    r = c.run(secs=20, keys=[(UP, 'backslash')], env={'MW2_TEST_PHIT': '10:5:11:80'}, hold=pulses(12, 14, 'space', 0.5, 0.1))
    s = r.at(10.2)
    c(s['gone'] & (1 << 4), 'right arm gone', 'gone 0x%x' % s['gone'])
    lost = weapon_ids(s, 1 << 4)
    c(bool(lost) and all(s['w'][i][1] == -1 for i in lost), 'right-arm weapons destroyed', str([s['w'][i] for i in lost]))
    fired_lost = [i for i in lost for st in r.span(10.2, 20) if st['w'][i][1] != -1]
    c(not fired_lost, 'lost weapons never come back')
    c(r.at(14.5)['fired'] > r.before(11.9)['fired'], 'the other weapons still fire')


@sequence
def side_torso_lost(c):
    """right torso shot off: the right arm goes with it; weapons there gone; their ammunition gone; can't fire them"""
    r = c.run(secs=20, keys=[(UP, 'backslash')], env={'MW2_TEST_PHIT': '10:2:11:120'}, hold=pulses(12, 16, 'space', 0.5, 0.1))
    s0, s = r.at(9.5), r.at(10.2)
    c(s['gone'] & 0x02 and s['gone'] & 0x10, 'right torso and right arm gone', 'gone 0x%x' % s['gone'])
    c(not s['dead'], 'not destroyed (a side torso)')
    lost = weapon_ids(s, 0x12)
    c(bool(lost) and all(s['w'][i][1] == -1 for i in lost), 'weapons there destroyed', str([s['w'][i] for i in lost]))
    ammo_lost = [i for i in lost if s0['w'][i][2] > 0 and s0['w'][i][0] != 0]   # LRM-20 ammo (ids 10001-10099) is exempt (engine 0x1001680a)
    c(all(s['w'][i][2] <= 0 for i in ammo_lost), 'ammunition of the lost weapons gone', str([s['w'][i] for i in ammo_lost]))
    c(not [i for i in lost for st in r.span(10.2, 20) if st['w'][i][1] != -1], 'lost weapons never fire again')
    c(r.at(16.5)['fired'] > r.before(11.9)['fired'], 'the remaining weapons fire')


@sequence
def ammo_critical_explosion(c):
    """machine-gun hits into the right arm, then the right torso (the MG ammunition), from random states that give an
    ammo critical (MW2_TEST_RNG): 'Internal ammo explosion detected', the bin's rounds x damage into the structure
    destroys the location, the machine gun fed by it loses its rounds; a side torso / arm: not destroyed. The bin's
    explosion (effect 7, 0x100167b0 -> 0x10045c30) is a damaging type: area damage on every location (0x10046700)"""
    # the player's armour carries the loader's x 4 (0x10041410): 3 x the MEK armour taken off first (MG hits before the
    # breach draw no random numbers), so the 60 hits at 10 s meet the MEK's armour and the same random states as before
    for suffix, rng, loc, mask, pre in (('_arm', 12347, 5, 0x10, 3 * 24), ('_torso', 12354, 2, 0x12, 3 * 25)):
        r = c.run(suffix, secs=12, env={'MW2_TEST_RNG': '9.9:%d' % rng, 'MW2_TEST_PHIT': '9:%d:10:%d,10:%d:10:60' % (loc, pre, loc),
                                        'MW2_DAMAGE_LOG': '1'})
        t = r.msg_time('ammo explosion')
        c(t is not None, suffix[1:] + ': "Internal ammo explosion detected"')
        # 0x100167b0: explosion 7 at the location's node (0x1000fec0), a damaging type: area damage on every location
        # (0x10046700) when the node is within the unit's radius + the effect's (545 + 141 cm) of its feet - the right
        # torso's node (5.5 m up) is, the arm's (8.7 m up) is not
        area = set(int(l.split()[5]) for l in r.out.splitlines() if l.startswith('damage area player'))
        if suffix == '_torso':
            c(area == set(range(1, 9)), 'torso: the bin\'s explosion (effect 7) gives area damage on every location',
              '%s' % sorted(area))
        s0, s1 = r.at(9.5), r.last
        c(s1['gone'] & mask == mask, suffix[1:] + ': location destroyed', 'gone 0x%x' % s1['gone'])
        lost = sum(a[2] - max(0, b[2]) for a, b in zip(s0['w'], s1['w']) if a[0] == 10)
        c(lost > 0, suffix[1:] + ': the machine gun ammunition is gone', str([w for w in s1['w'] if w[0] == 10]))
        c(not s1['dead'], suffix[1:] + ': not destroyed')


@sequence
def chain_fire(c):
    """chain fire (default): one press fires only the selected weapon; the selection moves on after the release"""
    r = c.run(secs=14, hold=[(10, 10.1, 'space'), (12, 12.1, 'space')])
    a, b = r.before(9.95), r.at(10.6)
    c(b['fired'] - a['fired'] == 1, 'one weapon fired per press', 'volleys %d -> %d' % (a['fired'], b['fired']))
    c(r.at(12.6)['fired'] - b['fired'] == 1, 'the second press fires one more', 'volleys %d -> %d' % (b['fired'], r.at(12.6)['fired']))
    c(b['sel'] != a['sel'], 'selection advances on release', 'sel %d -> %d' % (a['sel'], b['sel']))


@sequence
def group_fire(c):
    """group fire (\\): one press fires every ready weapon once (non-repeating ones stay recycling while held)"""
    r = c.run(secs=14, keys=[(9.5, 'backslash')], hold=[(10, 12, 'space')])
    a, b = r.before(9.95), r.at(10.4)
    c(r.msg_time('Group fire') is not None, '"Group fire" message')
    fired = [i for i in range(len(a['w'])) if a['w'][i][1] == 1 and b['w'][i][1] != 1]
    c(len(fired) == len([w for w in a['w'] if w[1] == 1]), 'every ready weapon fires', 'fired %s of %s' % (fired, a['w']))
    # LRM (weapon 0, non-repeating): one volley per press however long held
    lrm = [i for i, w in enumerate(a['w']) if w[0] == 0]
    for i in lrm:
        c(a['w'][i][2] - r.at(12)['w'][i][2] == 20, 'LRM: one volley per press while held', 'ammo %d -> %d' % (a['w'][i][2], r.at(12)['w'][i][2]))


@sequence
def repeat_while_held(c):
    """the machine gun (repeating) keeps firing while the trigger is held; stops on release"""
    r = c.run(secs=16, keys=[(9.5, 'backslash')], hold=[(10, 13, 'space')])
    mg = [i for i, w in enumerate(r.at(9.9)['w']) if w[0] == 10]
    c(bool(mg), 'Timber Wolf has machine guns')
    for i in mg:
        a, b, e = r.before(9.95)['w'][i][2], r.at(13)['w'][i][2], r.at(15.5)['w'][i][2]
        c(a - b > 20, 'MG fires repeatedly while held', 'ammo %d -> %d' % (a, b))
        c(b - e <= 2, 'MG stops on release', 'ammo %d -> %d' % (b, e))


@sequence
def out_of_ammo(c):
    """the two LRM 20 alone in fire group 1 (the rest moved to group 2), fired with weapon_fire_group_1 (NumLock) until
    empty: ammo 0, the weapons dead (state -1) and they stay dead; the rest untouched"""
    r = c.run(secs=100, env={'MW2_GROUPS': '2:0,1,2,3,4,5,6,7'}, hold=pulses(UP, 95, 'numlock', 1.0, 0.1))
    lrm = [i for i, w in enumerate(r.at(UP)['w']) if w[0] == 0]
    c(len(lrm) == 2, 'two LRM 20')
    for i in lrm:
        e = r.last['w'][i]
        c(e[2] == 0 and e[1] == -1, 'LRM %d empty and dead' % i, str(e))
        t_dead = next((s['t'] for s in r.states if s['w'][i][1] == -1), None)
        if t_dead:
            c(all(s['w'][i][1] == -1 for s in r.span(t_dead, 100)), 'LRM %d stays dead' % i)
    others = [i for i, w in enumerate(r.at(UP)['w']) if w[0] != 0]
    c(all(r.last['w'][i][1] == 1 for i in others), 'the other weapons were not fired', str([r.last['w'][i] for i in others]))


@sequence
def missile_lock(c):
    """LRM selected (7 x Enter in the Timber Wolf's panel order, during the start-up), a Nova 160 m ahead on open
    ground, held still by a test hook: on target (0x8000), locked (0x80) ~2 s later; the locked volley damages it;
    selecting a non-guided weapon drops the lock. The hold matters: missiles home on the target object's position (its
    feet, 0x10045910) with a 1-2 cm/tick^2 turn and a 1 m proximity hit, so a mech that jumps or sidesteps on the launch
    warning (0x10020ac0) can make a volley pass under / beside it - as in the original. (The Firemoth, actor 1, stands
    behind a 16 m rise from this standoff: the volley flies into the hill.)"""
    cyc = [(2 + 0.4 * i, 2.1 + 0.4 * i, 'enter') for i in range(7)]
    r = c.run(secs=19, env={'MW2_KILL_ACTOR': '0,9999', 'MW2_TEST_TARGET': '0', 'MW2_TEST_STANDOFF': 16000, 'MW2_TEST_HOLD_ACTOR': '0'},
              hold=cyc + [(11, 11.1, 'space'), (17, 17.1, 'enter')])
    s = r.at(UP)
    c(s['w'][s['sel']][0] == 0, 'LRM selected', 'sel %d %s' % (s['sel'], s['w'][s['sel']] if 0 <= s['sel'] < len(s['w']) else ''))
    on = next((x['t'] for x in r.states if x['lock'] & 0x8000), None)
    lk = next((x['t'] for x in r.states if x['lock'] & 0x80), None)
    c(on is not None, 'on target (0x8000)')
    c(lk is not None, 'locked (0x80)')
    if on is not None and lk is not None:
        c(1.5 < lk - on < 2.6, 'lock ~2 s after acquiring (0x16a ticks)', '%.2f s' % (lk - on))
    c(r.at(10.9)['lock'] & 0x80, 'still locked when fired', 'lock 0x%x' % r.at(10.9)['lock'])
    h0, h1 = r.at(10.9)['tgt_health'], r.at(16.9)['tgt_health']
    c(h1 < h0 - 10, 'the locked LRM volley hits the target', 'health %d -> %d, range %.0f cm' % (h0, h1, r.at(10.9)['tgt_range']))
    c(r.at(17.4)['lock'] & 0xc0 == 0 or r.at(17.4)['w'][r.at(17.4)['sel']][0] == 0, 'lock dropped with a non-guided weapon',
      'lock 0x%x sel %d' % (r.at(17.4)['lock'], r.at(17.4)['sel']))


@sequence
def target_cycling(c):
    """t / r / e / Ctrl+t: next, previous, nearest, none; only live enemies"""
    r = c.run(secs=14, keys=[(UP, 't'), (UP + 0.5, 't'), (UP + 1.0, 'r'), (UP + 1.5, 'e'), (UP + 2.0, 'ctrl+t'), (UP + 2.5, 'r')])
    t1, t2, t3, t4, t5, t6 = (r.at(UP + 0.2 + 0.5 * i)['tgt'] for i in range(6))
    c(t1 >= 0, 't selects a target', str(t1))
    c(t2 >= 0 and t2 != t1, 't again: another one', '%d %d' % (t1, t2))
    c(t3 == t1, 'r: back to the previous', '%d %d %d' % (t1, t2, t3))
    c(t4 >= 0, 'e: nearest enemy', str(t4))
    c(t5 == -1, 'Ctrl+t: none', str(t5))
    c(t6 >= 0, 'r from none selects one', str(t6))


@sequence
def target_mech_killed(c):
    """a targeted mech destroyed: the target is dropped (or moves to another live one), the lock clears"""
    r = c.run(secs=14, env={'MW2_KILL_ACTOR': '0,10.5', 'MW2_TEST_TARGET': '0'})
    c(r.at(10)['tgt'] == 0, 'target 0 selected', str(r.at(10)['tgt']))
    s = r.at(11.5)
    c(s['tgt'] == -1 or (s['tgt'] >= 0 and s['tgt_dead'] == 0), 'dead target dropped', 'tgt %d dead %d' % (s['tgt'], s['tgt_dead']))


@sequence
def target_mech_killed_hud_off(c):
    """the same with the HUD hidden (F11): the target must still be dropped (the engine clears it in the sim)"""
    r = c.run(secs=16, env={'MW2_KILL_ACTOR': '0,10.5', 'MW2_TEST_TARGET': '0'}, keys=[(10, 'f11')])
    s = r.at(12)
    c(s['hud_off'] == 1, 'HUD hidden')
    c(s['tgt'] == -1 or s['tgt_dead'] == 0, 'dead target dropped with the HUD hidden', 'tgt %d dead %d' % (s['tgt'], s['tgt_dead']))


@sequence
def target_mech_killed_while_down(c):
    """target killed while the player is shut down: dropped (lock target not a wreck)"""
    r = c.run(secs=16, env={'MW2_KILL_ACTOR': '0,11', 'MW2_TEST_TARGET': '0'}, keys=[(10, 's')])
    s = r.at(12.5)
    c(s['tgt'] == -1 or s['tgt_dead'] == 0, 'dead target dropped while shut down', 'tgt %d dead %d' % (s['tgt'], s['tgt_dead']))


@sequence
def target_structure_destroyed(c):
    """standing 60 m from the chem plant's Coolant tank: q (TARGET_AT_RETICLE, 0x1001d870) targets what the aim ray last
    struck (0x10044810 -> DAT_1025a664): the pipe (structure 5) running across in front of the tanks; group-fire volleys
    destroy it; the target is dropped (engine 0x10046a90 clears DAT_1025a664). (Before 2026-10-12 the port took the
    structure nearest the aim within 5 degrees, the tank 4)"""
    r = c.run(secs=20, env={'MW2_START': '315212,244208', 'MW2_START_HEADING': 0}, keys=[(UP - 0.3, 'q'), (UP, 'q'), (UP + 0.3, 'backslash')],
              hold=pulses(10, 18, 'space', 1.0, 0.1))
    c(r.at(UP + 0.2)['thing'] == 5, 'q targets the structure the reticle is on', 'thing %d' % r.at(UP + 0.2)['thing'])
    d = next((s for s in r.states if s['t'] > UP + 0.5 and s['thing'] == -1), None)
    c(d is not None and d['bld_destroyed'] > 0, 'destroyed by gunfire')
    if d:
        prev = [s for s in r.states if s['t'] < d['t']][-1]
        c(prev['thing'] == 5 and d['bld_destroyed'] > prev['bld_destroyed'], 'target dropped as it is destroyed',
          'thing %d -> %d, destroyed %d -> %d' % (prev['thing'], d['thing'], prev['bld_destroyed'], d['bld_destroyed']))


@sequence
def target_structure_destroyed_hud_off(c):
    """the same with the HUD hidden (F11) when it goes: the target must still be dropped"""
    r = c.run(secs=14, env={'MW2_START': '315212,244208', 'MW2_START_HEADING': 0, 'MW2_TEST_BLD_KILL': '11:5'}, keys=[(UP - 0.3, 'q'), (UP, 'q'), (10, 'f11')])
    c(r.at(UP + 0.2)['thing'] == 5, 'q targets the structure ahead')
    c(r.at(12)['thing'] == -1, 'target dropped with the HUD hidden', 'thing %d hp %d' % (r.at(12)['thing'], r.at(12)['thing_hp']))


@sequence
def vision_modes(c):
    """w image enhancement on / off, l light amplification on / off, w then l switches"""
    r = c.run(secs=14, keys=[(UP, 'w'), (UP + 0.5, 'w'), (UP + 1, 'l'), (UP + 1.5, 'l'), (UP + 2, 'w'), (UP + 2.5, 'l'), (UP + 3, 'l')])
    v = [r.at(UP + 0.2 + 0.5 * i)['vision'] for i in range(7)]
    c(v == [2, 0, 1, 0, 2, 1, 0], 'vision mode sequence', str(v))


@sequence
def wreck_in_image_enhancement(c):
    """image enhancement on a mech 60 m ahead: blue before; destroyed -> its wreck in dark red (60,0,0), not blue"""
    r = c.run(secs=16, env={'MW2_KILL_ACTOR': '0,11', 'MW2_VISION': 2}, shots=[(10.5, 'alive'), (14, 'wreck')])
    box = (0.42, 0.45, 0.6, 0.7)   # where the mech stands, inside the cockpit frame
    blue = lambda r_, g, b: (b > 150) & (r_ < 80) & (g < 120)
    wreck = lambda r_, g, b: (r_ >= 40) & (r_ <= 90) & (g < 20) & (b < 20)
    b0, b1 = count_px(r.shots['alive'], blue, box), count_px(r.shots['wreck'], blue, box)
    w1 = count_px(r.shots['wreck'], wreck, box)
    c(b0 > 200, 'live mech drawn blue', 'blue px %d' % b0)
    c(b1 < b0 * 0.2, 'wreck no longer blue', 'blue px %d -> %d' % (b0, b1))
    c(w1 > 50, 'dark red (60,0,0) drawn where it stood', 'dark red px %d' % w1)


@sequence
def fkey_displays(c):
    """F2 radar modes, x zoom, F3 satellite, F4 target display, F5 damage, F7/F8/F9 viewports, F11 HUD: each toggles and
    comes back; the HUD looks as before afterwards"""
    seq = [('f2', 'radar_mode', [2, 0, 1]), ('x', 'radar_i', [1, 0, 3, 2]), ('f3', 'satmap', [1, 0]), ('f4', 'tgtdisp', [2, 0, 1]),
           ('f5', 'dmg_off', [1, 0]), ('f7', 'vport', [1, 0]), ('f8', 'vport', [2, 0]), ('f9', 'vport', [3, 0]), ('f11', 'hud_off', [1, 0])]
    keys, checks, t = [], [], UP + 1
    for key, field, vals in seq:
        for v in vals:
            keys.append((t, key))
            checks.append((t + 0.15, key, field, v))
            t += 0.4
    r = c.run(secs=t + 1.5, keys=keys + [(t - 0.1, 'f7'), (t + 0.3, 'f3')], shots=[(UP + 0.5, 'before'), (t - 0.2, 'after'), (t + 0.2, 'f7'), (t + 0.6, 'sat')])
    bad = []
    for tt, key, field, v in checks:
        got = r.at(tt)[field]
        if got != v:
            bad.append('%s@%.1f %s=%s want %s' % (key, tt, field, got, v))
    c(not bad, 'F-key toggles', '; '.join(bad))
    g0, g1 = hud_green(r.shots['before']), hud_green(r.shots['after'])
    c(abs(g1 - g0) < 0.15 * g0, 'HUD as before after toggling everything back', 'green px %d -> %d' % (g0, g1))
    g2, g3 = hud_green(r.shots['f7']), hud_green(r.shots['sat'])
    c(g2 > 0.5 * g0 and g3 > 0, 'HUD drawn with the rear view / satellite map open', 'green %d / %d' % (g2, g3))


@sequence
def esc_menu_pause(c):
    """Esc opens the MAIN MENU: the simulation pauses (time, position stand still, keys like s ignored); Esc closes it
    and the game resumes"""
    r = c.run(secs=18, keys=[(UP, '0'), (11, 'esc'), (12, 's'), (14, 'esc')], shots=[(13, 'menu'), (15.5, 'after')])
    a, b = r.at(11.3), r.before(13.8)
    c(a['menu'] >= 1, 'menu open')
    c(abs(b['now'] - a['now']) < 50, 'simulation paused', 'now %d -> %d' % (a['now'], b['now']))
    c(abs(b['x'] - a['x']) + abs(b['z'] - a['z']) < 5, 'player does not move while paused')
    c(b['shut'] == 0, 's ignored while the menu is open')
    s = r.at(14.3)
    c(s['menu'] == 0, 'Esc closes the menu')
    c(r.at(16)['now'] > s['now'] + 1000, 'time runs again')
    c(r.at(16)['kmh'] > 20, 'walking again (throttle kept)', 'kmh %.1f' % r.at(16)['kmh'])
    c(hud_green(r.shots['after']) > 500, 'HUD back after the menu')


@sequence
def autopilot_engage_disengage(c):
    """a engages the nav autopilot (turns toward the nav point at the pilot's throttle); a again disengages; a manual leg
    turn also disengages"""
    r = c.run(secs=24, keys=[(UP, '0'), (UP + 0.5, 'a'), (15, 'a'), (17, 'a')], hold=[(20, 21, 'left')])
    c(r.at(UP + 0.7)['autopilot'] == 1, 'engaged', str(r.at(UP + 0.7)['autopilot']))
    c(r.msg_time('Autopilot engaged') is not None, '"Autopilot engaged" message')
    c(r.at(15.2)['autopilot'] == 0, 'a disengages')
    c(r.at(17.2)['autopilot'] == 1, 'a engages again')
    c(r.at(20.3)['autopilot'] == 0, 'a leg turn disengages')
    c(r.at(14)['kmh'] > 20, 'walks under autopilot')


@sequence
def autopilot_then_shutdown(c):
    """autopilot on, s: no steering while shut down; after the restart the autopilot is off (start-up clears it)"""
    r = c.run(secs=28, keys=[(UP, '0'), (UP + 0.5, 'a'), (12, 's'), (14, 's')])
    h1, h2 = r.at(12.5)['heading'], r.at(13.8)['heading']
    c(abs(h1 - h2) < 0.5, 'no autopilot steering while shut down', '%.1f -> %.1f' % (h1, h2))
    c(r.at(26)['autopilot'] == 0, 'autopilot off after the restart')


@sequence
def player_death(c):
    """the player destroyed (centre torso): the end sequence runs ~10 s and the mission ends with a loss"""
    r = c.run(secs=30, env={'MW2_TEST_PHIT': '10:3:11:200'})
    d = next((s for s in r.states if s['dead']), None)
    c(d is not None, 'destroyed')
    ov = r.tag('over')
    c(ov is not None, 'mission over (program left by itself)')
    if ov and d:
        c(5 < ov['t'] - d['t'] < 15, 'end sequence ~10 s', '%.1f s' % (ov['t'] - d['t']))
        c(ov['lost'] == 1 or ov['outcome'] == 3, 'result: lost', 'outcome %d lost %d' % (ov['outcome'], ov['lost']))
    res = os.path.join(OUT, 'player_death', 'game', 'MW2MSN.CFG')
    c(os.path.exists(res), 'results file written')


@sequence
def dead_player_controls(c):
    """keys after death do nothing harmful: s, Ctrl+s, o, j, fire, Esc during the end sequence"""
    r = c.run(secs=30, env={'MW2_TEST_PHIT': '10:3:11:200'}, keys=[(11, 's'), (11.5, 'ctrl+s'), (12, 'o'), (13, 'esc'), (13.5, 'a')],
              hold=[(12.5, 13, 'space'), (14, 15, 'j')])
    c(r.at(14)['menu'] == 0, 'Esc does nothing during the end phase', 'menu %d' % r.at(14)['menu'])
    c(r.at(15)['fired'] == r.at(12.2)['fired'], 'a dead mech does not fire', '%d -> %d' % (r.at(12.2)['fired'], r.at(15)['fired']))
    c(r.tag('over') is not None, 'mission over')


@sequence
def death_mission_failed(c):
    """the player destroyed (centre torso): the unresolved star fails at once - outcome 3, the table's failure line
    (YELLSCN1 gene001F) queued and played (DOS MW2.EXE 0x16e80, DOSBox: "Mission failed" 0.5 s after Ctrl+Alt+x); the
    camera leaves the cockpit for the spinning orbit (mode 1) with the HUD gone"""
    r = c.run(secs=19, env={'MW2_TEST_PHIT': '10:3:11:200', 'MW2_RADIO_TRACE': '1'})
    d = next((s for s in r.states if s['dead']), None)
    c(d is not None, 'destroyed')
    if not d:
        return
    a = r.at(d['t'] + 0.5)
    c(a['outcome'] == 3 and a['ending'] == 1, 'Mission failed at once', 'outcome %d ending %d' % (a['outcome'], a['ending']))
    c('radio gene001F' in r.out, 'the failure line queued (MTBL lose sound)')
    played = re.search(r'^RADIO now=(\d+) gene001F id=(\d+)', r.out, re.M)
    if played:   # the queue paces lines by the wall clock (a briefing line may still be playing when the run ends)
        c(int(played.group(2)) > 0, 'the failure line found in the archive', played.group(0))
    else:
        c.note('gene001F still queued behind another line at the end of the run')
    c(a['camode'] == 1, 'orbit camera after death', 'camode %d' % a['camode'])
    b = r.at(d['t'] + 3.0)
    ang = lambda s: math.atan2(s['cam_x'] - s['x'], s['cam_z'] - s['z'])
    turn = abs(math.degrees(ang(b) - ang(a))) % 360
    c(20 < min(turn, 360 - turn) < 180, 'the orbit turns by itself (0.25 deg a tick)', '%.0f deg in 2.5 s' % min(turn, 360 - turn))
    dist = math.hypot(b['cam_x'] - b['x'], b['cam_z'] - b['z'])
    c(800 < dist < 2500, 'orbit distance 3 x the radius', '%.0f cm' % dist)


@sequence
def override_death_mission_failed(c):
    """overheating to death after overriding (the user's report): the mission fails audibly"""
    r = c.run(secs=110, fire=1, keys=[(10, 'o')], env={'MW2_RADIO_TRACE': '1'})
    d = next((s for s in r.states if s['dead']), None)
    c(d is not None, 'destroyed by the overheat')
    if d:
        c(r.at(d['t'] + 0.5)['outcome'] == 3, 'Mission failed', 'outcome %d' % r.at(d['t'] + 0.5)['outcome'])
        c('radio gene001F' in r.out, 'the failure line queued')


@sequence
def eject_mission_failed(c):
    """ejecting (state 5): the star fails when the end flag is set, 0x38e ticks on (DOS 0x16e80)"""
    r = c.run(secs=22, keys=[(10, 'ctrl+alt+e')], env={'MW2_RADIO_TRACE': '1'})
    e = next((s for s in r.states if s['ejected'] == 1), None)
    c(e is not None, 'ejected')
    if e:
        c(r.at(e['t'] + 1)['outcome'] == 0, 'no result while the ejection camera runs', 'outcome %d' % r.at(e['t'] + 1)['outcome'])
        c(r.at(e['t'] + 6.5)['outcome'] == 3, 'Mission failed once the end flag is set', 'outcome %d' % r.at(e['t'] + 6.5)['outcome'])


@sequence
def wreck_debris(c):
    """a mech's death sequence (0x1001be40, once a frame): a few explosions a second, each type-3 explosion and each
    0x10b throwing four chunks, never more than the 32 chunk objects; chunks fall and come to rest"""
    r = c.run(secs=20, env={'MW2_KILL_ACTOR': '0,6', 'MW2_DEBRIS_TRACE': '2', 'MW2_FX_TRACE': '1'})
    ex = [float(m.group(1)) for m in re.finditer(r'^fx (\d+\.\d)s type 3 at', r.out, re.M)]
    seq = [t for t in ex if 6 <= t <= 16.5]
    c(8 <= len(seq) <= 80, 'type-3 explosions in the 10 s sequence: per frame, not per tick', '%d' % len(seq))
    # chunks (kinds 0 / 1) per trace; the blown-off parts (kind 2: a wreck's locations shot off by the area damage,
    # 0x10046700 on state 4) have their own pool (0x10015230, 128 bodies)
    counts = [blk.count(' kind 0 ') + blk.count(' kind 1 ') for blk in re.findall(r'^debris [\d.]+s: \d+ pieces.*$((?:\n  piece.*)*)', r.out, re.M)]
    c(counts and max(counts) > 4, 'chunks thrown', 'max %d' % (max(counts) if counts else -1))
    c(counts and max(counts) <= 32, 'at most 32 chunks', 'max %d' % (max(counts) if counts else -1))
    late = re.findall(r'^debris 1[89]\.\ds: .*$((?:\n  piece.*)*)', r.out, re.M)
    moving = sum(l.count('moving 1') for l in late)
    total = sum(l.count('piece') for l in late)
    c(total == 0 or moving < total, 'chunks come to rest', '%d of %d moving' % (moving, total))


@sequence
def death_red_flash(c):
    """the red palette flash at death (0x1001bd80 -> 0x1002ad50(0x11, 0x16a, 1)): a fade in over 181 ticks, back over
    181 (the 3D editions); and the death camera's servos (0x1002db70): the view glides out of the cockpit eye"""
    r = c.run(secs=16, keys=[(8, 'ctrl+alt+x')], dump='/0.1')
    m = re.findall(r'^FLASH now=(\d+) dur=(\d+) in=(\d+) back=(\d+)', r.out, re.M)
    c(len(m) == 1, 'one flash, at death', '%d' % len(m))
    if m:
        c(m[0][1:] == ('362', '181', '181'), 'death flash 0x16a: in / back 181 ticks', ' '.join(m[0][1:]))
    d = next((s for s in r.states if s['dead']), None)
    c(d is not None, 'destroyed')
    if not d:
        return
    dist = lambda s: math.hypot(s['cam_x'] - s['x'], s['cam_z'] - s['z'])
    a, b = r.at(d['t'] + 0.15), r.at(d['t'] + 3.0)
    c(dist(a) < 700, 'the camera starts at the eye', '%.0f cm at +0.15 s' % dist(a))
    c(1200 < dist(b) < 2200, 'then 3 x the radius out', '%.0f cm at +3 s' % dist(b))


@sequence
def damage_red_flash(c):
    """the damage flash (DAT_1024cce0): more than 2 points into the breached centre torso / head; n = 3 x the flashes so
    far, 0x16a x n / 15 (0x1001bda0): 72, 144, 217, 289 ticks, then a full 0x16a"""
    # the player's armour back to the MEK's points (without the loader's x 4) so 40 hits breach the centre torso
    r = c.run(secs=12, env={'MW2_TEST_PARMOR': '3:0.25', 'MW2_TEST_PHIT': '4:3:4:40,5:3:4:1,6:3:4:1,7:3:4:1,8:3:4:1,9:3:4:1,10:3:4:1,11:3:4:1'}, nostart=True)   # hits from 4 s: no damage before the first start-up (DAT_1024c570) otherwise
    durs = [int(x) for x in re.findall(r'^FLASH now=\d+ dur=(\d+)', r.out, re.M)]
    c(len(durs) >= 4, 'flashes on the breached torso', '%s' % durs)
    # the 4th hit kills: its flash is never taken (the cockpit update 0x1001e340 -> 0x1001e6c0 runs only outside
    # controller states 4 / 5) - the death's own 0x16a fade follows the third
    c(durs[:4] == [72, 144, 217, 362], 'durations grow with the count; none for the killing hit, then the death fade', '%s' % durs[:4])


@sequence
def enemy_blown_apart(c):
    """a mech killed through its centre torso: the hips carry everything, so every part flies off (0x10016640 ->
    0x1000ff70 -> 0x10015530); the parts fall and are gone after 0xe24 ticks; DOSBox: nothing is left standing"""
    r = c.run(secs=27, env={'MW2_KILL_ACTOR': '0', 'MW2_TEST_PASSIVE': '1', 'MW2_TEST_AHIT': '4:0:3:11:40',
                            'MW2_DEBRIS_TRACE': '2'}, nostart=True)   # the hit at 4 s: no damage before the first start-up otherwise (DAT_1024c570)
    def parts(t):
        m = re.search(r'^debris %s\.\ds: .*$((?:\n  piece.*)*)' % t, r.out, re.M)
        return m.group(1).count('kind 2') if m else -1
    c(parts(6) >= 15, 'the parts fly', '%d at 6 s' % parts(6))
    c(parts(26) == 0, 'gone 20 s later', '%d at 26 s' % parts(26))
    s = r.at(10)
    c(s['enemies_dead'] >= 1, 'destroyed')


@sequence
def aim_converges_on_reticle(c):
    """the player's shots converge on the reticle's line (0x10044950: the aim ray from the eye object, the camera's):
    a mech centred in the reticle at 100 m and at 300 m is hit within 30 cm of the reticle point - missiles (table type 3 /
    4) aside: they leave rand(200) - 100 cm off the mount (0x10044ac0) and fly parallel to their aim"""
    for k, d in ((0, 10000), (3, 30000)):
        r = c.run('_%d' % d, secs=10, fire=1, env={'MW2_TEST_AIM': '%d,%d,0' % (k, d), 'MW2_AIM_TRACE': '1',
                                                    'MW2_TEST_IMMOBILE': '0:%d' % k, 'MW2_DEBUG_INVULN': '1'})
        hits = [(float(m.group(1)), float(m.group(2))) for m in re.finditer(r'^AIMHIT now=\d+ unit range=(-?\d+) off_deg=[\d.]+ miss_cm=(\d+).* w=(\d+)$', r.out, re.M)
                if not (0 <= int(m.group(3)) <= 9 or int(m.group(3)) == 28)]
        c(len(hits) >= 3, '%d m: the mech hit' % (d // 100), '%d hits' % len(hits))
        if hits:
            worst = max(h[1] for h in hits[:12])
            c(worst <= 30, '%d m: hits on the reticle point' % (d // 100), 'worst miss %.0f cm' % worst)


@sequence
def throttle_response(c):
    """the engine's throttle -> speed chain (msim_drive_step: servos +0x44 / +0x24, velocity 1/45 per tick) against DOSBox
    YELLSCN1 (Timber Wolf, readout = 1.5 x the true speed; top 90 kph readout): FULL from rest 64 kph at 0.71 s, 88 at
    1.36 s (T 36.2 before the first 0x10 key); STOP from full: 70 % at 0.51 s, 23 % at 1.1 s; a second start is slower
    (T 90.5): 42 kph at 0.73 s. The stop is pressed at 11 s, on level ground: at 9.8-10.5 s the Timber Wolf runs off a
    crest and is airborne for 0.7 s, when the velocity is held (0x10019310: no walking approach in the air)"""
    r = c.run(secs=20, keys=[(5, '0'), (11, '1'), (16, '0')], dump='/0.1', nostart=True)
    k = lambda t: r.at(t)['kmh'] * 1.5
    c(abs(k(5.7) - 64) < 6, 'first start: 64 kph at 0.7 s', '%.1f' % k(5.7))
    c(abs(k(6.4) - 88) < 4, 'first start: 88 kph at 1.4 s', '%.1f' % k(6.4))
    full = k(10.9)
    c(abs(k(11.5) / full - 0.70) < 0.08 and abs(k(12.1) / full - 0.23) < 0.08, 'stop: 70 % at 0.5 s, 23 % at 1.1 s',
      '%.2f %.2f' % (k(11.5) / full, k(12.1) / full))
    c(abs(k(16.7) - 42) < 6, 'second start (T 90.5): 42 kph at 0.7 s', '%.1f' % k(16.7))


@sequence
def throttle_keys(c):
    """throttle presets, reverse, = / - ramp. REVERSE_DIRECTION (` / BSP, action 0x3f) toggles a flag (input +0x2f) the
    setting keeps: after a stop (1) = ramps it up still backwards; ` again goes forward (the throttle control spans
    0..1024 - no reverse below 0)"""
    r = c.run(secs=25, keys=[(UP, '5'), (12, '0'), (15, 'backquote'), (18, '1'), (21, 'backquote')],
              hold=[(19, 20, 'equal'), (22, 23, 'equal'), (23.5, 24.5, 'minus')])
    c(abs(r.at(11.5)['throttle'] - 4 / 9) < 0.01, 'preset 5 = 4/9', '%.2f' % r.at(11.5)['throttle'])
    c(abs(r.at(14.5)['throttle'] - 1017 / 1024) < 0.006, 'preset 0 = full (DOS 0x4684c: 9 x 113 / 1024)', '%.4f' % r.at(14.5)['throttle'])
    c(abs(r.at(17.5)['throttle'] + 1017 / 1024) < 0.006 and r.at(17.5)['kmh'] < 0, 'reverse', 'throttle %.2f kmh %.1f' % (r.at(17.5)['throttle'], r.at(17.5)['kmh']))
    c(r.at(18.5)['throttle'] == 0, 'preset 1 = stop')
    c(r.at(20.5)['throttle'] < -0.3, '= ramps it up, still in reverse', '%.2f' % r.at(20.5)['throttle'])
    c(r.at(23.2)['throttle'] > 0.3, 'reversed again: = forward', '%.2f' % r.at(23.2)['throttle'])
    c(r.at(24.9)['throttle'] == 0, '- stops at 0 (the control spans 0..1024)', '%.2f' % r.at(24.9)['throttle'])


LRM_SELECT = [(2 + 0.4 * i, 2.1 + 0.4 * i, 'enter') for i in range(7)]   # Timber Wolf panel order: the 8th is an LRM 20


@sequence
def volley_paused_by_shutdown(c):
    """an LRM 20 volley (20 missiles over 1.2 s) interrupted by s: no missiles leave while shut down nor during the
    start-up after Ctrl+s (engine 0x100437a0 runs only in state 2, up); once up the volley finishes, once"""
    r = c.run(secs=24, hold=LRM_SELECT + [(10, 10.1, 'space')], keys=[(10.45, 's'), (13, 'ctrl+s')])
    sel = r.at(UP)['sel']
    c(r.at(UP)['w'][sel][0] == 0, 'LRM selected')
    a0 = r.before(9.95)['w'][sel][2]
    a1 = r.at(10.6)['w'][sel][2]
    c(a0 > a1 > a0 - 20, 'volley interrupted part-way', 'ammo %d -> %d' % (a0, a1))
    c(r.before(12.95)['w'][sel][2] == a1, 'no missiles while shut down', 'ammo %d -> %d' % (a1, r.before(12.95)['w'][sel][2]))
    starting = [x for x in r.span(13, 24) if x['pow'] == 1]
    if starting:
        c(starting[-1]['w'][sel][2] == a1, 'no missiles during the start-up (state 1)',
          'ammo %d -> %d by t=%.1f, online at %.1f' % (a1, starting[-1]['w'][sel][2], starting[-1]['t'], starting[-1]['t'] + 0.25))
    c(r.last['w'][sel][2] == a0 - 20, 'the volley completes once', 'ammo %d -> %d' % (a0, r.last['w'][sel][2]))


@sequence
def override_then_manual_restart(c):
    """heat sequence, o (override), s, Ctrl+s: with the override (flag 8) the start-up is allowed (0x1001adf0)"""
    r = c.run(secs=30, keys=[(8.5, 'backslash'), (11.6, 'o'), (12.5, 's'), (13.5, 'ctrl+s')], hold=heat_pulses(9, 11.4))
    c(r.at(11.8)['ovr'] == 1, 'overridden', 'ovr %d pend %d heat %.1f' % (r.at(11.8)['ovr'], r.at(11.8)['pend'], r.at(11.8)['heat']))
    c(r.at(12.7)['shut'] == 1, 's shuts down')
    c(r.at(13.7)['shut'] == 0, 'Ctrl+s powers up (override set)', 'shut %d' % r.at(13.7)['shut'])
    c(r.at(23)['pow'] == 2, 'online', 'pow %d' % r.at(23)['pow'])


@sequence
def menu_pauses_shutdown_sequence(c):
    """the MAIN MENU open for 10 s during a heat shutdown sequence: the sequence's 1086 ticks count sim time only - the
    shutdown comes ~6 s of game time after it started, not while the menu is open"""
    r = c.run(secs=34, keys=[(8.5, 'backslash'), (11.6, 'esc'), (21.6, 'esc')], hold=heat_pulses(9, 11.4))
    t_seq = r.msg_time('Shutdown sequence initiated')
    c(t_seq is not None, 'sequence started before the menu')
    c(not any(s['shut'] for s in r.span(11.7, 21.5)), 'no shutdown while the menu is open')
    d = next((s for s in r.states if s['shut']), None)
    c(d is not None, 'shuts down after the menu closes')
    if d and t_seq:
        c(5000 < d['now'] - t_seq < 7000, 'after 1086 ticks of game time', '%d ms' % (d['now'] - t_seq))


@sequence
def jets_shutdown_midair(c):
    """Summoner jetting, s high up with J still held: no more thrust (it only decelerates and falls, no fuel burnt), it
    lands; Ctrl+s; after the start-up the jets work again"""
    r = c.run(secs=40, mech='summoner/smn00std', keys=[(12, 's'), (24, 'ctrl+s')], hold=[(UP, 20, 'j'), (34, 36, 'j')])
    y0 = r.at(UP - 0.2)['y']
    c(r.at(12)['y'] - y0 > 300, 'airborne when shut down', 'height %+.0f' % (r.at(12)['y'] - y0))
    down = [x for x in r.span(12.3, 20) if x['y'] - y0 > 100]   # airborne
    c(all(b['vy'] <= a['vy'] + 0.01 for a, b in zip(down, down[1:])), 'no thrust while shut down (vy only falls)')
    c(all(b['fuel'] >= a['fuel'] for a, b in zip(down, down[1:])), 'no fuel burnt while shut down')
    land = next((x for x in r.span(12.3, 24) if x['y'] - y0 < 50), None)
    c(land is not None, 'lands while shut down')
    c(not r.at(24)['dead'], 'survives the fall')
    c(max(x['y'] for x in r.span(34, 37)) - r.at(33.8)['y'] > 200, 'jets work after the restart')


@sequence
def hud_off_across_restart(c):
    """F11 hides the HUD, s / s restart, F11 again: the HUD comes back complete"""
    r = c.run(secs=22, keys=[(10, 'f11'), (11, 's'), (12, 's'), (21, 'f11')], shots=[(9.5, 'before'), (20.5, 'off'), (21.6, 'after')])
    g0, g1, g2 = hud_green(r.shots['before']), hud_green(r.shots['off']), hud_green(r.shots['after'])
    c(g1 < 0.1 * g0, 'HUD hidden', 'green %d / %d' % (g1, g0))
    c(g2 > 0.8 * g0, 'HUD back after F11', 'green %d / %d' % (g2, g0))


@sequence
def jettison_ammo(c):
    """k with an LRM 20 selected: its ammunition is dumped (0, weapon dead, 'Ammo for current weapon jettisoned.');
    firing it does nothing; k on a laser: nothing"""
    r = c.run(secs=14, hold=LRM_SELECT + [(11, 11.1, 'space')], keys=[(10, 'k')])
    sel = r.at(UP)['sel']
    w = r.at(10.3)['w'][sel]
    c(w[0] == 0 and w[2] == 0 and w[1] == -1, 'LRM ammunition jettisoned', str(w))
    c(r.msg_time('jettisoned') is not None, 'message')
    c(r.at(12)['fired'] == r.before(10.95)['fired'], 'a jettisoned weapon does not fire')


@sequence
def masc_not_fitted(c):
    """v on a mech without MASC: 'Not equipped with MASC.'; speed unchanged"""
    # v once the speed has settled: the engine's throttle chain (msim_drive_step) is still ~10 % short 1 s after FULL
    r = c.run(secs=14, keys=[(UP, '0'), (12, 'v')])
    c(r.msg_time('Not equipped with MASC') is not None, 'message')
    c(abs(r.at(13.5)['kmh'] - r.before(12)['kmh']) < 3, 'no MASC speed', '%.1f -> %.1f' % (r.before(12)['kmh'], r.at(13.5)['kmh']))


@sequence
def gamekey_actions_unhandled(c):
    """GAMEKEY.MAP actions the original has: PAUSE_GAME (Alt+p / Pause) pauses the simulation; EJECT (Ctrl+Alt+e) ends
    the mission; SELF_DESTRUCT (Ctrl+Alt+x) destroys the mech. F1 (MFD_CYCLE), F10 (ORDINANCE_VIEW), / (RESET_INPUTS):
    no crash"""
    r = c.run(secs=16, keys=[(UP, 'f1'), (UP + 0.2, 'f10'), (UP + 0.4, 'slash'), (10, 'alt+p'), (12, 'alt+p'), (13, 'ctrl+alt+x')])
    a, b = r.at(10.3), r.before(11.9)
    c(b['now'] - a['now'] < 100, 'Alt+p pauses the game (PAUSE_GAME)', 'now %d -> %d' % (a['now'], b['now']))
    # 3D editions: message 4, then state 7 for 0x16a ticks (~2 s, 0x1001a180 case 7) before 0x100174f0; DOS: at once
    c(r.at(14)['dead'] == 0 and any(m == 'Self-destruct sequence initiated' for _, m in r.msgs), 'Ctrl+Alt+x: self-destruct sequence initiated',
      'dead %d' % r.at(14)['dead'])
    c(r.at(15.5)['dead'] == 1, 'Ctrl+Alt+x self-destructs (SELF_DESTRUCT) ~2 s later', 'dead %d' % r.at(15.5)['dead'])
    r2 = c.run('_eject', secs=17, keys=[(10, 'ctrl+alt+e')])
    c(r2.at(16.5)['ending'] == 1 or r2.tag('over') is not None, 'Ctrl+Alt+e ejects (EJECT): the mission ends', 'ending %d' % r2.at(16.5)['ending'])


def career(r):
    """MW2CAR.CFG the run wrote (80 bytes) or None"""
    p = os.path.join(OUT, r.name, 'game', 'MW2CAR.CFG')
    return open(p, 'rb').read() if os.path.exists(p) else None


@sequence
def eject_camera(c):
    """EJECT (Ctrl+Alt+e, 0x100174f0 -> state 5): 'Ejecting'; camera mode 4 (0x1002e7d0) - straight down over the mech,
    climbing at 0.2368 cm/tick^2 from the eye (10 m in 0.5 s), turning; the mech stays (no death sequence); the end
    sequence starts 0x389 ticks (~5 s) later, the mission is over ~10 s after that; MW2CAR.CFG end status 2 and the
    player counted as ejected (+0x36), not lost (+0x34)"""
    r = c.run(secs=27, keys=[(10, 'ctrl+alt+e'), (10.5, 'c')], shots=[(10.6, 'up')])
    b, a = r.before(9.9), r.at(10.1)
    t_ej = r.msg_time('Ejecting')
    c(t_ej is not None, '"Ejecting" (message 0xc, voice 12)')
    c(a['camode'] == 4 and a['ejected'] == 1 and a['intact'] == 1 and a['dead'] == 1, 'state 5: ejected, camera mode 4',
      'camode %d ejected %d intact %d dead %d' % (a['camode'], a['ejected'], a['intact'], a['dead']))
    c(r.at(10.7)['camode'] == 4, 'COCKPIT_VIEW ignored once out (action 9: +0x14 & 2)', 'camode %d' % r.at(10.7)['camode'])
    y0 = b['cam_y']
    s1 = r.at(10.5 + 0.1)
    if t_ej is not None:
        def climb(s):
            t = (s['now'] - t_ej) * 0.182
            return 0.5 * 0.2368 * t * t, s['cam_y'] - y0
        e, g = climb(s1)
        c(abs(g - e) < 0.15 * e + 150, 'climb 0.5 s in: 0.5 x 0.2368 x t^2', 'expected %.0f got %.0f' % (e, g))
        e, g = climb(r.at(12))
        c(abs(g - e) < 0.1 * e + 300, 'climb 2 s in', 'expected %.0f got %.0f' % (e, g))
    c(abs(s1['cam_x'] - s1['x']) < 2 and abs(s1['cam_z'] - s1['z']) < 2, 'over the mech (x, z)')
    c(abs(r.at(12)['cam_yaw'] - b['cam_yaw']) > 5, 'the picture turns', 'yaw %.1f -> %.1f' % (b['cam_yaw'], r.at(12)['cam_yaw']))
    c(r.at(14.6)['ending'] == 0, 'no end sequence during the first 0x389 ticks', 'ending %d' % r.at(14.6)['ending'])
    c(r.at(15.5)['ending'] == 1, 'end sequence ~5 s after the ejection', 'ending %d' % r.at(15.5)['ending'])
    c(r.msg_time('Press CTRL-Q') is not None and t_ej is not None and 4700 < r.msg_time('Press CTRL-Q') - t_ej < 5300,
      '"Press CTRL-Q to exit..." 0x389 ticks after', '%s' % r.msg_time('Press CTRL-Q'))
    ov = r.tag('over') or (r.last if r.last.get('over') else None)
    c(ov is not None and ov['now'] - t_ej < 15500, 'mission over ~15 s after the ejection', '%s' % (ov and ov['now']))
    c(r.last['enemies_intact'] == 0 and r.at(20)['camode'] == 4, 'still the ejection camera in the end sequence')
    # the picture right after: no HUD (DOSBox), the mech below - its colours fill the middle, not the sand's
    up = r.shots.get('up')
    if up and os.path.exists(up):
        mid = count_px(up, lambda R, G, B: (abs(R - G) < 25) & (R < 120), box=(0.3, 0.3, 0.7, 0.7))
        c(mid > 0, 'the mech below the camera', 'dark px %d' % mid)
    car = career(r)
    c(car is not None and car[0x1d] == 2, 'MW2CAR.CFG end status 2 (ejected)', '%s' % (car and car[0x1d]))
    if car:
        c(car[0x36] == 1 and car[0x34] == 0, 'the player counted ejected (+0x36), not lost (+0x34)', '%d / %d' % (car[0x36], car[0x34]))


@sequence
def auto_eject(c):
    """TOGGLE_AUTOEJECT (Ctrl+e; DAT_1024bfbc, global): an AI mech's ammunition explosion (0x100167b0) puts it in
    state 5 and 0x10016280 sets state 4 - an ordinary death (wreck, death sequence), its pilot counted as ejected; off:
    the same explosion leaves it standing. The player (Summoner): ejects - camera mode 4, end status 2"""
    r = c.run(secs=16, keys=[(UP, 'ctrl+e')], env={'MW2_TEST_AMMO': '12:ai'})
    m = re.search(r'AMMOTEST now=\d+ unit=(-?\d+) crit=(\d) blown=(-?\d) dead=(-?\d) ejected=(-?\d) intact=(-?\d)', r.out)
    c(r.msg_time('Automatic ejection ON') is not None, '"Automatic ejection ON"')
    c(m is not None and m.group(2) == '1' and m.group(3) == '1', 'an AI ammunition bin exploded', m and m.group(0) or 'no AMMOTEST')
    if m:
        c(m.group(4) == '1' and m.group(5) == '1' and m.group(6) == '0', 'AI: out (state 5 -> 4), ejected, no intact state 5', m.group(0))
    # read just after the explosion: the wreck's own death blasts (area damage x2 at its centre, 0x10046700) may cook off
    # its other bins a second later, and a bin exploding in a wreck with auto-eject on leaves it in state 5 (0x100167b0
    # sets +0xa0 = 5 before 0x10016280, which returns for a unit already flagged dead)
    c(r.at(12.5)['enemies_dead'] == r.before(11.9)['enemies_dead'] + 1 and r.at(12.5)['enemies_intact'] == 0, 'one more enemy down, as a wreck')
    r2 = c.run('_off', secs=14, env={'MW2_TEST_AMMO': '12:ai'})
    m2 = re.search(r'AMMOTEST .* dead=(-?\d) ejected=(-?\d)', r2.out)
    c(m2 is not None and m2.group(2) == '0', 'auto-eject off: no ejection', m2 and m2.group(0) or 'no AMMOTEST')
    r3 = c.run('_player', secs=20, mech='summoner/smn00std', keys=[(UP, 'ctrl+e')], env={'MW2_TEST_AMMO': '12'})
    m3 = re.search(r'AMMOTEST now=\d+ unit=-1 crit=(\d) blown=(-?\d) dead=(-?\d) ejected=(-?\d) intact=(-?\d)', r3.out)
    if m3 and m3.group(1) == '1':
        c(m3.group(4) == '1' and m3.group(5) == '1', 'player: ejected (state 5 kept)', m3.group(0))
        c(r3.at(12.5)['camode'] == 4, 'ejection camera', 'camode %d' % r3.at(12.5)['camode'])
        c(r3.msg_time('Ejecting') is not None, '"Ejecting"')
    else:
        c.note('the Summoner has no ammunition that can explode: player check skipped (%s)' % (m3 and m3.group(0)))


@sequence
def ordinance_view(c):
    """ORDINANCE_VIEW (F10, action 0xe): nothing in flight - nothing happens; the newest projectile a missile in flight -
    camera mode 3 at it (0x1002e210, zoom 2.0; the HUD stays, no cockpit) until it is gone, then back to the cockpit;
    COCKPIT_VIEW (c) returns at once"""
    grp = {'MW2_GROUPS': '1:0,1,2,3,4,5,6,7;3:8,9'}   # Timber Wolf: the two LRM 20 alone in group 3 (keypad *)
    fire = [(6, 6.05, 'kpmultiply')]
    r = c.run(secs=14, nostart=True, env=grp, keys=[(2, 'f10'), (6.3, 'f10')], hold=fire, shots=[(6.6, 'ord')], size='640x400')
    c(r.at(2.3)['camode'] == 0, 'F10 with nothing in flight: no change', 'camode %d' % r.at(2.3)['camode'])
    a = r.at(6.5)
    c(a['camode'] == 3 and a['follow'] > 0, 'F10 with a missile in flight: the ordnance camera', 'camode %d follow %d sel %d' % (a['camode'], a['follow'], a['sel']))
    b = r.at(8)
    c(b['camode'] == 3 and math.hypot(b['cam_x'] - b['x'], b['cam_z'] - b['z']) > 5000, 'riding the missile away', 'camode %d' % b['camode'])
    c(r.last['camode'] == 0 and r.last['cam_y'] == r.before(5.9)['cam_y'], 'back in the cockpit once the missile is gone', 'camode %d' % r.last['camode'])
    shot = r.shots.get('ord')
    if shot and os.path.exists(shot):
        g = count_px(shot, lambda R, G, B: (G > 200) & (R < 140) & (B < 140))
        c(g > 200, 'the HUD stays (green)', 'green px %d' % g)
    r2 = c.run('_c', secs=9, nostart=True, env=grp, keys=[(6.3, 'f10'), (7, 'c')], hold=fire)
    c(r2.at(6.6)['camode'] == 3 and r2.at(7.3)['camode'] == 0, 'COCKPIT_VIEW returns to the cockpit', 'camode %d / %d' % (r2.at(6.6)['camode'], r2.at(7.3)['camode']))


# ---------------------------------------------------------------- main


def armor_loss(a, b, key):
    return sum(max(0.0, float(x) - float(y)) for x, y in zip(a[key].split(','), b[key].split(',')))


@sequence
def mech_lands_on_player(c):
    """YELLSCN1's Jenner (actor 2) dropped onto a standing player (MW2_TEST_DROP): dead centre it bounces on the
    player's sphere and stays up there (engine 0x1000ba20: a sphere contact, bounce n x max(1, dv / 2)) - the player
    takes nothing, the Jenner a little on its legs; landing off-centre within 10 m of the terrain it is set down beside
    with no damage to either (0x10019c4e); jumping into the player's side it hits once and bounces off (it had rammed
    again every few ticks while airborne, taking the arms off both) - only the Jenner, the mover, is damaged (0x10019d6a);
    with Collision Damage off (MW2DIF +3 = 0) no damage"""
    r = c.run(secs=14, throttle=0, env={'MW2_TEST_DROP': '10:2:0:1500:-5:0'})
    s0, s1 = r.at(9.9), r.last
    c(armor_loss(s0, s1, 'parmor') == 0 and s1['gone'] == 0, 'dropped dead centre: the player unhurt', 'loss %.1f' % armor_loss(s0, s1, 'parmor'))
    c(0 < armor_loss(r.at(10.1), s1, 'a_armor') < 2 and s1['a_gone'] == 0, 'the Jenner: a little leg damage',
      'loss %.1f %s' % (armor_loss(r.at(10.1), s1, 'a_armor'), s1['a_armor']))
    c(armor_loss(r.at(12), s1, 'a_armor') == 0, 'resting on the player: no further damage',
      'loss %.1f after 12 s' % armor_loss(r.at(12), s1, 'a_armor'))
    c(min(s['a_y'] for s in r.span(12, 14)) > 800, 'it stays on the player (sphere top), not inside it',
      'lowest %.0f cm' % min(s['a_y'] for s in r.span(12, 14)))
    r = c.run('_off', secs=13, throttle=0, env={'MW2_TEST_DROP': '10:2:400:1100:-6:0'})
    s1 = r.last
    c(armor_loss(r.at(9.9), s1, 'parmor') == 0 and armor_loss(r.at(10.1), s1, 'a_armor') == 0, 'landed off-centre: no damage')
    c(s1['a_y'] < 100 and math.hypot(s1['a_x'] - s1['x'], s1['a_z'] - s1['z']) > 900, 'set down beside the player',
      'y %.0f, %.0f cm away' % (s1['a_y'], math.hypot(s1['a_x'] - s1['x'], s1['a_z'] - s1['z'])))
    r = c.run('_side', secs=14, throttle=0, env={'MW2_TEST_DROP': '10:2:600:100:4:1500'})
    s1 = r.last
    c(armor_loss(r.at(9.9), s1, 'parmor') == 0 and s1['gone'] == 0, 'jumped into the side: the player (not the mover) unhurt',
      'loss %.1f gone 0x%x' % (armor_loss(r.at(9.9), s1, 'parmor'), s1['gone']))
    c(armor_loss(r.at(9.9), s1, 'a_armor') < 2 and s1['a_gone'] == 0, 'the Jenner (the mover) a small hit, no arms lost',
      'loss %.1f gone 0x%x' % (armor_loss(r.at(9.9), s1, 'a_armor'), s1['a_gone']))
    r = c.run('_nodmg', secs=14, throttle=0, env={'MW2_TEST_DROP': '10:2:600:100:4:1500'}, files={'MW2DIF.CFG': bytes([0, 0, 1, 0, 1, 1, 0, 0])})
    s1 = r.last
    c(armor_loss(r.at(9.9), s1, 'parmor') == 0 and armor_loss(r.at(9.9), s1, 'a_armor') == 0, 'Collision Damage off: none',
      'player %.1f jenner %.1f' % (armor_loss(r.at(9.9), s1, 'parmor'), armor_loss(r.at(9.9), s1, 'a_armor')))


@sequence
def mech_rams_mech(c):
    """the player walks into a standing Jenner (MW2_TEST_IMMOBILE) turned to heading ~96 (the engine's damage speed is
    |(dvx, dvy)|, 0x1000ba20 copies the other's z velocity): only the mover takes contact damage (0x10019d6a ->
    0x1000c160 damages the unit whose movement made the contact), the Jenner nothing"""
    h = math.radians(95.6)
    r = c.run(secs=15, hold=[(9.5, 10.3, 'left')], keys=[(11.2, '0')],
              env={'MW2_TEST_DROP': '11:2:%d:0:0:0:%d' % (2500 * math.cos(h), 2500 * math.sin(h)), 'MW2_TEST_IMMOBILE': '11:2'})
    pl, jl = armor_loss(r.at(10.9), r.last, 'parmor'), armor_loss(r.at(11.1), r.last, 'a_armor')
    c(pl > 0.3, 'the player (the mover) takes contact damage', 'loss %.1f' % pl)
    c(jl == 0, 'the standing Jenner takes none', 'loss %.1f' % jl)


@sequence
def display_damage_static(c):
    """display damage (0x1001e670 levels; 0x10011720 / 0x10021300 draws; DOSBox with all levels forced to 1): level 1 on
    the target viewer (13) and the viewport window (2) fuzzes in and out - static on about 30 % of the frames (on: 7 in
    10 clear it, off: 3 in 10 set it, per 1/30 s frame); the rest of the HUD (compass, weapons, radar, bars) does not
    flicker; level 2 draws normally, 3+ static for good; the radar (0) damaged refuses light amplification"""
    keys = [(9, 'e'), (9.2, 'f7'), (12, 'l')]
    shots = [(9.4, 'before')] + [(12.5 + 0.1 * i, 'd%d' % i) for i in range(6)]
    r = c.run(secs=15, keys=keys, env={'MW2_TEST_DISPLAY': '9.5:13:1,9.5:2:1,9.5:0:1'}, shots=shots)
    a, b = r.at(10), r.at(15)
    def frac(k):
        n0, n1 = [int(x) for x in a['snow_n'].split(',')], [int(x) for x in b['snow_n'].split(',')]
        o0, o1 = [int(x) for x in a['snow_on'].split(',')], [int(x) for x in b['snow_on'].split(',')]
        return (o1[k] - o0[k]) / max(1, n1[k] - n0[k]), n1[k] - n0[k]
    for k, what in ((0, 'viewport window'), (1, 'target viewer')):
        f, n = frac(k)
        c(n > 100 and 0.18 < f < 0.42, what + ': static on ~30 % of the frames at level 1', 'frac %.2f of %d' % (f, n))
    c(int(b['snow_flips'].split(',')[1]) - int(a['snow_flips'].split(',')[1]) > 20, 'target viewer fuzzes in and out', b['snow_flips'])
    c(r.at(12.5)['vision'] == 0, 'radar damaged: light amplification refused', 'vision %d' % r.at(12.5)['vision'])
    g0 = hud_green(r.shots['before'])
    gs = [hud_green(r.shots['d%d' % i]) for i in range(6)]
    c(min(gs) > g0 * 0.9, 'the other instruments do not flicker', 'green px %d vs %s' % (g0, gs))
    r = c.run('_lvl', secs=14, keys=[(9, 'e'), (9.2, 'f7')], env={'MW2_TEST_DISPLAY': '9.5:13:3,9.5:2:2,12:13:2,12:2:3'})
    on = [s['snow'] for s in r.span(10, 11.9)]
    c(all(x == '0,1' for x in on), 'level 3 target viewer: static for good; level 2 viewport: normal', str(sorted(set(on))))
    on = [s['snow'] for s in r.span(12.2, 14)]
    c(all(x == '1,0' for x in on), 'level 2 target viewer: normal; level 3 viewport: static', str(sorted(set(on))))



@sequence
def training_own_mech(c):
    """user report "you keep spawning a mech in the same spot as me" (Cadet Training): the training scenarios carry the
    cadet's mech in their own tree (TNx#USS1, the GPS with +0x0e == 0 - engine 0x1003f36b makes it the player whatever
    record holds it) and the shell's training launch writes no star file (FUN_000375a0 skips FUN_0003abb0 for screen 14).
    With a stale USERSTAR.BWD (Timber Wolf) on disk the port flew that and spawned the USS1 mech as an actor inside the
    player. Now: the USS1 mech is the player, nothing stands within 10 m of it"""
    for scene, skel, load in (('TNW1SCN1', 'FIREMOTH', 'FRM00TRN'), ('TNJ5SCN1', 'MADDOG', 'MDG01TRN'), ('TNW6SCN1', 'TIMBRWLF', 'TBR01TRN')):
        r = c.run('_' + scene, secs=2, mission=scene, mech='timbrwlf/tbr00std', nostart=True)
        s = r.at(1)
        c(s.get('near') == 0, scene + ': no unit within 10 m of the player at the start', 'near %s' % s.get('near'))
        c(s.get('pmech') == skel and s.get('pload') == load, scene + ': the player flies the mission\'s own mech ' + skel,
          '%s %s' % (s.get('pmech'), s.get('pload')))



@sequence
def mission_startup(c):
    """user question "are we sure the startup sequence matches dos for picture/sound?" - DOSBox YELLSCN1 / TNJ1SCN1 (video
    capture with audio, sounds identified by cross-correlation with the SNDS records; docs/reference/startup_dos_vs_port.png).
    The original: frame 1 MECTURX2 0xf7 (the player's state 0 -> 1, 0x1001ab71) and every non-enemy, non-resting unit's
    own at its position (TNJ1: the instructor - not the resting Timber Wolf), started distance / 331 m/s late; frame 2
    the opener line (yell00bS);
    the palette fades from black from frame 3 over 0x16a ticks in frame steps (2.0 s at 30 fps) - DOS look only: the 3D
    editions' set-palette driver entry is empty (0x10005b20), the picture is there at once; at online (state 2)
    MECBSYRX 0xce (0x1001e6c0) - no MECOMBEP; no TORSLOOP at the start; trn1_01S as its node completes (2 s); the target
    viewer black with no nav selected"""
    def sounds(r):
        out = []
        for l in r.out.splitlines():
            m = re.match(r'SFXLOG (-?[\d.]+) (snd|pcm) (\d+) vol ([\d.]+)( voice)?', l)
            if m:
                out.append((float(m.group(1)), m.group(2), int(m.group(3)), float(m.group(4)), bool(m.group(5))))
        return out
    r = c.run('_yell', secs=9.5, dump='/0.1', env={'MW2_SFX_LOG': '1'}, mission='YELLSCN1')
    snd = sounds(r)
    first = [x for x in snd if x[1] == 'snd']
    c(first and first[0][2] == 0xf7 and first[0][0] < 0.01, 'MECTURX2 on the first frame', str(first[:2]))
    op = [x for x in snd if x[2] == 338 and x[4]]
    c(op and op[0][0] < 0.1, 'the opener (YELL00BS) one frame after the power-up, not after 4 s', str(op[:1]))
    c(not [x for x in snd if x[2] == 232], 'no MECOMBEP')
    # (object sound loops, TSK type 4, start at volume 0 and are levelled by distance: not the torso loop)
    c(not [x for x in snd if x[1] == 'pcm' and x[0] < 1.0 and x[3] > 0], 'no torso loop at the start', str([x for x in snd if x[1] == 'pcm'][:2]))
    up = [s2 for s2 in r.states if s2.get('pow') == 2]
    bz = [x for x in snd if x[2] == 0xce]
    c(len(bz) == 1 and up and abs(bz[0][0] - up[0]['now'] / 1000.0) < 0.15, 'MECBSYRX once, as the mech comes online',
      'MECBSYRX %s, online %s' % (bz, up[0]['now'] if up else None))
    f0, f1 = r.at(0.05).get('fade'), r.at(1.0).get('fade')
    c(f0 == 1.0 and f1 == 1.0, '3D: no fade from black (empty Glide set-palette)', 'fade %s %s' % (f0, f1))
    rd = c.run('_dos', secs=2.5, dump='/0.1', mission='YELLSCN1', dos_look=True)
    f0, f1, f2 = rd.at(0.05).get('fade'), rd.at(1.0).get('fade'), rd.at(2.2).get('fade')
    c(f0 is not None and f0 < 0.05 and 0.4 < f1 < 0.6 and f2 == 1.0, 'DOS: palette fade from black over 2 s (frame steps)',
      'fade %s %s %s' % (f0, f1, f2))
    r2 = c.run('_tnj1', secs=3, dump='/0.5', env={'MW2_SFX_LOG': '1'}, mission='TNJ1SCN1')
    snd = sounds(r2)
    tu = [x for x in snd if x[2] == 0xf7]
    c(len(tu) == 2 and tu[1][0] - tu[0][0] < 0.01, 'TNJ1: MECTURX2 for the player and the instructor (not the resting Wolf), same frame', str(tu))
    dl = re.findall(r'SFXLOG [\d.]+ snd 247 vol [\d.]+ delay ([\d.]+)', r2.out)
    c(len(dl) == 1 and 0.14 < float(dl[0]) < 0.16, "TNJ1: the instructor's 50 m off: 0.15 s late (331 m/s; DOSBox 0.19 s)", str(dl))
    tr = [x for x in snd if x[1] == 'pcm' and x[4]]
    c(tr and 1.9 < tr[0][0] < 2.2, 'TNJ1: trn1_01S as its node completes (2 s)', str(tr[:1]))
    nr = r.out.count('music:')
    c(nr <= 1, 'music started once')
    c(not [x for x in snd if x[2] in (0xe5, 0xe6)], 'TNJ1: no touch-down (starts on the ground; DOSBox\'s first-frame MECMTNSF comes from its 1 s first frame)')


@sequence
def touchdown(c):
    """coordinator: the touch-down - YELLSCN1's start point (YELLST01 NAVP y 25 m) drops the player: MECMTNSF 0xe6 (fall
    speed up to 16.154 cm/tick, MECMTNHD 0xe5 faster; 0x10019e28 -> 0x1001c0a0, nothing below 5.384) and the camera jolt
    0x1001c120 (pitch +30 down / -15 up / 0 over 0.2 / 0.5 / 0.2 s, offsets -25 / +12.5 cm) at ~2.3 s; DOSBox: the
    sound 1.3 s after the first frame, the view tilting up (its frames skip the first keyframe). A jump-jet landing
    sounds and jolts the same way; the s shutdown sounds MECSHTD1 0xf2 and its Betty line waits for the opener; death
    MECTRDXX 0xf6"""
    def sounds(r):
        return [(float(m.group(1)), int(m.group(2))) for m in re.finditer(r'SFXLOG (-?[\d.]+) snd (\d+)', r.out)]
    r = c.run('_yell', secs=4, dump='/0.0333', env={'MW2_SFX_LOG': '1'}, mission='YELLSCN1')
    snd = sounds(r)
    td = [t for t, i in snd if i in (0xe5, 0xe6, 0xf0)]
    c(len(td) == 1 and [i for t, i in snd if i in (0xe5, 0xe6, 0xf0)][0] == 0xe6 and 2.1 < td[0] < 2.6,
      'YELL: MECMTNSF once as the dropped mech lands (~2.3 s)', str([(t, i) for t, i in snd if i in (0xe5, 0xe6, 0xf0)]))
    c(r.at(0.05)['y'] > 2400 and r.at(3.0)['y'] < 50, 'YELL: the player starts 25 m up and falls', 'y %s -> %s' % (r.at(0.05)['y'], r.at(3.0)['y']))
    jo = [s2['jolt'] for s2 in r.states if 2.3 < s2['t'] < 3.3]
    c(jo and max(jo) > 10 and min(jo) < -1, 'YELL: the camera jolt (down, then up)', 'jolt %.1f .. %.1f' % (max(jo or [0]), min(jo or [0])))
    c(r.at(3.6)['jolt'] == 0, 'YELL: the jolt over within 0.9 s')
    r3 = c.run('_whit', secs=9, dump='0.05,8.5', env={'MW2_SFX_LOG': '1'}, mission='WHITSCN1')
    hd = [t for t, i in sounds(r3) if i == 0xe5]
    c(hd and 3.0 < hd[0] < 4.2, 'WHIT: the 65 m drop lands hard (MECMTNHD; DOSBox 2.2 s after its first frame)', str(hd))
    c(r3.tag('at0').get('parmor') == r3.tag('at1').get('parmor'), 'WHIT: no damage before the first start-up is done (DAT_1024c570; DOSBox: display blue)',
      '%s -> %s' % (r3.tag('at0').get('parmor'), r3.tag('at1').get('parmor')))
    r2 = c.run('_jet', secs=22, mech='summoner/smn00std', dump='/0.1', env={'MW2_SFX_LOG': '1'}, keys=[(16, 's')],
               hold=[(UP, UP + 1.5, 'j')], nostart=False)
    snd = sounds(r2)
    land = [t for t, i in snd if i in (0xe5, 0xe6) and t > UP]
    c(land and land[0] < UP + 6, 'a jump-jet landing sounds MECMTNSF / MECMTNHD', str(land))
    c([t for t, i in snd if i == 0xf2 and t > 15.0], 's sounds MECSHTD1')


@sequence
def training_shell_launch(c):
    """the same through the real launch path: the shell's Cadet Training (screen 14, Jade Falcon) launches TNJ1 (TEST ONLY
    MW2_SHELL_TRAIN=1 presses NAV COMPUTER) with MW2_SIM running this glview; the stale USERSTAR.BWD on disk stays
    untouched (the original writes no star file for training) and the player is TNW1USS1's Firemoth with nobody on it"""
    shell = os.path.join(REPO, 'mw2shell')
    if not os.path.exists(shell):
        c.note('mw2shell not built: skipped')
        return
    os.makedirs(os.path.join(OUT, c.name), exist_ok=True)
    gd = install_dir(c.name, 'timbrwlf/tbr00std')
    us = os.path.join(gd, 'USERSTAR.BWD')
    before = open(us, 'rb').read()
    with open(os.path.join(OUT, c.name, 'mw2port.cfg'), 'w') as f:
        f.write('game=%s\nrenderer=enhanced\ntextures=original\n' % gd)
    e = {k: v for k, v in os.environ.items() if not k.startswith('MW2_')}
    e.update({'SDL_VIDEODRIVER': 'offscreen', 'SDL_AUDIODRIVER': 'dummy', 'MW2_CONFIG': os.path.join(OUT, c.name, 'mw2port.cfg'),
              'MW2_SAVE_DIR': gd, 'MW2_SHELL_SCREEN': '14', 'MW2_SHELL_CLAN': '1', 'MW2_SHELL_FRAMES': '3',
              'MW2_SHELL_SHOT': os.path.join(OUT, c.name, 'shell.ppm'), 'MW2_SHELL_TRAIN': '1',
              'MW2_SIM': 'MW2_STATE_DUMP=1 MW2_NO_STARTUP=1 MW2_SIZE=320x200 MW2_AUTOPILOT=1.5,0,0,0,%s,1 %s %s %s ati @ %%s' % (
                  os.path.join(OUT, c.name, 'final.ppm'), GLVIEW, os.path.join(GAME, '3d', 'models.prj'), os.path.join(GAME, '3d', 'textures.prj'))})
    try:
        p = subprocess.run([shell], env=e, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 * TSCALE)
        out = p.stdout.decode('latin-1')
    except subprocess.TimeoutExpired:
        c(False, 'shell run timed out')
        return
    with open(os.path.join(OUT, c.name, 'log.txt'), 'w') as f:
        f.write(out)
    r = Result(c.name, out, p.returncode, 0, {})
    c('launch: training TNJ1SCN1' in out, 'the shell launched TNJ1SCN1')
    c(open(us, 'rb').read() == before, 'USERSTAR.BWD untouched by the training launch')
    if not c(len(r.states) > 0, 'the mission ran (state dump)'):
        return
    s = r.states[0]
    c(s.get('near') == 0 and s.get('pmech') == 'FIREMOTH', 'the cadet Firemoth, nobody within 10 m', '%s near %s' % (s.get('pmech'), s.get('near')))

@sequence
def training_scrub_billboards(c):
    """user report "Nav points are black dots too?": the dark pyramids on the training ground were TNJ1SCRI's scrub
    (rocks and cacti, colour type 3 0x3240-0x3270) drawn as plain triangles in palette colour 0x32. The engine draws a
    type 3 polygon as a camera-facing square of bank-0 bitmap (w >> 4) & 0xff (0x10026fc7 -> 0x1002a8e0) - TNJ1SCRI's own
    BMID bank 0 entries (tan_rk, tan_rk2, rrock, cact_2). 9 scrub tiles x 26 sprites, each stored twice (both windings)
    and drawn once"""
    r = c.run(secs=1.5, mission='TNJ1SCN1', nostart=True, shots=[(1, 'scrub')], size='640x400')
    s = r.at(1)
    c(s.get('billboards') == 234, 'TNJ1: 234 scrub billboards (9 tiles x 26)', 'billboards %s' % s.get('billboards'))
    img = ppm(r.shots['scrub'])
    w, h, px = img
    dark = 0
    for y in range(int(h * 0.5), int(h * 0.65)):   # the ground below the horizon, right of the Kit Fox (old: 733 px)
        for x in range(int(w * 0.55), int(w * 0.72)):
            o = (y * w + x) * 3
            if px[o] < 40 and px[o + 1] < 40 and px[o + 2] < 60:
                dark += 1
    c(dark < 200, 'no near-black pyramids on the training ground', 'dark px %d' % dark)


@sequence
def object_tasks(c):
    """moving objects stay in the engine's collision list (0x1000fbe0 -> 0x10029c10 moves their collision entry with the
    node): GOLDDRP1's dropship (type 0 box) blocks a mech walking into it until it has risen above the mech's centre;
    TSK types 0-4: the training targets spin (type 0) and blink (type 1 colour frames), CYANARE1's monorail loops
    its sound MECMRAIL (type 4, a RIFF WAVE record) where it is"""
    # (5 m from the box: the start drop holds the velocity at 0 until touch-down, 0x10019310 - it reaches the box at
    # ~1.2 s, before the dropship lifts at ~1.8 s)
    r = c.run('_drop', secs=3.5, throttle=1, nostart=True, dump='/0.25', mission='GOLDSCN1',
              env={'MW2_START': '35000,-88500', 'MW2_START_HEADING': 0})
    z1, z2, z3 = r.at(1.5).get('z', 0), r.at(1.75).get('z', 0), r.at(3.5).get('z', 0)
    c(-87900 < z1 < -87650 and z2 <= z1 + 5, 'GOLD: the grounded dropship stops the walk at its box (no further in; the impact bounce, 0x1000b5e0, may ease it back)',
      'z %s %s' % (z1, z2))
    c(z3 > z2 + 500, 'GOLD: walked on under it once it lifted above the centre', 'z %s -> %s' % (z2, z3))
    r2 = c.run('_spin', secs=3.3, nostart=True, mission='TNJ1SCN1', shots=[(3.0, 'a'), (3.15, 'b')],
               env={'MW2_START': '76334,-3500', 'MW2_START_HEADING': 0})
    wa, ha, pa = ppm(r2.shots['a'])
    wb, hb, pb = ppm(r2.shots['b'])
    diff = 0
    for y in range(int(ha * 0.2), int(ha * 0.45)):   # the target ahead (its path keeps it in this box for the moment)
        for x in range(int(wa * 0.4), int(wa * 0.6)):
            o = (y * wa + x) * 3
            if abs(pa[o] - pb[o]) + abs(pa[o + 1] - pb[o + 1]) + abs(pa[o + 2] - pb[o + 2]) > 60:
                diff += 1
    c(diff > 150, 'TNJ1: the target spins / blinks between frames 0.15 s apart', 'changed px %d' % diff)
    r3 = c.run('_snd', secs=1, nostart=True, mission='CYANSCN1', env={'MW2_SFX_LOG': '1', 'MW2_START': '-96500,184000'})
    c(re.search(r'SFXLOG [\d.]+ pcm 38097 ', r3.out) is not None, 'CYAN: the monorail sound loop starts (MECMRAIL, 38097 samples)')


@sequence
def kph_readout_dos(c):
    """the cockpit "NN kph" against DOSBox YELLSCN1 (Timber Wolf, 0.9 g, FULL preset, steady; compass headings read from
    the tape): 90 / 84 / 82 / 91 kph at 158.25 / 142.5 / 128.25 / 193 degrees. The readout estimates |v| as
    (4 max + mid + min) / 4 of |vx|, |vy|, |vz| (DOS 0x32085), / 10002 truncated, x 1.5 truncated; the speed is DOS's
    integer c[0x22] (527 -> 585 on 0.9 g) x FULL = 9 x 113 / 65536 (0x4684c) = 9.078 cm/tick - 60 km/h (the 3Dfx floats,
    n / 576) read 91 / 84 / 84 / 91, and the port had rounded the last step (92 at the start heading)"""
    for h, want in ((158.25, 90), (142.5, 84), (128.25, 82), (193, 91)):
        r = c.run('_%d' % int(h * 100), secs=10, keys=[(5, '0')], dump='/1', nostart=True,
                  env={'MW2_KPH_TRACE': '1', 'MW2_START': '360067,347177', 'MW2_START_HEADING': h})
        got = [int(m.group(1)) for m in re.finditer(r'^kph (?:9|10)\.\d+s (-?\d+) kph', r.out, re.M)]
        # walking off a bump or down a slope the mech is airborne for a moment (no snap-down, 0x10019e28) and the readout
        # takes the falling speed too (|v| of all three parts): a few frames read 1-3 more
        c(got and min(got) == want and max(set(got), key=got.count) == want and max(got) <= want + 3, 'heading %g: %d kph' % (h, want),
          str(sorted(set(got))))


@sequence
def terrain_impact(c):
    """walking into a wall (YELLARE6's S_BCHEMA, a type 0 box, head-on from the north at FULL): the engine's impact
    (0x1000b5e0 world hit -> 0x10019310): the move stops at the touching point, the velocity is reflected and cut to a
    quarter (x -0.25 of r = d - 2 (d . n) n: head-on a quarter forward), collision damage at the attempted speed (59.5
    km/h = 9.08 cm/tick -> (9.08 - 3.069) x 0.0501 x 3 = 0.9) on every contact frame (0x10019d84 -> 0x1000c3c0); pressing
    on, the speed builds back by 1/45 a tick and is cut again - it settles about 2.6 km/h, under the damage threshold, so
    one hit (the port had kept full speed against the wall, with a once-per-contact damage latch)"""
    r = c.run(secs=13, keys=[(5, '0')], dump='/0.1', nostart=True, env={'MW2_START': '319122,262000', 'MW2_START_HEADING': 180})
    hit = next((s['t'] for s in r.span(10, 13) if s['kmh'] < 10), None)
    c(hit is not None, 'stopped at the box', 'kmh %s' % [round(s['kmh'], 1) for s in r.span(11.5, 12.2)])
    if hit is None:
        return
    held = r.span(hit + 0.2, hit + 1.0)
    c(all(1.5 < s['kmh'] < 4 for s in held), 'pressing on: about 2.6 km/h', str(sorted(set(round(s['kmh'], 1) for s in held))))
    c(max(s['z'] for s in held) - min(s['z'] for s in held) < 5 and min(s['z'] for s in held) > 251104 + 500,
      'held at the touching point (the box ends at z 251104, radius 545)', '%d..%d' % (min(s['z'] for s in held), max(s['z'] for s in held)))
    sa, sb = r.at(hit - 0.15), r.at(hit + 0.7)
    # YELL's enemy star now comes for the player (node targets, 0x10014ba0) and its hits (whole points, front or rear) land
    # in this window too: count only the fractional per-location losses - the impact's (v - 3.069) x 0.0501 x 3
    d = [max(0.0, float(x) - float(y)) for x, y in zip(sa['parmor'].split(','), sb['parmor'].split(','))]
    loss = sum(x for x in d if abs(x - round(x)) > 0.01)
    c(0.7 < loss < 1.1, 'one impact: 0.9 armour', 'impact loss %.2f (all losses %s)' % (loss, [round(x, 2) for x in d]))



@sequence
def ai_self_destruct(c):
    """An AI mech out of weapons (0x10020880) with a top speed (+0x88) picks, unless it jumps (rand(4) != 0, jets):
    rand(2) == 0 flee, else - pilot level 1 only (+0x19e bit 0x20) - manoeuvre 6, a full-throttle charge (0x1001fb90)
    whose end within 20 m sets input +0x43, the self-destruct: 3D editions state 7 for 0x16a ticks (1.99 s) then the
    ordinary death (0x1001a180 -> 0x100174f0(c, 0) -> 0x10016280); DOS at once (0x27785 -> 0x265f0). No message for
    an AI unit. YELL's Jenner (actor 2) disarmed at 12 s, made level 1 without jets (test hook)"""
    env = {'MW2_TEST_DISARM': '12:2:1', 'MW2_START': '330000,262000', 'MW2_START_HEADING': 180, 'MW2_TEST_RNG': '11.9:1'}
    def times(r):
        a = re.search(r'^\s*([\d.]+)s .*\(YELLENS3\) self-destruct \(input \+0x43\)', r.out, re.M)
        b = re.search(r'^\s*([\d.]+)s .*\(YELLENS3\) self-destructs - DESTROYED', r.out, re.M)
        return (float(a.group(1)) if a else None), (float(b.group(1)) if b else None)
    r = c.run(secs=26, env=env)
    t0, t1 = times(r)
    c(t0 is not None and t1 is not None, 'charge ends, self-destruct taken and the Jenner destroyed', '%s / %s' % (t0, t1))
    if t0 is not None and t1 is not None:
        c(1.8 <= t1 - t0 <= 2.2, 'state 7: destroyed 0x16a ticks after input +0x43', '%.1f s' % (t1 - t0))
        c(r.at(t1 + 1.5)['enemies_dead'] == r.before(t0)['enemies_dead'] + 1, 'one more enemy down (a wreck)')
    c('Self-destruct sequence' not in r.out, 'no message for an AI unit')
    r2 = c.run('_dos', secs=26, env=env, edition='dos')
    d0, d1 = times(r2)
    c(d0 is not None and d1 is not None and abs(d1 - d0) < 0.05, 'DOS: destroyed at once', '%s / %s' % (d0, d1))


@sequence
def eject_view_distance(c):
    """ejection, the DOS look (DOSBox YELLSCN1): from high up the ground is the flat fill alone - objects wholly beyond the
    planet's VIEW far (YELLPLT1 50000 cm) are not drawn (DOS 0x3f500 / 3Dfx 0x1002fba0); without the cull the terrain
    pieces still show; leaving, the picture fades to black over 0x5a steps (DOS 0x28980, ~1.3 s) before the results"""
    def spread(path):
        w, h, px = ppm(path)
        import numpy as np
        a = np.frombuffer(px, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)[int(h * 0.15):int(h * 0.85)].astype(np.int32)
        return int((a.max(axis=(0, 1)) - a.min(axis=(0, 1))).max())   # per channel
    shots = [(UP + 7, 'high')]
    # the DOS end (0x16e80): "Press any key to exit..." once the star is resolved (state 5: 0x38e ticks on) and the
    # radio idle (voice off), a key then ends the mission
    r = c.run(secs=UP + 22, keys=[(UP, 'ctrl+alt+e'), (UP + 12, 'space')], shots=shots, dos_look=True, size='320x200',
              files=voice_off_cfg())
    c(r.at(1)['viewfar'] == 50000, 'VIEW far read from the planet record', 'viewfar %s' % r.at(1).get('viewfar'))
    sp = spread(r.shots['high']) if os.path.exists(r.shots['high']) else -1
    c(0 <= sp <= 12, '7 s after ejecting: the flat ground fill only (DOSBox: 125, 109, 73)', 'colour spread %d' % sp)
    r2 = c.run('_nocull', secs=UP + 8, keys=[(UP, 'ctrl+alt+e')], shots=shots, dos_look=True, size='320x200', env={'MW2_NO_FAR_CULL': '1'})
    sp2 = spread(r2.shots['high']) if os.path.exists(r2.shots['high']) else -1
    c(sp2 > 12, 'without the cull the terrain still shows (the check sees the difference)', 'colour spread %d' % sp2)
    o = next((s for s in r.states if s.get('over') == 1), None)
    t = r.tag('over')
    c(o is not None and t is not None and t['t'] - o['t'] >= 1.0, 'DOS: the exit fade runs before leaving',
      'over at %s, left at %s' % (o and o['t'], t and t['t']))
    c(any(0 < s.get('exitfade', 0) < 1 for s in r.states), 'the fade level passes between 0 and 1')


@sequence
def powerup_windows_clipped(c):
    """shutting down / powering up (0x10021730 / 0x100217f0, 0x10011aa0 / 0x10011b70 -> 0x100017f0): the target display
    and the viewport draw their contents into the moving box - a selected nav point's emblem centred and clipped, the
    rear view at its full scale clipped to the box (DOSBox YELLSCN1: emblem and REAR view inside the squashing boxes);
    a mech target's model only with the mech up (0x100216a2)"""
    yellow = lambda r, g, b: (r > 150) & (g > 110) & (b < 30)   # the emblem (188, 144, 0); the sand has more blue
    box = (0.045, 0.765, 0.19, 0.86)                              # inside the target display's box (1024x768)
    r = c.run(secs=UP + 3, keys=[(UP - 2, 'n'), (UP, 's')], shots=[(UP - 0.5, 'up'), (UP + 0.15, 'closing')],
              env={'MW2_VPORT': '1'}, size='1024x768')
    c(r.at(UP - 1)['nav'] >= 0, 'a nav point selected', 'nav %d' % r.at(UP - 1)['nav'])
    up, cl = count_px(r.shots['up'], yellow, box), count_px(r.shots['closing'], yellow, box)
    c(up > 10, 'the nav emblem in the target display', '%d px' % up)
    c(cl > 10, 'the nav emblem still drawn while the box closes', '%d px' % cl)


@sequence
def standing_wreck_frozen(c):
    """a mech destroyed without its centre torso (head / cockpit, heat, ammunition, self-destruct) stays standing in the
    pose it died in: no ANIM record has a fall sequence (every mech: walk 0, run 1, reverse 2), the death hook at state 4
    entry (0x1000ad40, DOS 0x17280) is an empty stub, and DOSBox (YELL, the Mad Dog self-destructed at full speed) shows
    it mid-stride, one foot raised, ~8 s later. An AI Nova killed by the head walking: only the head goes, the walk key
    stays put; the player's walk key stays put after the self-destruct"""
    r = c.run(secs=19, throttle=0, nostart=True, dump='/0.5',
              env={'MW2_KILL_ACTOR': '0', 'MW2_TEST_PASSIVE': '1', 'MW2_TEST_STANDOFF': '30000', 'MW2_TEST_AHIT': '13:0:1:11:40'})
    a, b = r.at(12.0), r.at(12.5)
    c(a.get('k_key', 0) != b.get('k_key', 0), 'the Nova walks before the hit', '%s -> %s' % (a.get('k_key'), b.get('k_key')))
    d = next((s for s in r.states if s.get('k_dead')), None)
    c(d is not None, 'the Nova is destroyed by the head hit')
    if not d:
        return
    e = r.at(d['t'] + 5.5)
    c(e.get('k_gone') == 0x01, 'only the head is gone (the mech stays: no centre torso)', 'gone %#x' % e.get('k_gone', -1))
    c(e.get('k_key') == d.get('k_key') and e.get('k_key', 0) > 0, 'its walk key is frozen at death', '%s -> %s' % (d.get('k_key'), e.get('k_key')))
    r2 = c.run('_player', secs=11, keys=[(0.2, '0'), (3, 'ctrl+alt+x')], nostart=True, dump='/0.5')
    d2 = next((s for s in r2.states if s['dead']), None)
    c(d2 is not None, 'the player self-destructs')
    if not d2:
        return
    w = [s['pkey'] for s in r2.states if s['t'] < d2['t']]
    c(len(set(w)) > 2, 'the player walks before', '%s' % w[-3:])
    e2 = r2.at(d2['t'] + 5.0)
    c(e2.get('pkey') == d2.get('pkey'), "the player's walk key is frozen at death", '%s -> %s' % (d2.get('pkey'), e2.get('pkey')))



# ---------------------------------------------------------------- the Titanium look (the user's Titanium disc files)

def titanium_dir():
    """the Titanium edition's 16-bit files (MW2.PRJ + SKYGND.PAR): the game folder's titanium/, or the test install's"""
    for d in (os.path.join(GAME, 'titanium'), os.path.join(GTEST, 'titanium')):
        if os.path.exists(os.path.join(d, 'MW2.PRJ')) and os.path.exists(os.path.join(d, 'SKYGND.PAR')):
            return os.path.realpath(d)
    return None


def titanium_cfg(name, tdir):
    """a configuration naming the Titanium folder (run() writes the run's own beside it)"""
    p = os.path.join(OUT, name, 'ti.cfg')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w') as f:
        f.write('game=%s\nrenderer=enhanced\ntextures=original\ntitanium=%s\n' % (os.path.join(OUT, name, 'game'), tdir))
    return p


def mean_diff(p1, p2, box=(0, 0, 1, 0.45)):
    """mean absolute RGB difference of two shots over box (fractions of the frame); -1 if either is missing"""
    if not (p1 and p2 and os.path.exists(p1) and os.path.exists(p2)):
        return -1
    import numpy as np
    w, h, a = ppm(p1)
    w2, h2, b = ppm(p2)
    if (w, h) != (w2, h2):
        return -1
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
    A = np.frombuffer(a, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)[y0:y1, x0:x1].astype(np.int32)
    B = np.frombuffer(b, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)[y0:y1, x0:x1].astype(np.int32)
    return float(np.abs(A - B).mean())


@sequence
def titanium_look_start(c):
    """TEXTURES TITANIUM from the start (CYANSCN1, the ENHANCED renderer): the Titanium archive's textures by name, its
    sky dome, ground and SKYGND.PAR; a different picture from the enhanced mix's (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder (MW2.PRJ + SKYGND.PAR of the Titanium 16-bit build): skipped')
        return
    shots = [(2.5, 'view')]
    r = c.run(secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots,
              env={'MW2_CONFIG': titanium_cfg('titanium_look_start', tdir), 'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'),
                   'MW2_TEXTURES': 'titanium'})
    m = re.search(r'titanium: (\d+) textures from the Titanium archive, (\d+) from', r.out)
    c(m is not None and int(m.group(1)) > 0, 'textures taken from the Titanium archive', m.group(0) if m else 'no titanium line')
    c('no Titanium archive' not in r.out, 'the Titanium archive opens')
    e = c.run('_enh', secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots)
    d = mean_diff(r.shots.get('view'), e.shots.get('view'))
    c(d > 8, 'the sky differs from the enhanced look', 'mean diff %.1f' % d)
    c.note('%s; sky mean diff vs enhanced %.1f' % (m.group(0) if m else '?', d))


@sequence
def titanium_look_switch(c):
    """the in-mission Textures to TITANIUM, the Renderer to 3DFX with them, and the Textures back to ORIGINAL
    (switch_look, MW2_TEST_VIDEO): the look changes live each time; the Titanium sky stays with the 3Dfx renderer (no
    fog: sky= as it is); the simulation goes on (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    shots = [(2.5, 'before'), (4.0, 'titanium'), (5.5, 'ti3dfx'), (7.0, 'after')]   # (the drop has settled by 2.5 s)
    cfg = titanium_cfg('titanium_look_switch', tdir)
    r = c.run(secs=7.5, throttle=0, nostart=True, mission='YELLSCN1', shots=shots,
              env={'MW2_CONFIG': cfg, 'MW2_TEST_VIDEO': '3:titanium,4.5:3dfx,6:original'})
    i1 = r.out.find('video mode: enhanced, textures titanium')
    i2 = r.out.find('video mode: 3dfx, textures titanium')
    i3 = r.out.find('video mode: 3dfx, textures original')
    c(i1 >= 0, 'Textures to titanium (renderer kept)', 'no "video mode: enhanced, textures titanium"')
    c(i2 > i1 >= 0, 'Renderer to 3dfx (textures kept)', 'no "video mode: 3dfx, textures titanium" after it')
    c(i3 > i2 >= 0, 'Textures back to original', 'no "video mode: 3dfx, textures original" after it')
    c(re.search(r'titanium: [1-9]\d* textures from the Titanium archive', r.out) is not None, 'Titanium textures in use')
    # YELL's Titanium sky: sky=64,64,200 under the clouds (the 3D editions' is a pale grey-blue): saturated blue pixels
    blue = [count_px(r.shots.get(k), lambda R, G, B: (B > R + 80) & (B > G + 80), (0, 0, 1, 0.45)) for k in ('before', 'titanium', 'ti3dfx', 'after')]
    c(blue[1] > 2000 and blue[2] > 2000, 'the Titanium sky with both renderers (sky=64,64,200 + clouds)', 'blue pixels %s' % blue)
    c(0 <= blue[0] < 200 and 0 <= blue[3] < 200, 'the original sky before and after', 'blue pixels %s' % blue)
    c(r.last.get('t', 0) >= 7.0, 'the mission runs on through the switches')
    with open(cfg) as f:
        saved = f.read()
    c('renderer=3dfx' in saved and 'textures=original' in saved and 'edition=' not in saved, 'saved as renderer= / textures=', saved)
    c.note('saturated-blue sky pixels before / titanium / 3dfx+titanium / after: %s' % blue)



@sequence
def look_menu_textures(c):
    """the GRAPHICS menu's Textures row (port): Esc, 2 (Graphics), 4 steps Textures to Titanium, 0 accepts - the look
    switches live and is saved; in a DOS mission the row is locked to Original (4 does nothing, no switch); the old
    edition=titanium is read as renderer=enhanced textures=titanium (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    cfg = titanium_cfg('look_menu_textures', tdir)
    keys = [(3, 'esc'), (3.3, '2'), (3.6, '4'), (3.9, '0'), (4.2, 'esc')]
    r = c.run(secs=6, throttle=0, nostart=True, mission='YELLSCN1', keys=keys, shots=[(3.75, 'menu')], env={'MW2_CONFIG': cfg})
    c('video mode: enhanced, textures titanium' in r.out, 'Textures stepped and accepted: the Titanium textures live',
      'no "video mode: enhanced, textures titanium"')
    with open(cfg) as f:
        saved = f.read()
    c('textures=titanium' in saved and 'renderer=enhanced' in saved, 'saved for the next mission', saved)
    d = c.run('_dos', secs=6, throttle=0, nostart=True, mission='YELLSCN1', keys=keys, dos_look=True,
              env={'MW2_CONFIG': titanium_cfg('look_menu_textures_dos', tdir), 'MW2_TEXTURES': 'titanium'})
    c('video mode:' not in d.out and 'titanium:' not in d.out, 'DOS: Textures locked to Original', 'a look switch in a DOS mission')
    # the old single look, migrated
    old = os.path.join(OUT, 'look_menu_textures', 'old.cfg')
    with open(old, 'w') as f:
        f.write('game=%s\nedition=titanium\ntitanium=%s\n' % (os.path.join(OUT, 'look_menu_textures', 'game'), tdir))
    m = c.run('_old', secs=6, throttle=0, nostart=True, mission='YELLSCN1', edition='titanium',   # (the old MW2_EDITION too)
              env={'MW2_CONFIG': old, 'MW2_TEST_VIDEO': '3:3dfx'})
    with open(old) as f:
        saved = f.read()
    c('video mode: 3dfx, textures titanium' in m.out, 'edition=titanium: the Titanium textures kept on a renderer switch', m.out[-300:])
    c('renderer=3dfx' in saved and 'textures=titanium' in saved and 'edition=' not in saved, 'migrated to renderer= / textures=', saved)


def kph_at(r, t0, t1):
    """the cockpit readouts (MW2_KPH_TRACE) between t0 and t1 seconds"""
    return [int(m.group(2)) for m in re.finditer(r'^kph (\d+\.\d+)s (-?\d+) kph', r.out, re.M) if t0 <= float(m.group(1)) <= t1]


@sequence
def top_speed_gravity_dos(c):
    """top speed = walk MP x 105.49 (integer c[0x22], DOS 0x4cd46) / the planet's gravity ratio (DOS 0x26973, no clamp; 3Dfx
    0x100190d0 c[0x22] / g), presets n x 113 / 1024 (0x4684c), readout x 1.5 (0x32085). DOSBox, steady readouts on flat
    ground: ORANSCN1 (Wolf mission 2, 0.6 g) at the start heading 180 - Timber Wolf (TBR00STD, 5 MP) FULL 133 kph, Nova
    (NVA00STD, 6 MP - the mission's default 'Mech) FULL 160 kph; TNJ1SCN1 (1.0 g) Firemoth (FRM00TRN, 10 MP) FULL 160 kph
    at heading 0; the Nova on ORAN (c[0x22] 633 -> 1055, the Firemoth's on 1 g) THROTTLE_8 (7 x 113 / 1024) 124 kph -
    the user's Firemoth reading - and THROTTLE_7 106 kph"""
    for suf, mission, mech, start, key, want in (
            ('_tbr', 'ORANSCN1', 'timbrwlf/tbr00std', None, '0', 133),
            ('_nva', 'ORANSCN1', 'nova/nva00std', None, '0', 160),
            ('_frm', 'TNJ1SCN1', None, '16441,12000', '0', 160),
            ('_frm8', 'TNJ1SCN1', None, '16441,12000', '8', 124),
            ('_nva8', 'ORANSCN1', 'nova/nva00std', None, '8', 124),
            ('_nva7', 'ORANSCN1', 'nova/nva00std', None, '7', 106)):
        env = {'MW2_KPH_TRACE': '1'}
        if start:
            env['MW2_START'] = start
        kw = {'mech': mech} if mech else {}
        r = c.run(suf, secs=10, keys=[(1, key)], dump='/1', nostart=True, mission=mission, env=env, **kw)
        got = kph_at(r, 8.5, 10)
        c(got and min(got) == max(got) == want, '%s %s key %s: %d kph' % (mission, mech or 'FRM00TRN', key, want), str(sorted(set(got))))


@sequence
def kph_readout_jets(c):
    """the readout is the controller's whole velocity (DOS 0x32085 reads +0xf4 / +0xf8 / +0xfc): the directional jets'
    drift counts. DOSBox ORANSCN1 Nova from a standstill, forward jets (GreyHome) held: 60 / 141 / 232 / 298 / 364 / 391
    kph over ~6 s (the port had read 0 - the drift was left out); jumping at FULL: 160 -> 184..193 while rising, back
    toward 160 at the top (the vertical speed counts)"""
    r = c.run(secs=12, hold=[(6, 8, 'home')], dump='/1', nostart=True, mission='ORANSCN1', mech='nova/nva00std',
              env={'MW2_KPH_TRACE': '1'})
    before, during = kph_at(r, 5.5, 6.0), kph_at(r, 6.1, 8.0)
    c(before and max(before) == 0, 'standing: 0 kph', str(before[-3:]))
    c(during and during[-1] > 200, 'forward jets: over 200 kph after 2 s', str(during[::6]))
    c(during and all(b >= a for a, b in zip(during, during[1:])), 'rising while the jets push', str(during[::6]))
    r2 = c.run('_jump', secs=13, keys=[(1, '0')], hold=[(9, 11, 'j')], dump='/1', nostart=True, mission='ORANSCN1',
               mech='nova/nva00std', env={'MW2_KPH_TRACE': '1'})
    k0, k1 = kph_at(r2, 8.0, 8.9), kph_at(r2, 10.0, 11.0)
    c(k0 and min(k0) == max(k0) == 160, 'FULL before the jump: 160 kph', str(sorted(set(k0))))
    c(k1 and max(k1) > 175, 'jumping at FULL: the reading rises (DOS 184..193)', str(k1[::6]))


@sequence
def armour_difficulty_factor(c):
    """The loader's armour factor (engine 0x10041410, DOS 0x4c760), by Combat Variables difficulty (MW2DIF +5) 0 / 1 / 2:
    front and rear armour - not structure - x 4 for the player whatever the difficulty, x 1 / 3 / 4 for another
    alliance's unit (x 3 / 3 / 4 for the player's side). Damage per hit stays the weapon table's +0x30 (medium laser 5).
    The port had loaded the MEK's points as they are, so everything died 3-4 times too fast (user report 6). Timber Wolf
    head 9 armour + 3 structure; YELL's Jenner (actor 2) right leg 9 + 8"""
    def loc(st, key, i):
        return float(st[key].split(',')[i])
    for d, k in ((0, 1), (1, 3), (2, 4)):
        r = c.run('_d%d' % d, secs=6, nostart=True, files={'MW2DIF.CFG': bytes([0, 0, 1, 1, 1, d, 0, 0])},
                  env={'MW2_TEST_WATCH': '1:2', 'MW2_TEST_PHIT': '3:1:23:3', 'MW2_TEST_AHIT': '3:2:7:23:1'})
        a, b = r.at(2.0), r.at(5.0)
        if 'parmor' not in a or 'a_armor' not in a or 'parmor' not in b:
            c(False, 'difficulty %d: state dumps' % d)
            continue
        c(abs(loc(a, 'parmor', 0) - (9 * 4 + 3)) < 0.01, 'difficulty %d: player head 9 x 4 + 3' % d, a['parmor'])
        c(abs(loc(a, 'a_armor', 6) - (9 * k + 8)) < 0.01, 'difficulty %d: enemy right leg 9 x %d + 8' % (d, k), a['a_armor'])
        c(abs(loc(a, 'parmor', 0) - loc(b, 'parmor', 0) - 15) < 0.01, 'difficulty %d: three medium laser hits = 15' % d,
          '%s -> %s' % (a['parmor'], b['parmor']))
        c(abs(loc(a, 'a_armor', 6) - loc(b, 'a_armor', 6) - 5) < 0.01, 'difficulty %d: one medium laser hit = 5' % d,
          '%s -> %s' % (a['a_armor'], b['a_armor']))


@sequence
def walking_contact_damage(c):
    """Ordinary walking does not wear the mech down: CYANSCN1 at FULL for 60 s (slopes, rocks) - collision damage
    (0x10019d84 -> 0x1000c3c0, (v - 3.069) x 0.0501 x 3 every contact frame) lands about once per impact, the impact
    cutting the speed to a quarter (0x1000b5e0); measured 2 contacts, 1.6 points (MW2_DAMAGE_LOG). Guards the
    session-13 removal of the once-per-contact latch (user report 6)"""
    r = c.run(secs=60, throttle=1, twist=0, mission='CYANSCN1', env={'MW2_DAMAGE_LOG': '1'}, dump='/1')
    hits = [float(l.split()[-1]) for l in r.out.splitlines() if l.startswith('damage terrain player') or l.startswith('damage unit player')]
    c(r.last.get('t', 0) >= 59, 'the walk runs 60 s')
    c(len(hits) <= 6 and sum(hits) < 5.0, 'a few contacts, under 5 points in all', '%d contacts, %.2f' % (len(hits), sum(hits)))
    c(all(h < 1.5 for h in hits), 'each contact under 1.5 points at walking speed', str([round(h, 2) for h in hits]))



@sequence
def objectives_dustoff_shown(c):
    """user report: the first campaign mission's "proceed to the dustoff" radio plays but the F12 list never shows the
    dustoff. YELLSCN1 (Wolf mission 1): node 14 (toggle, all of nodes 6 + 7 done) flips the shown flag of node 10
    "Dustoff site: Nav Eta", priority 8 (Return). The engine's list (0x10017b80 -> 0x100176d0) shows the shown nodes by
    priority 1, 2, 4, 0, 8 - the port had listed 1, 2, 0 only (no Tertiary 4, no Return 8). DOSBox YELLSCN1 F12 (node
    10 made visible in a PRJ copy): Primary x2, Secondary, Tertiary, Return"""
    r = c.run(secs=4, nostart=True, throttle=0, mission='YELLSCN1', dump='0.5,3.5', env={'MW2_TEST_NODE_OK': '1:6,1:7'})

    def rows(s):
        return [tuple(int(v) for v in x.split(':')) for x in str(s.get('objs', '')).split(';') if x and x != '-']
    a, b = rows(r.at(0.5)), rows(r.at(3.5))
    c([(n, p) for n, p, _ in a] == [(6, 1), (7, 1), (13, 2), (20, 4)], 'start: Primary x2, Secondary, Tertiary (priority 4)', str(a))
    c(10 not in [n for n, _, _ in a], 'the dustoff hidden at the start', str(a))
    c(len(b) == 5 and b[-1][:2] == (10, 8), 'nodes 6 + 7 done: the dustoff (node 10, Return) listed last', str(b))
    c([st for n, _, st in b if n in (6, 7)] == [5, 5], 'nodes 6 and 7 listed as succeeded', str(b))


@sequence
def autopilot_turn_announced(c):
    """user report: a manual turn switches the autopilot off silently. The engine (0x1001a480..0x1001a4fc) turns a
    leg-turn input with the autopilot on into the TOGGLE_AUTOPILOT request: off, and message 0x1d suffix 2 - "Autopilot"
    (voice 65) "disengaged" (84), as the key; the HUD's "AUTOPILOT" (0x100183d0, while +0xbc is 1 / 2) goes. DOSBox
    YELLSCN1: a, then Left held -> "Autopilot disengaged." and the AUTOPILOT text gone"""
    r = c.run(secs=15, keys=[(UP, '0'), (UP + 0.5, 'a')], hold=[(13, 13.5, 'left')], shots=[(12.5, 'on'), (14.5, 'off')],
              size='640x400')
    c(r.at(12.5)['autopilot'] == 1 and r.at(13.7)['autopilot'] == 0, 'engaged, then off after the turn')
    m = re.search(r'MSGV now=(\d+) voice=65 Autopilot disengaged\.', r.out)
    c(m is not None and 12900 <= int(m.group(1)) <= 13700, '"Autopilot disengaged." (voice 65) at the turn', m.group(0) if m else 'none')
    box = (0.15, 0.92, 0.36, 0.99)   # "AUTOPILOT" at (110, 456) of the 640 x 480 layout
    g = lambda R, G, B: (G > 140) & (R < 120) & (B < 120)
    on, off = count_px(r.shots.get('on'), g, box), count_px(r.shots.get('off'), g, box)
    c(on > 30 and off < on / 4, 'the HUD "AUTOPILOT" shown while engaged, gone after', 'green px %d -> %d' % (on, off))


@sequence
def restart_powerup_sound(c):
    """user report: a manual restart (s, s) played only the online beep. The restart takes the player through state
    0 -> 1 again (0x1001adf0 "Powering up..." -> 0x1001ab71), which plays MECTURX2 0xf7 at the mech as at the mission
    start; MECBSYRX 0xce when online. DOSBox YELLSCN1 (SB16 capture): BET11_1 at the shutdown, MECTURX2 0.1 s after the
    restart key, MECBSYRX 6.4 s later"""
    r = c.run(secs=22, keys=[(10, 's'), (13, 's')], env={'MW2_SFX_LOG': '1'})
    t_up = r.msg_time('Powering up')
    if not c(t_up is not None, 'restarted ("Powering up...")'):
        return
    snd = [(float(t) * 1000.0, int(i)) for t, i in re.findall(r'SFXLOG (-?[\d.]+) snd (\d+)', r.out)]
    c(any(i == 247 and abs(t - t_up) < 150 for t, i in snd), 'MECTURX2 (0xf7) at the restart', str([x for x in snd if x[0] > 9000]))
    c(any(i == 206 and t_up + 4000 < t < t_up + 9000 for t, i in snd), 'MECBSYRX (0xce) once back online')
    c(not any(i == 247 and 9500 < t < t_up - 150 for t, i in snd), 'no MECTURX2 at the shutdown')


@sequence
def shell_music_after_mission(c):
    """user report: the training menu's music did not resume after a training mission. The original's shell restarts
    after the simulation ("mw2shell sim", 0x36700) on the saved screen with nothing playing and applies that screen's
    music: training 14 (Falcon 0x29), Instant Action 7 (0x23) start again (the debriefing, 0 = no change, stays silent).
    TEST ONLY hooks: MW2_SHELL_TRAIN / MW2_SHELL_CLICKS launch, MW2_SIM a no-op, MW2_SHELL_MUSICLOG"""
    shell = os.path.join(REPO, 'mw2shell')
    if not os.path.exists(shell):
        c.note('mw2shell not built: skipped')
        return
    os.makedirs(os.path.join(OUT, c.name), exist_ok=True)
    gd = install_dir(c.name, None)
    if not os.path.exists(os.path.join(gd, 'mt32')) and not os.path.exists(os.path.join(gd, 'ultrasnd')):
        c.note('no mt32/ or ultrasnd/ in the test game folder: skipped')
        return
    with open(os.path.join(OUT, c.name, 'mw2port.cfg'), 'w') as f:
        f.write('game=%s\nedition=enhanced\n' % gd)
    for scr, extra, label in (('14', {'MW2_SHELL_CLAN': '1', 'MW2_SHELL_TRAIN': '1'}, 'training'),
                              ('7', {'MW2_SHELL_CLICKS': '0'}, 'Instant Action')):
        e = {k: v for k, v in os.environ.items() if not k.startswith('MW2_')}
        e.update({'SDL_VIDEODRIVER': 'offscreen', 'SDL_AUDIODRIVER': 'dummy', 'MW2_CONFIG': os.path.join(OUT, c.name, 'mw2port.cfg'),
                  'MW2_SAVE_DIR': gd, 'MW2_SHELL_SCREEN': scr, 'MW2_SHELL_FRAMES': '3', 'MW2_SHELL_MUSICLOG': '1',
                  'MW2_SHELL_SHOT': os.path.join(OUT, c.name, 'shell%s.ppm' % scr), 'MW2_SIM': 'echo %s'})
        e.update(extra)
        try:
            p = subprocess.run([shell], env=e, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 * TSCALE)
            out = p.stdout.decode('latin-1')
        except subprocess.TimeoutExpired:
            c(False, '%s: shell run timed out' % label)
            continue
        with open(os.path.join(OUT, c.name, 'log%s.txt' % scr), 'w') as f:
            f.write(out)
        ev = re.findall(r'music: (start entry \d+|stop \(mission\))', out)
        c(len(ev) >= 3 and ev[0].startswith('start') and ev[1] == 'stop (mission)' and ev[2] == ev[0],
          '%s: the screen\'s song stops for the mission and starts again on return' % label, str(ev))


# ---------------------------------------------------------------- walk animation: stopping comes to rest

@sequence
def idle_mech_rests(c):
    """user report (training): Keating's mech "doesn't seem to rest properly.. he gets caught in a mid stride when just
    sitting there". The key player (engine 0x1000dec0) with the gait code's request (0x1000b3a0: c[0x13] at most 1/32 of
    full over the stop level -> sequence -1, gait 0): the stride runs on to the sequence's end key (flag 0x400 - walk key
    11, reached from the run through key 15 flag 0x100 -> walk 6) and the keys sum back to the standing pose; the port had
    stopped advancing the keys when the ground speed fell under 1 cm/s. TNJ1: the instructor (actor 1) stands until
    ~25 s, walks / runs to ~50 s and stops: at rest by ~55 s and held. The player: full throttle 3.5 s, stop -> the
    standing pose within 3 s. (Dead mechs still keep their pose: standing_wreck_frozen)"""
    REST = (0.0, 12.0, 32.0)   # every mech's ANIM: walk 0-11, run 12-19, reverse 20-31 -> rest at key 0 / 12 / 32
    r = c.run('_tnj1', secs=62, throttle=0, mission='TNJ1SCN1', dump='/1', env={'MW2_WATCH_ACTOR': '1'})
    s0, w = r.at(20.0), r.span(30.0, 45.0)
    c(s0.get('w_key') == 0.0 and s0.get('w_on') == 0, 'TNJ1: the instructor stands at first', 'key %s on %s' % (s0.get('w_key'), s0.get('w_on')))
    c(len(set(x.get('w_key') for x in w)) > 5 and max(x.get('w_thr', 0) for x in w) > 0.5, 'TNJ1: he walks (keys move)',
      'keys %s' % [x.get('w_key') for x in w][:6])
    stop = next((x for x in r.states if x['t'] > 30 and x.get('w_thr', 1) <= 1 / 32.0), None)
    c(stop is not None, 'TNJ1: he stops (throttle back to the stop level)')
    if not stop:
        return
    end = r.span(stop['t'] + 4.0, 62.0)
    c(end and all(x.get('w_key') in REST and x.get('w_on') == 0 for x in end), 'TNJ1: within 4 s of stopping he stands in the rest pose, nothing playing',
      'stop at %.1f s; keys after %s' % (stop['t'], sorted(set(x.get('w_key') for x in end))))
    c(end and len(set((x.get('w_x'), x.get('w_z')) for x in end)) == 1, 'TNJ1: and stays put')
    # YELLSCN1 drops the player from 25 m (on the ground at ~2.3 s) and the keys hold while airborne
    # (walk_keys_hold_airborne): the throttle after touch-down
    r2 = c.run('_player', secs=11.5, keys=[(3.0, '0'), (6.5, '1')], nostart=True, dump='/0.25')
    mid = r2.at(6.5)
    c(mid.get('pkey') not in REST, 'player: mid-stride when the stop is pressed', 'key %s' % mid.get('pkey'))
    tail = r2.span(9.5, 11.5)
    c(tail and all(x.get('pkey') == 12.0 for x in tail), "player: the walk ends at its last key (11) -> the standing pose within 3 s",
      'keys %s' % sorted(set(x.get('pkey') for x in tail)))
    moved = [x['pkey'] for x in r2.span(6.5, 9.5)]
    c(len(set(moved)) > 4, 'player: the stride finishes (keys move after the stop)', '%s' % moved[:8])


# ---------------------------------------------------------------- AI aim: the eye ray and the aim range servos

@sequence
def ai_aim_eye_ray(c):
    """AI aim (engine 0x10043cc0 -> 0x10044810 / 0x100448e0, DOS 0x52766 -> 0x53190 / 0x53250): each AI projectile as it
    leaves casts the aim ray from the EYE (unit +0x44, the EYEO node as posed - lagged twist; no torso sway: the controller
    sets the torso's rotation every frame) along the eye's world yaw and the controller's lagged pitch; a unit struck sets the range +0xb0 (2000 at the least), else it keeps its
    value, drifting to 50000 (servo +0xa8, tau 20 s); +0xa0 follows it (tau 0.2 s) and the shot heads from its mount for the
    point at +0xa0 on the ray. The player in the cockpit view is struck on the cockpit set (LOD 4, 0x100267d0): TW5_HEAD,
    arms, legs. AQUASCN1, Timber Wolf standing: hits land where the eye ray meets the head (9-12 m up), not on the
    centre (the port had aimed every shot at the target's centre); DOSBox HTAL: CT / RT front only, the arms and legs
    only the area damage (0x10046700) of one explosion"""
    r = c.run(secs=45, throttle=0, twist=0, mission='AQUASCN1', dump='/1', env={'MW2_AI_AIM_TRACE': '1'})
    aim, hits, last = [], [], {}
    for l in r.out.splitlines():
        f = l.split()
        if l.startswith('aiaim ') and len(f) >= 24:
            t, a = float(f[1]), int(f[3])
            aim.append(dict(t=t, a=a, hit=int(f[7]), dist=float(f[9]), rng=int(f[11]), conv=float(f[13]),
                            yaw=float(f[19]), body=float(f[21]), gap=t - last.get(a, -9)))
            last[a] = t
        elif l.startswith('plhit ') and len(f) >= 13:
            hits.append(dict(t=float(f[1]), group=int(f[7]), up=float(f[11])))
    c(len(aim) >= 20, 'the AI fires (aim ray cast per projectile)', '%d projectiles' % len(aim))
    conv_ok = [x for x in aim if x['hit'] and x['gap'] >= 0.25]
    c(conv_ok and all(abs(x['conv'] - max(2000.0, int(x['dist']))) <= 1.0 for x in conv_ok),
      'a struck unit sets the range and the convergence follows (0.2 s servo)',
      '%s' % [(x['dist'], x['conv']) for x in conv_ok if abs(x['conv'] - max(2000.0, int(x['dist']))) > 1.0][:4])
    c(all(x['conv'] < 149000 for x in aim), 'nothing struck: the range keeps its value (never the ray\'s 150000)')
    # within 0.3 deg: the hip nodes below the torso carry their gait-change offsets (walk_run_carried_offset) - 0.11 here
    c(all(abs(x['yaw'] - x['body']) < 0.3 for x in aim),
      'the eye ray along the legs + lagged twist: the walk\'s rotation keys on the torso never add up (0x1001a180 sets its '
      'rotation every frame; Gargoyle +-4 deg yaw)', '%s' % [(x['t'], x['yaw'], x['body']) for x in aim if abs(x['yaw'] - x['body']) >= 0.3][:4])
    c(len(hits) >= 8, 'the AI hits the player', '%d hits' % len(hits))
    c(hits and all(x['group'] == 3 and 900 <= x['up'] <= 1250 for x in hits),
      'every hit on the cockpit set\'s head at the eye ray\'s height (not the centre ~5.5 m)',
      '%s' % [(x['group'], x['up']) for x in hits if not (x['group'] == 3 and 900 <= x['up'] <= 1250)][:5])


@sequence
def ct_hit_spread_by_edition(c):
    """a CT hit is spread over the side torsos: the 3D editions rand(3) -> LT / CT / RT (0x10017264), DOS rand(2) -> 2 / 3
    only (MW2.EXE 0x263c8). DOSBox AQUASCN1 H T A L: one side torso and the CT take every hit, the other side none.
    40 medium laser hits on location 3 (front armour x 4: LT / RT 116, CT 167 with structure)"""
    def loc(st, i):
        return float(st['parmor'].split(',')[i])
    for ed, three in (('enhanced', True), ('dos', False)):
        r = c.run('_' + ed, secs=6, nostart=True, env={'MW2_EDITION': ed, 'MW2_TEST_PHIT': '3:3:23:40'})
        a, b = r.at(2.0), r.at(5.0)
        lost = [loc(a, i) - loc(b, i) for i in (1, 2, 3)]
        c(lost[0] > 0 and lost[1] > 0, '%s: locations 2 and 3 take damage' % ed, '%s' % lost)
        c((lost[2] > 0) == three, '%s: location 4 %s' % (ed, 'too' if three else 'untouched'), '%s' % lost)


@sequence
def splash_area_damage(c):
    """area damage (0x10046490 -> 0x10046700): a live effect of a damaging type (0x1025b180: missile / PPC hits, death
    explosions ...) gives every unit within its contact radius + the effect's radius ticks / 256 on locations 2-8 and
    half that on the head, each frame - the DOSBox AQUASCN1 H T A L shows every bar, legs and head too, taking a sliver
    after a PPC hit. Combat Variables SPLASH DAMAGE (MW2DIF +2) off: none"""
    for sp in (1, 0):
        r = c.run('_%d' % sp, secs=60, throttle=0, twist=0, mission='AQUASCN1', dump='/2', env={'MW2_DAMAGE_LOG': '1'},
                  files={'MW2DIF.CFG': bytes([0, 0, sp, 1, 1, 1, 0, 0])})
        area = [l.split() for l in r.out.splitlines() if l.startswith('damage area player')]
        if sp:
            locs = set(int(f[5]) for f in area)
            c(locs == set(range(1, 9)), 'splash on: every location takes area damage', '%s' % sorted(locs))
            h = [float(f[-1]) for f in area if f[5] == '1']
            o = [float(f[-1]) for f in area if f[5] == '7']
            c(h and o and abs(sum(h) * 2 - sum(o)) < 0.05 * sum(o) + 0.01, 'the head half the others', '%.3f / %.3f' % (sum(h), sum(o)))
        else:
            c(not area, 'splash off: none', '%d lines' % len(area))



# ---------------------------------------------------------------- mech-mech contact while walking: sliding round

@sequence
def tnj1_slide_round_instructor(c):
    """agent report: TNJ1's Firemoth driven straight ahead from the start at FULL is "blocked about 4 s near z 8420, then
    slides past". It is the instructor (TNJ1ENS2, a Mad Dog standing at TNW1ST21 17000,9500 until ~25 s), 198 cm to the
    side of the player's line: contact spheres 550 + 500 (MGEO int[6]). The engine (0x1000b5e0 -> 0x1000ba20) projects the
    end of every move that comes inside r1 + r2 onto that sphere and sets the velocity to n x max(1, |(dvx, dvy, the
    other's vz)| / 2) (the mover's own z velocity is overwritten by the other's, 0x1000bc93) - walking as well as
    airborne; the drive brings the velocity back at 1/45 a tick (its part across the heading easing off the same way) and
    each frame moves by the mean velocity (v + a dt / 2) dt (0x10019310), the next move projected again: the mech creeps
    round the sphere, faster at lower frame rates. Heading 0 the bounce is the 1 cm/tick floor and the contact speed
    |(dvx, dvy)| ~ 0: no damage. DOSBox (MCGA, cycles 40000, ~32 fps): ~8 s in contact, the readout 9-28 km/h, then on at
    full speed (~1.5 s at the SVGA default's ~5 fps); the port at 30 fps 8 s (it had pushed a walker back 60 cm at -30 %
    speed: 4.5 s, a back-and-forth stagger)"""
    tnj1_slide(c, 30)


def tnj1_slide(c, fps):
    """TNJ1's slide round the standing instructor at `fps` frames a second (MW2_TEST_FPS; 30 = the default)"""
    r = c.run(suffix='' if fps == 30 else '_%d' % fps, secs=22, throttle=1, mission='TNJ1SCN1', dump='/0.25',
              env={'MW2_TEST_FPS': fps} if fps != 30 else None)
    if fps != 30:
        c0 = c
        c = lambda cond, what, detail='': c0(cond, '%d fps: %s' % (fps, what), detail)
    pre = r.before(8.0)
    c(pre.get('kmh', 0) > 100 and pre.get('x') == 16802, 'full speed straight up the start line before the instructor',
      'kmh %s x %s' % (pre.get('kmh'), pre.get('x')))
    slow = [s for s in r.span(8.0, 22.0) if s['kmh'] < 30]
    c(slow and slow[0]['t'] < 8.5, 'stopped by the instructor at ~8 s', str([(s['t'], round(s['kmh'])) for s in r.span(7.5, 9.0)]))
    if not slow:
        return
    t0, t1 = slow[0]['t'], slow[-1]['t']
    c(6.0 <= t1 - t0 <= 10.0, 'in contact 6-10 s (DOSBox ~8 s at ~32 fps)', '%.2f..%.2f' % (t0, t1))
    con = r.span(t0, t1)
    d = [math.hypot(s['x'] - 17000, s['z'] - 9500) for s in con]
    c(min(d) >= 1040 and max(d) < 1100, 'on the contact sphere throughout (r1 + r2 = 1050; across the ground ~1046, the centres 661 / 550 high)', '%.0f..%.0f' % (min(d), max(d)))
    xs = [s['x'] for s in con]
    c(all(b <= a for a, b in zip(xs, xs[1:])), 'creeps round it to the left (x never back)', str(xs[:12]))
    c(all(0 <= s['y'] < 10 for s in con), 'feet on the ground but for the projection\'s lift (0x1000ba20 in 3D: n.y x the penetration, a few cm)',
      str(sorted(set(s['y'] for s in con))))
    end = r.at(21.5)
    c(end.get('kmh', 0) > 100 and end.get('z', 0) > 9500 + 5000 and abs(end.get('heading', 1)) < 0.1, 'then on past it at full speed, heading 0',
      'kmh %s z %s heading %s' % (end.get('kmh'), end.get('z'), end.get('heading')))
    c(r.at(1.0).get('parmor') == end.get('parmor'), 'no collision damage (contact speed |(dvx, dvy)| ~ 0)', '%s -> %s' % (r.at(1.0).get('parmor'), end.get('parmor')))
    return t1 - t0



@sequence
def tnj1_slide_frame_rates(c):
    """the walking contact slide is per frame in the engine (0x10019310 / 0x1000ba20: each frame's move projected onto
    the contact sphere, the velocity reset to n x max(1, dv / 2) and restored at 1/45 a tick - DOSBox TNJ1 ~8 s at ~32
    fps, 4.6 s at ~60, ~1.5 s at ~5); the port's standing rule normalises per-frame effects to the 30 fps reference, so
    in contact the player's move runs in 1/30 s steps (the remainder carried): TNJ1's slide round the instructor takes the
    30 fps time at 60, 120 and 144 fps (MW2_TEST_FPS)"""
    ts = [tnj1_slide(c, fps) for fps in (60, 120, 144)]
    c(all(t is not None and 6.0 <= t <= 10.0 for t in ts), 'the same ~8 s at 60 / 120 / 144 fps', str(ts))


@sequence
def tnj4_wall_slide(c):
    """walking into a wall at an angle (TNJ4's S_1BQDAS, a type 0 box, its south face z 166195; the Firemoth at FULL,
    heading -18.5, 18.5 degrees off head-on - DOSBox's run). The engine's world hit (0x1000b5e0): the move's segment
    (centre to centre + R) ends at the face; the touching point is that hit less R along the move - BEHIND the start
    when the face is nearer than R; the position + r x untravelled x 0.25 (0x10247148), r = d - 2 (d . n) n, the
    velocity the whole vector r x |v| x -0.25 (0x10247144), restored at 1/45 a tick (0x10019310), the next moves hit
    again: the mech creeps west along the face. DOSBox (TNJ4, heading -18.5, ~28 fps by its video's distinct frames):
    the Beta readout 222 -> 213 m in ~38 s = ~60 cm/s along the face; the port ~70 cm/s at 30 fps (the touching point
    found by halving the move - never behind the start - had given ~28 with the whole vector, so the port had kept the
    velocity along the heading: ~40 here). The engine's slide is per frame; the port's 1/30 s contact steps give the
    same speed at 60 and 144 fps (MW2_TEST_FPS)"""
    speeds = []
    for fps in (30, 60, 144):
        env = {'MW2_START': '-143500,163000', 'MW2_START_HEADING': -18.5}
        if fps != 30:
            env['MW2_TEST_FPS'] = fps
        r = c.run(suffix='' if fps == 30 else '_%d' % fps, secs=26, throttle=1, mission='TNJ4SCN1', dump='/0.5', env=env)
        hit = next((s['t'] for s in r.span(7.0, 26.0) if s['kmh'] < 30), None)
        c(hit is not None and hit < 12, '%d fps: stopped at the wall' % fps, 'kmh %s' % [round(s['kmh']) for s in r.span(7.0, 12.0)])
        if hit is None:
            continue
        seg = r.span(hit + 1.0, hit + 15.0)
        zs = [s['z'] for s in seg]
        c(max(zs) - min(zs) < 60 and max(zs) < 165800, '%d fps: along the face (z steady, outside the box)' % fps, '%d..%d' % (min(zs), max(zs)))
        a, b = r.at(hit + 1.0), r.at(hit + 15.0)
        v = (a['x'] - b['x']) / 14.0
        speeds.append(v)
        c(all(q['x'] <= p['x'] for p, q in zip(seg, seg[1:])), '%d fps: never back east' % fps, str([q['x'] for q in seg[:8]]))
        c(50 <= v <= 90, '%d fps: slides west along it at ~70 cm/s (DOSBox ~60)' % fps, '%.1f cm/s' % v)
    c(len(speeds) == 3 and max(speeds) - min(speeds) <= 0.15 * max(speeds), 'the same speed at 30 / 60 / 144 fps', str(speeds))

@sequence
def aqua_time_to_death(c):
    """AQUASCN1, the player's Timber Wolf standing (DOS edition: a CT hit goes to RT / CT, rand(2)): the two attackers
    (Gargoyle, Stormcrow) open fire from ~650 m and kill it in DOS-like time. The engine's controller (0x1001a180 ->
    0x1000f990 -> 0x1002f070 / 0x1000f8f0) sets the torso node's (the first THNG node) whole local rotation every frame -
    pitch, twist, roll 0 - so the walk's relative rotation keys on it (0x1000dec0 -> 0x1000f9d0; the Gargoyle's +-4 deg
    yaw) never add up and the AI's eye ray (0x10044950: the eye node's world yaw) points along legs + lagged twist.
    The AI fires through its trigger (controller +0x25): the weapon step (0x100437a0, run before the AI by 0x1001a180)
    fires the selected weapon on the press only and on the release moves the selection on (0x10043f80 with 0; DOS
    0x52080 -> 0x528b0), so an AI fires its weapons in turn - the Stormcrow's UAC/20 one shot in every ~8, not every
    decision (the port had kept the weapon that fired selected: the Stormcrow emptied its 10 UAC rounds within ~10 s of
    reaching 400 m and the player died at ~30 s in 9 of 12 seeds). DOSBox (19 runs): death 25-59.4 s (median 45.5), one
    alive at 90 s; the Stormcrow's UAC sound (MECACAN1) 0-4 times before the player's death (4 video captures).
    The DOSBox runs fly the install's USERSTAR.BWD mech - a MAD DOG MDG00STD (weapon list LPLAS / MPLAS / LRM20, the
    H T A L lengths), not the Timber Wolf the port had been compared with - and DOSBox's game clock runs slow when the
    host is busy (2026-10-20: AQUA, AIs held still, the player at 114 kph readout = 21.2 m/s covered 17.8 m per wall
    second, k 0.84; 0.93-0.95 on a quieter host), so wall times overstate the game time. 8 runs timed by the
    Stormcrow's approach (25.7 m/s game, c22 / g like the player's): death 28.6-87.1 game s, median 41; side torso gone
    ~26 s, CT front armour gone ~33 s (port ~25 / ~30); armour lost by 15 / 21 / 27 s 8 / 70 / 178 (port 16 / 75 / 162).
    Port (Mad Dog, seeds 1-24): 25-52 s, median 32 - the last CT structure lasts ~1 s against DOS ~6 s (open).
    2026-10-21 (DOSBox ZMBV captures, emulated = game time; H T A L read per frame): 12 more runs 29.4-77.5 s, median 45;
    up to ~20 s the H T A L matches the port; with the player's armour x 10 (patched copy, 0x4c7bf) the total armour
    lost matches too (30 / 40 / 50 s: DOS median 238 / 305 / 367, port 262 / 316 / 365), but the port's CT + RT take
    more in 20-25 s (72 against 43 points) and, with the Gargoyle's PPC / pulse lasers out of range in both, the
    Stormcrow's lasers do about twice DOS's damage from 25 s on (by 40 s 147 against 68; UAC/20 shots alike).
    The cause (2026-10-21, DOSBox with the Gargoyle disarmed - its MEK's ER lasers retyped to ER PPCs and the ER PPC /
    pulse laser ranges 0 in a copy; the Stormcrow's ER ML given the cannon sound to count its shots): DOS fires the ER
    ML from ~505 m (21 s, as the port) but those shots miss - the first laser hits come at ~24-29 s (350-400 m); the
    weapon step (0x100437a0) spawns a projectile (0x10043cc0) with life = table +0x4c less slot +0x10, the volley
    timer before the frame's ticks (~54 of an interval-60 laser's 60 at 30 fps): the ER ML reaches ~358 m, not 509 m
    (DOS: ~380 m at its 8-22 fps). Port, the timer kept (seeds 1-24): 24.3-76.5 s, median 43 (was 32); 4 of 24 by 30 s
    (DOSBox 2 of 20; 12 runs 29.4-77.5 s, median 45)"""
    ttd, uac, reach = [], [], []
    for seed in ('1', '2', '3', '4', '5', '6'):
        r = c.run(suffix='_' + seed, secs=90, throttle=0, twist=0, mission='AQUASCN1', edition='dos', dump='/1',
                  mech='maddog/mdg00std', env={'MW2_AI_AIM_TRACE': '1', 'MW2_AI_SEED': seed})
        dead = [s['t'] for s in r.states if s.get('dead')]
        ttd.append(dead[0] if dead else 999.0)
        end = dead[0] if dead else 90.0
        aim = [l.split() for l in r.out.splitlines() if l.startswith('aiaim ')]
        # the Stormcrow (actor 1): UAC/20 (weapon 19) and ER medium lasers (23) leaving before the player's death
        e45 = min(end, 45.0)   # DOSBox counted 0-4 UAC sounds before deaths at ~25-60 s
        uac.append((sum(1 for f in aim if f[3] == '1' and f[5] == '19' and float(f[1]) < e45),
                    sum(1 for f in aim if f[3] == '1' and f[5] == '23' and float(f[1]) < e45)))
        # each Stormcrow ER ML hit: the distance (eye to the player) of its last ER ML shot before it
        ml = [(float(f[1]), math.hypot(float(f[15]) - 127500.0, float(f[17]) - 57000.0) / 100.0) for f in aim
              if f[3] == '1' and f[5] == '23']
        for l in r.out.splitlines():
            if l.startswith('plhit '):
                f = l.split()
                if f[3] == '1' and f[5] == '23':
                    prev = [d for t, d in ml if t <= float(f[1])]
                    if prev:
                        reach.append(prev[-1])
        reach.append(-max([d for t, d in ml if t < 23.0] or [0.0]))   # negative: the farthest shot before 23 s
        if seed != '3':
            continue
        hits = [float(l.split()[1]) for l in r.out.splitlines() if l.startswith('plhit ')]
        c(hits and 11.0 <= hits[0] <= 17.0, 'first hit at ~13 s from ~650 m (DOSBox ~13.5 s)', '%s' % hits[:3])
        pre = r.before((dead[0] if dead else 90.0) - 1.0)
        arm = [float(x) for x in str(pre.get('parmor', '')).split(',') if x]
        full = [float(x) for x in str(r.at(1.0).get('parmor', '')).split(',') if x]
        c(len(arm) == 8 and len(full) == 8 and all(a < f for a, f in zip(arm, full)),
          'every location has lost a little (area damage of the PPC impacts, 0x10046700)', '%s -> %s' % (full, arm))
        by_hit = 'PLAYER DESTROYED' in r.out   # else a critical (ammunition, engine) ended it with armour left
        # DOSBox H T A L: one side torso and the CT take every hit, the other side torso none - every shot enters the
        # torso piece's sphere first (group 3), DOS spreads it to RT / CT (rand(2)); the rest takes only the area damage
        c(not by_hit or (len(arm) == 8 and arm[1] < full[1] * 0.5 and arm[2] < full[2] * 0.5 and
                         all(arm[k] > full[k] * 0.75 for k in (3, 6, 7)) and all(arm[k] > full[k] * 0.4 for k in (4, 5))),
          'RT and CT shot down, LT / legs only grazed by area damage, the arms a few hits close in (DOS: CT hits to RT / CT;'
          ' DOSBox: arm hits within ~200 m)', '%s' % arm)
        c(by_hit or (len(arm) == 8 and arm[2] < full[2]), 'a critical ended it: the CT armour struck', '%s' % arm)
        c(aim and all(abs(float(f[19]) - float(f[21])) < 0.3 for f in aim), 'every AI aim ray along legs + lagged twist (no torso sway; the hips\' carried offsets)')
    hits_d, far = [d for d in reach if d >= 0], [-d for d in reach if d < 0]
    c(hits_d and max(hits_d) <= 380.0, 'ER ML hits only from within ~358 m (life 182 less the ~54-tick volley timer, 0x10043cc0)',
      'farthest %.0f m' % max(hits_d or [0]))
    c(far and max(far) >= 450.0, 'the Stormcrow fires the ER ML from beyond 450 m (DOSBox: from ~505 m, 21 s) - those miss',
      '%s' % ['%.0f' % d for d in far])
    c(all(22.0 <= t <= 90.0 for t in ttd), 'each seed dead within DOSBox\'s range (game time 28.6-87.1 s, 20 timed runs)', '%s' % ttd)
    st = sorted(ttd)
    c(36.0 <= (st[2] + st[3]) / 2.0 <= 52.0, 'the median of seeds 1-6 near DOSBox\'s (median 45 of 12 runs, 41 of 8; port seeds 1-24 43)', '%s' % ttd)
    c(sum(1 for t in ttd if t <= 30.0) <= 2, 'few early deaths (DOSBox 2 of 20 by 30 s)', '%s' % ttd)
    c(all(u <= 6 and u < m for u, m in uac),
      'the Stormcrow fires its weapons in turn: few UAC/20 rounds by 45 s (DOSBox 0-4 sounds), lasers outnumber them',
      '(uac, ermlas) %s' % uac)


@sequence
def death_explosion_area_damage(c):
    """a mech's death explosion (0x1001be40, the first: effect 0x0d at the unit, 0x10045c30) is a damaging effect type
    (0x1025b180): every unit within its contact radius + the effect's radius (0x0d: 19 m) takes ticks / 256 on every
    location for its 579 ticks (0x10046490 -> 0x10046700) - the port had shown it without the damage. AQUASCN1, the
    player 12 m / 25 m from the Gargoyle's start, every enemy destroyed at 3 s"""
    for dx, near in ((1200, True), (2500, False)):
        r = c.run('_%d' % dx, secs=8, throttle=0, twist=0, mission='AQUASCN1', dump='/1', nostart=True, edition='dos',
                  env={'MW2_DAMAGE_LOG': '1', 'MW2_START': '%d,-20000' % (110000 + dx), 'MW2_TEST_KILL_ALL': '3:0'})
        area = [l.split() for l in r.out.splitlines() if l.startswith('damage area player')]
        if near:
            c(set(int(f[5]) for f in area) == set(range(1, 9)), '12 m: area damage on every location', '%d lines' % len(area))
            h = sum(float(f[-1]) for f in area if f[5] == '1')
            c(0.5 <= h <= 2.0, '12 m: the head ~579 / 1024 (DOS edition, MW2.EXE 0x517c0: a quarter of 579 ticks / 256 - the 3D editions half; more from the wreck\'s later blasts and its ammunition'
              ' cooking off - the wreck takes the area damage too, x 2 in state 4)', '%.3f' % h)
        else:
            c(not area, '25 m: outside (19.1 + 5.45 m)', '%d lines' % len(area))


@sequence
def walk_into_structure(c):
    """walking into a destructible structure damages it (engine 0x10019d84 -> 0x1000c3c0 -> 0x10046c20): YELLARE6's
    chemical plant (S_BCHEMA, a type 0 box with a thing) walked into head-on at FULL (59.5 km/h = 9.08 cm/tick) takes
    (9.08 - 3.069) x 0.0501 x 3 = 0.9 at its centre - the same as the player's own collision damage, and whatever the
    Collision Damage setting; the impact sound is MECBLDC1 0xc8 (the object has a thing, 0x10019cf8; MECMTNHD 0xe5 for
    the terrain or a plain object); the class 0xb0 branch (0x1000c3cb) runs only in the network game (DAT_1024ffcc)"""
    def go(tag, files=None):
        return c.run(tag, secs=13, keys=[(5, '0')], dump='/0.1', nostart=True, files=files,
                     env={'MW2_START': '319122,262000', 'MW2_START_HEADING': 180, 'MW2_DAMAGE_LOG': '1', 'MW2_SFX_LOG': '1'})
    r = go('')
    st = [float(l.split()[-1]) for l in r.out.splitlines() if l.startswith('damage structure Chem.Plant by player')]
    c(st and 0.8 < st[0] < 1.0, 'the plant takes the impact damage (0.9)', str(st))
    snd = [int(m.group(1)) for m in re.finditer(r'SFXLOG -?[\d.]+ snd (\d+)', r.out)]
    c(0xc8 in snd and 0xe5 not in snd, 'the impact sounds MECBLDC1 (a destructible object)', str([hex(x) for x in snd if x in (0xc8, 0xe5)]))
    r = go('_nodmg', files={'MW2DIF.CFG': bytes([0, 0, 1, 0, 1, 1, 0, 0])})
    st = [float(l.split()[-1]) for l in r.out.splitlines() if l.startswith('damage structure Chem.Plant by player')]
    pl = [l for l in r.out.splitlines() if l.startswith('damage terrain player')]
    c(st and 0.8 < st[0] < 1.0 and not pl, 'Collision Damage off: the plant still takes it, the player none', '%s %s' % (st, pl[:2]))


@sequence
def downhill_flight(c):
    """no snap-down (engine 0x10019310 -> 0x10019e28: the unit stands only with its feet at or under the ground, asked at
    its centre height 0x10010080; off anything lower it is airborne, +0x78 clear, and falls at 0.0296 cm/tick^2): CYANSCN1
    straight ahead at FULL, the mech walks up a 37 m rise and leaves its crest at ~25 m/s - airborne ~3.6 s, it lands
    at ~11.5 cm/tick with MECMTNSF 0xe6 (the port had followed the ground down by up to 10 m a frame)"""
    r = c.run(secs=44, throttle=1, twist=0, mission='CYANSCN1', dump='/0.1', env={'MW2_SFX_LOG': '1'})
    top = max(r.span(30, 44), key=lambda s: s['y'])
    c(top['y'] > 3500, 'up the rise', 'top y %d at %.1f s' % (top['y'], top['t']))
    fall = [s for s in r.span(top['t'], 44) if s['vy'] < -1.0]
    c(len(fall) > 20 and min(s['vy'] for s in fall) < -9.0, 'off the crest: a ballistic fall (vy below -9 cm/tick)',
      'vy min %.1f over %d samples' % (min([s['vy'] for s in fall] or [0]), len(fall)))
    snd = [(float(m.group(1)), int(m.group(2))) for m in re.finditer(r'SFXLOG (-?[\d.]+) snd (\d+)', r.out)]
    land = [t for t, i in snd if i == 0xe6 and t > top['t']]
    c(land, 'lands with MECMTNSF', str([(t, hex(i)) for t, i in snd if i in (0xe5, 0xe6)]))
@sequence
def death_stops_or_falls(c):
    """state 4 (destroyed): the physics 0x10019310 (DOS 0x26990) returns at once for a unit in state 4 that is on the
    ground (object +0x78) - no move, no servo: it stops dead where it died (the controller's cleared drive never acts).
    Killed in the air it flies on at its velocity (no drive), falls, and freezes on touching down (0x1000c4d0 sets +0x78;
    no fall damage: it returns before 0x100171d0). Player self-destructs walking at full; a Summoner self-destructs jetting"""
    r = c.run(secs=9, keys=[(0.2, '0'), (2.0, 'ctrl+alt+x')], nostart=True, dump='/0.1')
    d = next((s for s in r.states if s['dead']), None)
    c(d is not None, 'walking: the player self-destructs')
    if not d:
        return
    b = [s for s in r.states if s['t'] < d['t']]
    c(b and b[-1]['kmh'] > 40, 'walking at speed before', str(b[-1]['kmh'] if b else None))
    after = r.span(d['t'], 9.0)
    c(after and len(set((s['x'], s['z']) for s in after)) == 1, 'walking: stops dead at death (x, z frozen)',
      str([(s['x'], s['z']) for s in after[:4]]))
    r2 = c.run('_air', secs=12, mech='summoner/smn00std', keys=[(0.2, '0'), (2.0, 'ctrl+alt+x')], hold=[(2.5, 6.0, 'j')],
               nostart=True, dump='/0.1')
    d2 = next((s for s in r2.states if s['dead']), None)
    c(d2 is not None, 'jetting: the player self-destructs')
    if not d2:
        return
    c(d2['y'] > d2['gnd'] + 100, 'killed in the air', 'y %s gnd %s' % (d2['y'], d2['gnd']))
    land = next((s for s in r2.states if s['t'] > d2['t'] and s['y'] <= s['gnd'] + 1), None)
    c(land is not None, 'falls to the ground')
    if not land:
        return
    c((land['x'], land['z']) != (d2['x'], d2['z']), 'flies on while falling', '%s -> %s' % ((d2['x'], d2['z']), (land['x'], land['z'])))
    rest = r2.span(land['t'] + 0.2, 12.0)
    c(rest and len(set((s['x'], s['z'], s['y']) for s in rest)) == 1, 'frozen after touching down', str([(s['x'], s['z']) for s in rest[:3]]))


@sequence
def jets_climb_and_release_dos(c):
    """directional jets on the ground (DOS 0x26990 / 3Dfx 0x10019310): thrust T = (jump << 16) / walk x 0x1e50 >> 16
    (DOS 0x4ceaa; 3Dfx x 0.1184, 0x10041410), drag f = kS + B toward the terminal speed L = 2.5 x top, k and B in the DOS
    integers (msim_jet_drag: k = 3 for the floats' 3.85 - the floats ran 16 kph ahead at 1.4 s); on release the
    velocity eases to the walking velocity by (target - v) / max(45, frame ticks) per tick. DOSBox ORANSCN1 Nova at
    320x200 (~8-tick frames, ZMBV capture), backward jets (GreyEnd) from rest: 72 / 144 / 207 / 267 / 307 / 337 / 355 /
    372 / 376 kph at 0.42 / 0.92 / 1.42 / 1.92 / 2.42 / 2.92 / 3.42 / 3.92 / 4.12 s; released at 378: 303 / 255 / 192 /
    132 / 85 / 43 / 22 after 0.04 / 0.1 / 0.19 / 0.27 / 0.33 / 0.46 / 0.61 s, 0 by ~1 s. (At 1024x768 DOSBox ran at
    35-75 ticks a frame: the drift vanished in one frame - frames of 45 ticks or more set the velocity outright)"""
    r = c.run(secs=12, hold=[(6, 10.1, 'end')], dump='/1', nostart=True, mission='ORANSCN1', mech='nova/nva00std',
              env={'MW2_KPH_TRACE': '1'})
    k = [(float(m.group(1)), int(m.group(2))) for m in re.finditer(r'^kph (\d+\.\d+)s (-?\d+) kph', r.out, re.M)]
    t0 = next((t for t, v in k if t > 5.9 and v >= 4), None)
    c(t0 is not None, 'the jets push')
    if t0 is None:
        return
    def at(t):
        q = [v for tt, v in k if tt <= t]
        return q[-1] if q else 0
    dos = ((0.42, 72), (0.92, 144), (1.42, 207), (1.92, 267), (2.42, 307), (2.92, 337), (3.42, 355), (3.92, 372), (4.0, 376))
    got = [(d, at(t0 + dt)) for dt, d in dos]
    c(all(abs(g - d) <= 10 for d, g in got), 'climb within 10 kph of DOSBox (the DOS integer drag: the floats ran 16 kph ahead at 1.4 s)', str(got))
    rel = next((t for t, v in k if t > 10.0 and v < at(10.0) - 20), None)
    c(rel is not None, 'released: the reading falls')
    if rel is None:
        return
    a, b, z = at(rel + 0.1), at(rel + 0.33), at(rel + 1.2)
    c(150 <= a <= 320 and 40 <= b <= 140 and z <= 5, 'eases off over ~1 s (DOSBox 255 at 0.1 s, 85 at 0.33 s, 0 by ~1 s)',
      '%d %d %d' % (a, b, z))


@sequence
def player_hit_effect_flag(c):
    """the impact effect of a shot striking the player (engine 0x100453xx -> 0x10045d00): flag 0x2000 - the player's
    own impact types (lasers 0x0e-0x10, missiles 0x11) - only when the part struck is in location 1-4 (head, right /
    centre / left torso: the part's +0x16) and the player is still alive after the hit; arms and legs show the plain unit
    effect (laser 0-2, missile 3 / 4). AQUASCN1, the player standing under fire"""
    PL, PLAIN = (0x0e, 0x0f, 0x10, 0x11, 0x12), (0, 1, 2, 3, 4)   # PPC (6) has no player variant
    # the cockpit's LOD 4 pieces (all CT); the AI aims at the eye, so the limbs are forced (TEST hook) - a leg, an arm
    for suffix, env in (('', {}), ('_leg', {'MW2_TEST_PLHIT_GROUP': '7'}), ('_arm', {'MW2_TEST_PLHIT_GROUP': '5'})):
        env.update({'MW2_FX_TRACE': '1', 'MW2_DEBUG_INVULN': '1'})
        r = c.run(suffix, secs=30, throttle=0, twist=0, mission='AQUASCN1', dump='/1', env=env)
        ev = [tuple(int(x) for x in l.split()[3::2]) for l in r.out.splitlines() if l.startswith('plfx ')]
        torso = [t for g, t in ev if 1 <= g <= 4]
        limbs = [t for g, t in ev if g >= 5]
        c(torso if not suffix else limbs, suffix + ': hits seen', '%d / %d' % (len(torso), len(limbs)))
        c(torso and any(t in PL for t in torso) or suffix, suffix + ': head / torso hits: the player types', str(sorted(set(torso))))
        c(not any(t in PLAIN for t in torso), suffix + ': head / torso hits: no plain types', str(sorted(set(torso))))
        c(not any(t in PL for t in limbs), suffix + ': arm / leg hits: the plain unit types', str(sorted(set(limbs))))


@sequence
def walk_run_carried_offset(c):
    """the engine never sets a node's pose: every key adds its delta (0x1000dec0: +0x18 = the key's delta + what is left,
    applied through 0x1000f850 / 0x1000f9d0; the loader 0x100401c0 keeps the ANIM values), so a transition into a key
    whose table pose differs from the pose reached carries the difference. Mad Dog: walk 5 -> run 16 (MADDANIM, 0x40
    jump at walk key 4 to the run's 0x200 bridge key) leaves one hip 17 degrees while running; the run -> walk jump
    takes it back off"""
    r = c.run(secs=16, mech='maddog/mdg00std', keys=[(0.2, '5'), (6.0, '0'), (11.0, '5')], nostart=True, dump='/0.25')
    walk, run, back = r.span(3.0, 5.9), r.span(9.0, 10.9), r.span(14.5, 16.0)
    c(walk and all(s['pseq'] == 0 and s['poff'] < 0.01 for s in walk), 'walking: no offset', str([(s['pseq'], s['poff']) for s in walk[:3]]))
    c(run and all(s['pseq'] == 1 for s in run), 'running at FULL (sequence 1)', str([s['pseq'] for s in run[:3]]))
    c(run and all(abs(s['poff'] - 17.0) < 0.01 for s in run), 'running: the hip carries 17 degrees', str(sorted(set(s['poff'] for s in run))))
    c(back and all(s['pseq'] == 0 and s['poff'] < 0.01 for s in back), 'back to walking: taken off again', str([(s['pseq'], s['poff']) for s in back[:3]]))

@sequence
def node_task_guard_patrol(c):
    """an enemy star's protect node (kind 4) on a thing (0x10014ba0 -> state 7 with the target, 0x10013f00): four 30 m nav
    points at x - R, z - R, x + R, z + R round it (R 250 m for a thing), the goal the first own point in the nav table; on
    arriving the DEFLT state-7 rule (condition 3, ref 0x2a) re-enters patrol with the target (ref2 0x40): every other
    point deleted (0x10013e90 steps on past each deletion), four more made, the goal again the first left - the -z point
    of the first ring next. SILVSCN1: the Gargoyle (actor 3, table 2) guards silvare2 (3050, -1058) for 30 s - the port
    had left kind 4 unapplied (the leader stood, its wingman followed)"""
    r = c.run(secs=34, throttle=0, twist=0, mission='SILVSCN1', dump='/0.25', nostart=True,
              env={'MW2_WATCH_ACTOR': '3', 'MW2_DEBUG_NODE_TASKS': '1'})
    tasks = [l for l in r.out.splitlines() if l.startswith('node task') and 'kind 0x4 ' in l and 'silvare2' in l]
    c(len(tasks) == 2 and all('-> 7 ' in l for l in tasks), 'both members of table 2 put in state 7 on silvare2', str(tasks))
    # the protect node completes at 31 s (then kind 2 on the userstar): the Gargoyle, online only after its start-up
    # (0x43e + rand(0x16a) ticks, 0x1001ab97), reaches the first point at ~28.5 s - so the turn for the second is checked
    # over the last ~2.5 s, sampled every 0.25 s (the 1 s dump had left one sample there: the test, not the port, was wrong)
    pts = [(s['t'], s['w_x'], s['w_z']) for s in r.states if 'w_x' in s and s['t'] <= 31.0]
    d = [((x + 21950) ** 2 + (z + 1058) ** 2) ** 0.5 for t, x, z in pts]
    k = d.index(min(d)) if d else -1
    c(d and min(d) < 3500, 'reaches the first point, x - 250 m (-21950, -1058)', '%.0f cm' % (min(d) if d else -1))
    later = pts[k + 1:] if k >= 0 else []
    ok = len(later) >= 4 and later[-1][2] < pts[k][2] - 2000
    if ok:   # the last leg's bearing within 30 deg of the second point's (3050, -26058)
        ex, ez = later[-1][1] - later[-5][1], later[-1][2] - later[-5][2]
        gx, gz = 3050 - later[-5][1], -26058 - later[-5][2]
        ok = (ex * gx + ez * gz) / max(1.0, math.hypot(ex, ez) * math.hypot(gx, gz)) > math.cos(math.radians(30))
    c(ok, 'then heads for the second, z - 250 m (3050, -26058)', '%s -> %s' % (pts[k][1:] if k >= 0 else '-', [p[1:] for p in later]))


@sequence
def wreck_takes_area_damage(c):
    """area damage (0x10046700) has no state test and doubles for a unit in state 4 (x DAT_102485f8 = 2): a destroyed mech
    standing in its own death explosion (0x0d, 579 ticks) takes 2 x ticks / 256 on locations 2-8 - about 4.5 each - and
    so loses locations / cooks off its ammunition as the engine's wrecks do. AQUASCN1, every enemy destroyed at 3 s; the
    port had skipped destroyed units"""
    r = c.run(secs=10, throttle=0, twist=0, mission='AQUASCN1', dump='/1', nostart=True, edition='dos',
              env={'MW2_DAMAGE_LOG': '1', 'MW2_TEST_KILL_ALL': '3:0'})
    wreck = [l.split() for l in r.out.splitlines() if l.startswith('damage area wreck')]
    by = {}
    for f in wreck:
        by.setdefault((f[3], int(f[5])), 0.0)
        by[(f[3], int(f[5]))] += float(f[-1])
    leg = [v for (u, loc), v in by.items() if loc == 7]
    c(wreck and leg, 'the wrecks take area damage', '%d lines' % len(wreck))
    c(leg and max(leg) >= 4.0, 'x 2 in state 4: a leg over 4 points from the death explosion (579 x 2 / 256 = 4.5)', '%s' % ['%.2f' % v for v in leg])


@sequence
def dos_renderer_dos_rules(c):
    """the DOS renderer is the DOS edition (the shell sets renderer=dos with MW2_EDITION=dos, portcfg_edition): its rules
    follow whatever MW2_EDITION says - a CT hit to RT / CT only (MW2.EXE 0x263c8 rand(2)); the 3D renderers keep rand(3)"""
    def loc(st, i):
        return float(st['parmor'].split(',')[i])
    r = c.run(secs=6, nostart=True, dos_look=True, env={'MW2_EDITION': 'enhanced', 'MW2_TEST_PHIT': '3:3:23:40'})
    a, b = r.at(2.0), r.at(5.0)
    lost = [loc(a, i) - loc(b, i) for i in (1, 2, 3)]
    c(lost[0] > 0 and lost[1] > 0 and lost[2] == 0, 'DOS renderer: locations 2 and 3 only', '%s' % lost)


@sequence
def mission_config_gravity_override(c):
    """the PLNT handler (0x1003de10) takes the gravity from the mission configuration (DAT_1024ac6c +0xf, 16.16, when not 0;
    +0x13 the temperature) - MW2DIF.CFG read into 0x17 zeroed bytes (0x100408a0). The shell writes 8 bytes, so normally
    the planet's; a 0x17-byte file with 1.2 g on ORANSCN1 (0.6 g) halves the Timber Wolf's top speed (133 kph)"""
    dif = bytearray([0, 0, 1, 1, 1, 1, 0, 0] + [0] * 15)
    dif[0xf:0x13] = int(1.2 * 65536).to_bytes(4, 'little')
    r = c.run(secs=10, keys=[(1, '0')], dump='/1', nostart=True, mission='ORANSCN1', mech='timbrwlf/tbr00std',
              env={'MW2_KPH_TRACE': '1'}, files={'MW2DIF.CFG': bytes(dif)})
    got = kph_at(r, 8.5, 10)
    c(got and 60 <= min(got) <= max(got) <= 70, '1.2 g from MW2DIF +0xf: FULL about 66 kph (133 at 0.6 g)', str(sorted(set(got))))


def voice_off_cfg():
    """MW2SND.CFG with the VOICE slider at 0 (+8): radio lines are not played, so the radio is idle in simulation time
    (the headless audio otherwise runs on the wall clock)"""
    snd = bytearray(open(os.path.join(GTEST, 'MW2SND.CFG'), 'rb').read())
    snd[8:12] = b'\0\0\0\0'
    return {'MW2SND.CFG': bytes(snd)}


@sequence
def dos_end_prompt(c):
    """the DOS end (MW2.EXE 0x16e80): the player's star resolved (here: self-destruct, failed at once), the radio idle
    (DAT_00097ddc; the voice off so it is idle in simulation time) - "Press any key to exit..." past 3 whole seconds
    (0x11020: 0x1554 ticks, priority 100), never the 3D editions' "Press CTRL-Q to exit..."; with no key the mission ends
    past 0x14 s (DAT_00095864 += 2); a key once the prompt is up ends it at once (0x45d80). DOSBox YELLSCN1 (sound off):
    the prompt 5.4 s after Ctrl+Alt+x, the exit fade 21.0 s after the prompt"""
    SD = 8
    r = c.run(secs=SD + 26, keys=[(SD, 'ctrl+alt+x')], edition='dos', dump='/0.5', files=voice_off_cfg())
    d = next((s for s in r.states if s['dead']), None)
    tp = r.msg_time('Press any key to exit')
    c(d is not None, 'destroyed')
    c(r.msg_time('Press CTRL-Q') is None, 'no CTRL-Q prompt in the DOS look')
    c(tp is not None and d is not None and 3000 <= tp - d['now'] <= 4600, 'the prompt 3-4 s after the end (whole seconds)',
      '%s' % (tp and d and tp - d['now']))
    ov = r.tag('over') or (r.last if r.last.get('over') else None)
    c(ov is not None and d is not None and 20000 <= ov['now'] - d['now'] <= 21600, 'over by itself past 0x14 s',
      '%s' % (ov and d and ov['now'] - d['now']))
    r2 = c.run('_key', secs=SD + 8, keys=[(SD, 'ctrl+alt+x'), (SD + 6, 'space')], edition='dos', dump='/0.25', files=voice_off_cfg())
    tp2 = r2.msg_time('Press any key to exit')
    o2 = r2.tag('over') or (r2.last if r2.last.get('over') else None)
    c(tp2 is not None and tp2 < (SD + 5.5) * 1000, 'prompt up before the key', '%s' % tp2)
    c(o2 is not None and o2['now'] < (SD + 6.6) * 1000 and r2.before(SD + 5.9)['over'] == 0, 'the key ends the mission',
      '%s' % (o2 and o2['now']))
    r3 = c.run('_3d', secs=12, keys=[(8, 'ctrl+alt+x')], dump='/0.5')
    c(r3.msg_time('Press CTRL-Q') is not None and r3.msg_time('Press any key') is None, '3D editions: CTRL-Q prompt')


@sequence
def scrub_patch_follow(c):
    """the scrub patch (TNJ1SCRI's SCRG: 3 x 3 tiles of TNJ1SCR1) follows the camera on a 16384 cm grid (engine
    0x100113f0 / 0x10011570, DOS 0x1f120 / 0x1f270): started 40 km out it moves to the nearest grid point; TERRAIN
    TEXTURES off (MW2SND.CFG +0x18 = 0): unlinked - no scrub billboards drawn (0x100116d0)"""
    r = c.run(secs=1.5, mission='TNJ1SCN1', nostart=True, env={'MW2_START': '40000,-30000'})
    m = re.findall(r'^SCRG now=\d+ at (-?\d+),(-?\d+)', r.out, re.M)
    c(m and m[0] == ('32768', '-32768'), 'the patch at the camera\'s grid point (2, -2) x 16384', '%s' % m[:2])
    c(r.at(1).get('billboards') == 234, 'its billboards drawn', '%s' % r.at(1).get('billboards'))
    snd = bytearray(open(os.path.join(GTEST, 'MW2SND.CFG'), 'rb').read())
    snd[0x18:0x1c] = b'\0\0\0\0'
    r2 = c.run('_off', secs=1.5, mission='TNJ1SCN1', nostart=True, files={'MW2SND.CFG': bytes(snd)})
    c(r2.at(1).get('billboards') == 0, 'TERRAIN TEXTURES off: the patch unlinked', '%s' % r2.at(1).get('billboards'))
def titanium_chase_env(name, tdir, secs):
    """TEXTURES TITANIUM with the external camera (the autopilot's cockpit field 0)"""
    return {'MW2_CONFIG': titanium_cfg(name, tdir), 'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'), 'MW2_TEXTURES': 'titanium',
            'MW2_AUTOPILOT': '%g,0,-999,0,%s,0,0' % (secs, os.path.join(OUT, name, 'final.ppm'))}


@sequence
def titanium_models(c):
    """TEXTURES TITANIUM draws the Titanium archive's own models with its own texture table, so mechs and buildings take
    the Titanium textures too (they were renamed: the 3D editions' slot names mostly miss): the world objects by record
    and OBJ index (renamed S_B* -> S_1*, the effect pool MW2_SHT1 left alone), the player's Timber Wolf as the Titanium
    TIMBRWLF (its own skeleton offsets, the same ANIM) with the star's CAMO set (CYAN: star 0 -> set 6, plates g); the
    simulation keeps the original data. CYANSCN1 from behind the mech, against TEXTURES ORIGINAL (skipped without the
    titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    shots = [(2.5, 'view')]
    r = c.run(secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots, env=titanium_chase_env('titanium_models', tdir, 3))
    m = re.search(r'titanium: (\d+) of (\d+) world objects with a Titanium model', r.out)
    c(m is not None and int(m.group(1)) > 300, 'world objects drawn as their Titanium models', m.group(0) if m else 'no line')
    lm = re.search(r'titanium: lights .* camo ((?:\d+ ?){8})', r.out)
    c(lm is not None and lm.group(1).split()[0] == '6', 'CYAN\'s CAMO chunks read (star 0 -> set 6)', lm.group(0) if lm else 'no line')
    o = c.run('_orig', secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots,
              env={'MW2_AUTOPILOT': '3,0,-999,0,%s,0,0' % os.path.join(OUT, 'titanium_models_orig', 'final.ppm')})
    # the mech, from behind: the ATi Timber Wolf's camouflage is blue-green, the Titanium plates g brown
    teal = lambda R, G, B: (G > R + 8) & (B > R + 8)
    box = (0.35, 0.45, 0.65, 0.85)
    tt, to = count_px(r.shots.get('view'), teal, box), count_px(o.shots.get('view'), teal, box)
    c(to > 500 and 0 <= tt < to // 4, 'the mech in the Titanium plates, not the ATi camouflage', 'blue-green pixels %d (original %d)' % (tt, to))
    d = mean_diff(r.shots.get('view'), o.shots.get('view'), box=box)
    c(r.last.get('t', 0) >= 2.9 and o.last.get('t', 0) >= 2.9, 'both runs go on')
    c.note('%s; mech box: blue-green pixels %d (original %d), mean diff %.1f' % (m.group(0) if m else '?', tt, to, d))


@sequence
def titanium_lights(c):
    """the Titanium lights from SKYGND.PAR (mercsw 0x10053dc5..0x10053ec0 -> the light table 0x10097cc0): amb, wa, wb
    divided by 255, a 'P' light a point else a direction; the ground's one colour (0x1005527e) = amb + each directional
    light x |its y| / |its vector| + each point light whole, clamped to 1 - checked against the file for CYAN and TNJ1
    (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    par = {}
    with open(os.path.join(tdir, 'SKYGND.PAR'), errors='replace') as f:
        for line in f:
            line = line.split('#')[0]
            mm = re.match(r'mission=(\w{4})', line.strip())
            if mm and mm.group(1).lower() not in par:
                par[mm.group(1).lower()] = line
    for mission in ('CYANSCN1', 'TNJ1SCN1'):
        line = par.get(mission[:4].lower(), '')
        amb = [int(x) for x in re.search(r'amb=(\d+),(\d+),(\d+)', line).groups()]
        lights = [re.search(r'%s=(\d+),(\d+),(\d+),(\w),([-.\d]+),([-.\d]+),([-.\d]+)' % k, line).groups() for k in ('wa', 'wb')]
        want = []
        for ch in range(3):
            g = amb[ch] / 255.0
            for L in lights:
                col, typ, vec = int(L[ch]) / 255.0, L[3], [float(x) for x in L[4:7]]
                ln = math.sqrt(sum(x * x for x in vec))
                g += col if typ == 'P' else (col * abs(vec[1]) / ln if ln > 0 else 0)
            want.append(min(1.0, g))
        r = c.run('_' + mission[:4].lower(), secs=1, throttle=0, nostart=True, mission=mission,
                  env={'MW2_CONFIG': titanium_cfg('titanium_lights_' + mission[:4].lower(), tdir),
                       'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'), 'MW2_TEXTURES': 'titanium'})
        lm = re.search(r'titanium: lights amb ([\d.]+) ([\d.]+) ([\d.]+) wa (\d) ([\d.]+) ([\d.]+) ([\d.]+) wb (\d) ([\d.]+) ([\d.]+) ([\d.]+) '
                       r'ground ([\d.]+) ([\d.]+) ([\d.]+)', r.out)
        c(lm is not None, '%s: the lights line' % mission)
        if not lm:
            continue
        got = [float(x) for x in lm.groups()]
        c(all(abs(got[i] - amb[i] / 255.0) < 0.002 for i in range(3)), '%s: amb / 255' % mission, str(got[:3]))
        c(int(got[3]) == (2 if lights[0][3] == 'P' else 1) and int(got[7]) == (2 if lights[1][3] == 'P' else 1), '%s: light types' % mission)
        c(all(abs(got[11 + i] - want[i]) < 0.002 for i in range(3)), '%s: the ground colour' % mission, 'got %s want %s' % (got[11:14], want))


@sequence
def titanium_smoke_and_lights(c):
    """TEXTURES TITANIUM, an LRM volley (group fire): the missiles leave the Titanium smoke (particle.PID emitters, mercsw
    0x1004df04 / 0x1004d7ba) and their blasts light the objects (0x100123cd: point lights (0.6, 0.6, 0.2), +-200 m); with
    MW2_NO_PARTICLES none. Needs particle.PID beside the Titanium MW2.PRJ (or in its install folder) - skipped without"""
    tdir = titanium_dir()
    pid = None
    for d in (tdir, os.path.dirname(os.path.realpath(os.path.join(tdir, 'MW2.PRJ'))) if tdir else None):
        if d and os.path.exists(os.path.join(d, 'particle.PID')):
            pid = os.path.join(d, 'particle.PID')
    if not tdir or not pid:
        c.note('no titanium/ folder or no particle.PID: skipped')
        return
    tmp = os.path.join(OUT, 'titanium_smoke_dir')   # the three files side by side
    os.makedirs(tmp, exist_ok=True)
    for fn, src in (('MW2.PRJ', os.path.join(tdir, 'MW2.PRJ')), ('SKYGND.PAR', os.path.join(tdir, 'SKYGND.PAR')), ('particle.PID', pid)):
        dst = os.path.join(tmp, fn)
        if not os.path.exists(dst):
            os.symlink(os.path.realpath(src), dst)
    shots = [(3.6, 'smoke')]
    keys, hold = [(2.5, 'backslash')], [(2.8, 3.0, 'space')]
    env = titanium_chase_env('titanium_smoke', tmp, 4)
    r = c.run(secs=4, throttle=0, nostart=True, mission='CYANSCN1', shots=shots, keys=keys, hold=hold, env=env)
    c(re.search(r'titanium: missile smoke from ([\d.]+) s', r.out) is not None, 'the missiles smoke', r.out[-300:])
    t = c.run('_light', secs=5, throttle=0, nostart=True, mission='CYANSCN1', keys=keys,
              hold=[(1.0, 2.4, 'up')] + hold, env=titanium_chase_env('titanium_smoke_light', tmp, 5))   # (torso down: it lands)
    c('titanium: explosion light at' in t.out, 'an explosion light (the volley\'s blasts on the ground)')
    env2 = titanium_chase_env('titanium_smoke_off', tmp, 4)
    env2['MW2_NO_PARTICLES'] = '1'
    o = c.run('_off', secs=4, throttle=0, nostart=True, mission='CYANSCN1', shots=shots, keys=keys, hold=hold, env=env2)
    c('missile smoke' not in o.out, 'MW2_NO_PARTICLES: none')
    d = mean_diff(r.shots.get('smoke'), o.shots.get('smoke'), box=(0.3, 0.25, 0.7, 0.7))
    c(d > 1.0, 'the trails show (the picture differs from the one without particles)', 'mean diff %.2f' % d)
    c.note('trail box mean diff with / without the particles: %.2f' % d)
@sequence
def powerup_snow_clipped(c):
    """display damage while the windows close / open (0x10021730 / 0x10011aa0 set the window record to the moving box,
    then 0x10021300 / 0x10011720 -> 0x10021710 / 0x10011a40 -> 0x100022f0 draws SNOWCLR at the window's top-left,
    clipped to it): with the target viewer (13) and the viewport window (2) at level 3, the static shows in the
    closing boxes too, and none outside them"""
    grey = lambda r, g, b: (b > r + 60) & (b > g + 60)   # SNOWCLR's blue specks (the sand and the black box have none)
    tbox, tout = (0.06, 0.79, 0.17, 0.83), (0.045, 0.735, 0.19, 0.745)   # target display: centre band / its top rows
    vbox, vout = (0.83, 0.79, 0.95, 0.83), (0.815, 0.733, 0.965, 0.743)
    shots = [(UP - 0.5, 'up'), (UP + 0.15, 'closing'), (UP + 0.3, 'closing2')]
    r = c.run(secs=UP + 1, keys=[(UP - 2, 'e'), (UP, 's')], shots=shots, size='1024x768',
              env={'MW2_VPORT': '1', 'MW2_TEST_DISPLAY': '%g:13:3,%g:2:3' % (UP - 3, UP - 3)})
    for nm in ('up', 'closing', 'closing2'):
        c(count_px(r.shots[nm], grey, tbox) > 50, nm + ': static in the target display', '%d px' % count_px(r.shots[nm], grey, tbox))
        c(count_px(r.shots[nm], grey, vbox) > 50, nm + ': static in the viewport window', '%d px' % count_px(r.shots[nm], grey, vbox))
    c(r.at(UP - 1).get('tgt', -1) >= 0, 'a target selected', 'tgt %s' % r.at(UP - 1).get('tgt'))
    whole = (0.035, 0.73, 0.2, 0.895)
    fu = count_px(r.shots['up'], grey, whole)
    c(fu > 500, 'full size: the static fills the target display', '%d px' % fu)
    for nm in ('closing', 'closing2'):
        c(count_px(r.shots[nm], grey, tout) < 10, nm + ': none outside the closing target box', '%d px' % count_px(r.shots[nm], grey, tout))
        c(count_px(r.shots[nm], grey, vout) < 10, nm + ': none outside the closing viewport box', '%d px' % count_px(r.shots[nm], grey, vout))


@sequence
def damage_outline_from_hud_record(c):
    """the damage outline is the SHP the mech's HUD record names: value 11 (+0x28) = the outline's ID in the 320x200
    set, + 1 / + 2 the 640x480 / 1024x768 sets (loader 0x100403a0 -> 0x101de8e0, read by 0x1001b230 / 0x1001b620 with
    the set index 0x102453c8); values 12 / 13 are 0 in every record and unread. The Warhawk's record names WARKDMG,
    the Timber Wolf's TIMBDMG: each player's outline shows in the bottom-right box"""
    box = (0.80, 0.72, 0.98, 0.92)
    lit = lambda r, g, b: (r + g + b > 150) & ((abs(r - g) > 40) | (abs(g - b) > 40))   # the outline's colours, not the sand
    for mech in ('timbrwlf/tbr00std', 'warhawk/whk00std'):
        nm = mech.split('/')[0]
        r = c.run('_' + nm, secs=UP + 1, mech=mech, shots=[(UP, 'o')], size='1024x768')
        n = count_px(r.shots['o'], lit, box)
        c(n > 300, nm + ': its damage outline drawn', '%d px' % n)


@sequence
def hud_mission_palette(c):
    """the HUD is drawn in the mission's palette, not the COCKPIT record: the engine's 16-bit table 0x10260324 is filled
    only by 0x1002ac40 (RGB555 of the current PALG slot's PAL record), DOS draws in the VGA palette (DOSBox YELLSCN1:
    viewport frame 0,0,199 and the bars' light blue 0,48,215 = YELL_DA 6 / 7; COCKPIT has 0,0,190 / 60,60,255). The
    cold heat bar's light band (index 7): YELL_DA 0,12,53 -> 0,48,215; FUCH 0,12,56 -> 0,48,227; SILV 2,12,49 -> 8,48,199"""
    box = (0.3, 0.75, 0.7, 0.95)
    exact = lambda rgb: (lambda r, g, b: (abs(r - rgb[0]) <= 2) & (abs(g - rgb[1]) <= 2) & (abs(b - rgb[2]) <= 2))
    for scene, rgb in (('YELLSCN1', (0, 48, 215)), ('FUCHSCN1', (0, 48, 227)), ('SILVSCN1', (8, 48, 199))):
        r = c.run('_' + scene, secs=UP + 0.5, mission=scene, shots=[(UP, 'h')], size='1024x768', env={'MW2_DEBUG_HUDPAL': '1'})
        n, old = count_px(r.shots['h'], exact(rgb), box), count_px(r.shots['h'], exact((60, 60, 255)), box)
        c(n > 100 and old < 20, scene + ': the heat bar in the mission palette', '%d px %s, %d px COCKPIT 60,60,255' % (n, rgb, old))


@sequence
def hud_sprite_set_by_size(c):
    """the HUD sprite set (SHP ID + 0 / 1 / 2): the engine takes the entry of 320x200 / 640x480 / 1024x768 (0x102492a0)
    nearest the screen by |dw| + |dh|, the first of equals (0x100024b0 -> 0x102453c8); the port applies it to the
    drawable size: 640x480 and 800x600 the 6 set, 1024x768, 960x720 and 1280x720 the K set"""
    for size, want in (('640x480', '6'), ('800x600', '6'), ('960x720', 'K'), ('1280x720', 'K'), ('1024x768', 'K')):
        r = c.run('_' + size, secs=1, size=size, shots=[(0.5, 's')], nostart=True, env={'MW2_DEBUG_HUDPAL': '1'})
        m = re.search(r"hud set: '(\w*)' for (\d+)x(\d+)", r.out)
        c(m is not None and m.group(1) == want, size + ': the ' + want + ' set', m.group(0) if m else 'no line')


@sequence
def command_panels(c):
    """the in-mission panels (menu records 0x102653f0; layout 0x10065580, draw 0x10065df0): u (USER_MENU) opens SYSTEMS
    STATUS (menu 5) - Light Amplification / Image Enhancement / HUD / Auto Eject toggled by their digits, the panel
    stays; b the COMMAND COMPUTER root (NO STAR MATES with no live mate), Ctrl+F2 COMMAND POINT 2 (NOT AVAILABLE); a key
    closes its own menu. Layout as DOSBox YELLSCN1 at 1024 x 768: COMMAND POINT 2 title at 225,241 px, rows from 276
    every 23; SYSTEMS STATUS title 563,267, rows from 309 every 28"""
    keys = [(UP, 'u'), (UP + 0.5, '1'), (UP + 1.0, '4'), (UP + 1.5, 'u'), (UP + 2.0, 'b'), (UP + 2.5, 'esc'), (UP + 3.0, 'ctrl+f2')]
    shots = [(UP + 0.3, 'sys'), (UP + 2.3, 'root'), (UP + 3.3, 'pt2')]
    r = c.run(secs=UP + 3.6, keys=keys, shots=shots, size='1024x768', env={'MW2_CC_TRACE': '1'})
    at = lambda t: r.at(t)
    c(at(UP + 0.2).get('cc') == 100, 'u: SYSTEMS STATUS', 'cc %s' % at(UP + 0.2).get('cc'))
    c(at(UP + 0.7).get('vision') == 1 and at(UP + 0.7).get('cc') == 100, '1: light amplification on, the panel stays',
      'vision %s cc %s' % (at(UP + 0.7).get('vision'), at(UP + 0.7).get('cc')))
    c(at(UP + 1.2).get('autoeject') == 1, '4: auto eject on', 'autoeject %s' % at(UP + 1.2).get('autoeject'))
    c(at(UP + 1.7).get('cc') == -1, 'u again: closed', 'cc %s' % at(UP + 1.7).get('cc'))
    c(at(UP + 2.2).get('cc') == 0, 'b: the COMMAND COMPUTER root', 'cc %s' % at(UP + 2.2).get('cc'))
    c(at(UP + 2.7).get('cc') == -1, 'Esc: closed', 'cc %s' % at(UP + 2.7).get('cc'))
    c(at(UP + 3.2).get('cc') == 3, 'Ctrl+F2: COMMAND POINT 2', 'cc %s' % at(UP + 3.2).get('cc'))
    lay = re.findall(r'cc layout: title ([\d.]+),([\d.]+) rows ([\d.]+),([\d.]+) pitch ([\d.]+)', r.out)
    sysl = [tuple(float(x) for x in l) for l in lay if float(l[0]) > 300]
    cpl = [tuple(float(x) for x in l) for l in lay if float(l[0]) < 300]
    c(sysl and abs(sysl[0][0] - 563 * 0.625) < 0.2 and abs(sysl[0][1] - 267 * 0.625) < 0.2 and abs(sysl[0][3] - 309 * 0.625) < 0.2
      and abs(sysl[0][4] - 28 * 0.625) < 0.01, 'SYSTEMS STATUS at the DOS place', str(sysl[:1]))
    c(cpl and abs(cpl[0][0] - 225 * 0.625) < 0.2 and abs(cpl[0][1] - 241 * 0.625) < 0.2 and abs(cpl[0][3] - 276 * 0.625) < 0.2
      and abs(cpl[0][4] - 23 * 0.625) < 0.01, 'COMMAND COMPUTER at the DOS place', str(cpl[:1]))


@sequence
def target_viewer_camera(c):
    """the target viewer's model (DOS 0x30220 -> 0x28810(7, 0x20000, camera, object); 3Dfx 0x10021300 -> 0x1001bc50): a
    perspective view from 3 x the target's radius back along the player's bearing, level at the target's point, zoom 2
    (focal = the window's width), in colour 7, hidden edges removed (each polygon filled black, back to front) - not fitted
    to the box: DOSBox YELLSCN1 (e, the Nova 1.11 km off) shows the Nova filling the box, its legs cut off by the bottom
    edge (docs/reference/target_viewer_dos_vs_port.png)"""
    blue = lambda r, g, b: (b > 150) & (r < 60) & (g < 90)
    box = (0.035, 0.73, 0.19, 0.9)
    r = c.run(secs=UP + 1.5, shots=[(UP + 1.0, 'tv')], size='1024x768', dos_look=True, env={'MW2_TEST_TARGET': '0'})   # the Nova
    n = count_px(r.shots['tv'], blue, box)
    c(n > 400, 'the wireframe drawn', '%d px' % n)
    bot = count_px(r.shots['tv'], lambda R, G, B: ((B > 150) & (R < 60) & (G < 90)) | ((R > 150) & (G > 120) & (B < 60)), (0.045, 0.885, 0.185, 0.893))
    c(bot > 5, 'the model reaches the bottom edge (cut off, as DOS)', '%d px on the bottom rows' % bot)


@sequence
def inspect_target(c):
    """INSPECT_TARGET (i; engine 0x1001d510, DOS 0x2a7xx): inspected when the horizontal distance is under the unit's
    radius (+0xe8 = MGEO int[6], the Nova 600) + 200 m; the viewer's text line (0x10020e40) then shows what the target
    holds - a mech its GPS 4th string (unit +0xfe: the pilot) - for 0x16a ticks; again (inspected before, 3) the same;
    beyond: "Out of range" and "Target is beyond inspection radius" (0x10020eac -> 0x10031870(0x14, -1)); a success posts
    no message (no caller of message 19) """
    env = {'MW2_TEST_TARGET': '0', 'MW2_TEST_PASSIVE': '1', 'MW2_TEST_IMMOBILE': '0.1:0'}
    r = c.run(secs=UP + 2, keys=[(UP, 'i'), (UP + 1, 'i')], env=dict(env, MW2_TEST_AIM='0,20500,0'))
    ins = re.findall(r'INSPECT r=(\d) text=(.*)', r.out)
    c(len(ins) == 2 and ins[0] == ('1', 'St.Cpt.Haines') and ins[1][0] == '3', '205 m from the Nova: inspected, the pilot shown; again: 3', str(ins))
    c(r.msg_time('Inspection successful') is None, 'no message (the table\'s entry 19 is never posted: the viewer line only)', str(r.msgs[-3:]))
    r = c.run('_far', secs=UP + 1, keys=[(UP, 'i')], env=dict(env, MW2_TEST_AIM='0,20750,0'))
    ins = re.findall(r'INSPECT r=(\d) text=(.*)', r.out)
    c(ins == [('2', 'Out of range')], '207.5 m: out of range', str(ins))
    c(r.msg_time('beyond inspection radius') is not None, 'the message', str(r.msgs[-3:]))


@sequence
def key_ramps(c):
    """the key ramp (0x10039ee0 / DOS 0x486e0) with the control table's shifts (0x10256540 +0x18: legs_pan_delta 7,
    pilot_pan 5, zoom_factor 6), per frame at 30 fps: legs (a _delta control: value = 2 x min(value + step, 0.5) a frame)
    reach full turn on the 4th frame - 6-7 frames held turn 11-14 degrees, 11-12 frames 26-29 (DOSBox taps of 0.2 / 0.35 s:
    7-9 / 21 - about 1.5 frames less held); pilot pan 0.15 s a few degrees, 0.6 s about 70 (DOS: a few / well round); a Z
    tap of 3-4 frames zooms 1.00 -> 1.16-1.27 (DOSBox 0.12 s: 1.17)"""
    hold = [(UP, UP + 0.2, 'left'), (UP + 2, UP + 2.35, 'left'), (UP + 4, UP + 4.15, 'ctrl+right'), (UP + 5, UP + 5.6, 'ctrl+left'),
            (UP + 7, UP + 7.12, 'z')]
    r = c.run(secs=UP + 8, throttle=0, hold=hold, env={'MW2_RAMP_TRACE': '1'}, dump='/0.05')
    ramp = [dict((k, float(v)) for k, v in (kv.split('=') for kv in l.split()[1:])) for l in r.out.splitlines() if l.startswith('RAMP ')]
    def at(t, key):
        for d in ramp:
            if d['t'] >= t:
                return d[key]
        return ramp[-1][key] if ramp else 0
    def hd(t):
        return r.at(t)['heading']
    t1 = abs(hd(UP + 1) - hd(UP - 0.2))
    t2 = abs(hd(UP + 3) - hd(UP + 1.8))
    # frames held: 0.2 s = 6-7, 0.35 s = 11-12 (the hold's both ends): 0.071 + 0.213 + 0.497 + 1 + ... of 90 deg/s / 30
    c(9 < t1 < 15, '0.2 s legs tap: 11-14 degrees (DOSBox 7-9, its key timing shorter)', '%.1f' % t1)
    c(22 < t2 < 30, '0.35 s legs tap: 26-29 degrees (DOSBox 21)', '%.1f' % t2)
    p1, p2 = at(UP + 4.5, 'pan'), at(UP + 5.9, 'pan')
    c(0.5 < p1 < 10, 'pilot pan 0.15 s: a few degrees', '%.1f' % p1)
    c(p2 < -50, 'pilot pan 0.6 s back: well round the other way', '%.1f' % p2)
    z = at(UP + 7.5, 'zoom')
    c(1.10 < z < 1.30, 'Z 0.12 s (3-4 frames): zoom 1.16-1.27 (DOSBox 1.17)', '%.3f' % z)
@sequence
def overheat_alarm_and_viewzoom(c):
    """agent U: MECOVHW1 0xe9 - the 3Dfx (0x10015f50) sounds it with the shutdown sequence only (message 3), DOS
    0x00024f90 also with the 66 'Heat level critical' warning (DOSBox YELLSCN1: two alarms 0.7-1.2 s apart, the shutdown
    6 s after the second) - so twice in the DOS look, once in the 3D looks; VIEWZOOM 0x147 on any change of the camera's
    zoom (0x1002df60): the cockpit zoom factor and the external camera's 1.0 (mode 1, 0x1002e280)"""
    def sounds(r, i):
        return [float(m.group(1)) for m in re.finditer(r'SFXLOG (-?[\d.]+) snd %d ' % i, r.out)]
    for dos in (False, True):
        r = c.run('_dos' if dos else '_3d', secs=16, keys=[(8.5, 'backslash')], hold=heat_pulses(9, 13), dos_look=dos,
                  env={'MW2_SFX_LOG': '1'})
        al = sounds(r, 0xe9)
        pend = next((s2['t'] for s2 in r.states if s2.get('pend')), None)
        c(pend is not None, ('DOS' if dos else '3D') + ': shutdown sequence started')
        c(len(al) == (2 if dos else 1), ('DOS' if dos else '3D') + ': MECOVHW1 %s' % ('at the warning and the sequence' if dos else 'with the sequence only'), str(al))
        if al and pend is not None:
            c(abs(al[-1] - pend) < 0.3, ('DOS' if dos else '3D') + ': the last alarm as the sequence starts', '%s vs %.2f' % (al, pend))
    r = c.run('_zoom', secs=12, nostart=True, hold=[(3.0, 3.4, 'z'), (6, 6.05, 'kpmultiply')], keys=[(4.5, 'ctrl+z'), (6.3, 'f10')],
              env={'MW2_SFX_LOG': '1', 'MW2_GROUPS': '1:0,1,2,3,4,5,6,7;3:8,9'})
    vz = sounds(r, 0x147)
    c([t for t in vz if 3.0 <= t < 3.5], 'VIEWZOOM while the zoom key changes the cockpit zoom', str(vz))
    c([t for t in vz if 4.5 <= t < 4.6], 'VIEWZOOM on COCKPIT_RESET_ZOOM', str(vz))
    c(r.at(6.5)['camode'] == 3 and [t for t in vz if 6.3 <= t < 6.45], 'VIEWZOOM entering the ordnance camera (zoom 2.0)', str(vz))
    back = next((s2['t'] for s2 in r.states if s2['t'] > 6.5 and s2['camode'] == 0), None)
    c(back is not None and [t for t in vz if back - 0.3 <= t <= back + 0.05], 'VIEWZOOM back in the cockpit (1.0)', '%s back %s' % (vz, back))


@sequence
def gait_sounds(c):
    """agent U: the player's gait sounds by the engine's rule (0x1000b4e0, table 0x1024b2b8 indexed by the sequence
    playing +0x84 and the one asked for +0x88): full throttle from standing STOP2WLK 302 at once and WALK2RUN 328 as the
    run sequence starts; stop: RUN2WLK 282, then WLK2STOP 335 when the stride has gone back to the walk; reverse from
    standing: STOP2REV 301 as the reverse sequence starts (nothing on the request); stop: REV2STOP 281"""
    r = c.run(secs=31, keys=[(9, '0'), (15, '1'), (20, '5'), (20.034, 'backspace'), (26, '1')], env={'MW2_SFX_LOG': '1', 'MW2_STEP_TRACE': '1'})
    snd = [(float(m.group(1)), int(m.group(2))) for m in re.finditer(r'SFXLOG (-?[\d.]+) snd (\d+)', r.out) if int(m.group(2)) in (281, 282, 301, 302, 328, 335)]
    fw = [x for x in snd if x[0] < 19]
    c([i for t, i in fw] == [302, 328, 282, 335], 'forward: STOP2WLK, WALK2RUN, RUN2WLK, WLK2STOP', str(fw))
    if len(fw) == 4:
        c(9.0 <= fw[0][0] < 9.2, 'STOP2WLK on the throttle', str(fw[0]))
        c(15.0 <= fw[2][0] < 15.6, 'RUN2WLK on the stop', str(fw[2]))
    # 5 then BSP a frame later: the first frame asks for the walk (STOP2WLK), then the reverse (cur 0 -> WLK2STOP)
    rv = [x for x in snd if x[0] >= 19]
    sr = [t for t, i in rv if i == 301]
    c(len(sr) == 1 and 20.0 < sr[0] < 21.5, 'STOP2REV once, as the reverse sequence starts', str(rv))
    c(rv and rv[-1][1] == 281 and 26.0 <= rv[-1][0] < 26.8, 'REV2STOP on the stop', str(rv))


@sequence
def star_in_mw2prm(c):
    """agent U: the star configuration lives where the original keeps it - MW2PRM.CFG record 0x840dc at +0x10 (formation,
    slot, maximum, size, tonnage, 3 x {mech, loadout[16], pilot[16]}; FUN_0003a810 / FUN_0003a850, written at launch,
    read at every shell start). The DOS-written file's star (Mad Dog, MIKE, Friend 1 / 2) loads; a change is written
    back in place, the rest of the file untouched"""
    mech2 = os.path.join(os.path.dirname(GAME.rstrip('/')), 'MECH2')
    d = os.path.join(OUT, 'star_in_mw2prm')
    os.makedirs(d, exist_ok=True)
    src = os.path.join(mech2, 'MW2PRM.CFG')
    if not os.path.exists(src):
        c.note('no MW2PRM.CFG in %s: skipped' % mech2)
        return
    shutil.copy(src, os.path.join(d, 'MW2PRM.CFG'))
    e = dict(os.environ, MW2_STAR_PRM=d, SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy', MW2_CONFIG=os.path.join(d, 'x.cfg'))
    out = subprocess.run([os.path.join(os.path.dirname(GLVIEW), 'mw2'), mech2], env=e, capture_output=True, text=True, timeout=120).stdout
    lines = [l for l in out.splitlines() if l.startswith('star ')]
    old, new = open(src, 'rb').read(), open(os.path.join(d, 'MW2PRM.CFG'), 'rb').read()
    c(len(lines) == 2, 'loaded, saved, reloaded', out[-300:])
    if len(lines) == 2:
        f0 = int(lines[0].split()[2])
        c('MIKE' in lines[0] and 'Friend 1' in lines[0], 'the DOS-written star (pilot names) loads', lines[0])
        c(int(lines[1].split()[2]) == (f0 + 1) % 6, 'the changed formation comes back', lines[1])
    diff = [i for i in range(min(len(old), len(new))) if old[i] != new[i]]
    c(len(new) == 536 and diff == [0x10], 'only the formation word changed (536 bytes)', '%d bytes, diff at %s' % (len(new), diff[:8]))


@sequence
def archive_layout(c):
    """agent U: the Archive Holoprojector as DOSBox shows it (docs/reference/dos_archive_*.png): links go to the 1-based
    page (Clan History -> 'The Clans' = page 1), shell font 31 from (88, 70), 12 px lines, 19 a page, 460 px wrap, 36 px
    tabs, grey 5 / white links, the title in font 29 at y 50, no box over the picture; NEXT PAGE's label under the
    pointer. Compared pixel by pixel outside the DOS mouse pointer"""
    from PIL import Image
    import numpy as np
    shell = os.path.join(os.path.dirname(GLVIEW), 'mw2shell')
    mech2 = os.path.join(os.path.dirname(GAME.rstrip('/')), 'MECH2')
    d = os.path.join(OUT, 'archive_layout')
    os.makedirs(d, exist_ok=True)
    ref = os.path.join(REPO, 'docs', 'reference')
    for page, refn, mouse, cur in (('0,0', 'dos_archive_0.png', None, (390, 335, 430, 375)), ('2,0', 'dos_archive_2.png', None, (140, 105, 180, 140)),
                                   ('2,1', 'dos_archive_2_next.png', '517,388', (515, 378, 550, 410))):
        out = os.path.join(d, 'p%s.ppm' % page.replace(',', '_'))
        e = dict(os.environ, MW2_ARCH_PAGE=page, MW2_SHELL_SCREEN='5', MW2_SHELL_CLAN='0', MW2_SHELL_FRAMES='2', MW2_SHELL_SHOT=out,
                 SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy', MW2_CONFIG=os.path.join(d, 'x.cfg'), MW2_BITMAP_TEXT='1')
        if mouse:
            e['MW2_SHELL_MOUSE'] = mouse
        subprocess.run([shell, mech2], env=e, capture_output=True, timeout=120)
        if not os.path.exists(out):
            c(False, 'page %s rendered' % page)
            continue
        a = np.array(Image.open(out).convert('RGB')).astype(int)
        b = np.array(Image.open(os.path.join(ref, refn)).convert('RGB')).astype(int)
        diff = np.abs(a - b).max(2) > 6
        x0, y0, x1, y1 = cur
        diff[y0:y1, x0:x1] = False   # the DOS mouse pointer
        c(int(diff[0:440].sum()) == 0, 'page %s matches DOSBox (%s)' % (page, refn), '%d px differ' % int(diff[0:440].sum()))


def voice_plays(r):
    """[(sim ms, sound id)] of the voice-channel plays (MW2_SFX_LOG; with MW2_SFX_SIMCLOCK a line holds the voice channel
    for its length in sim time - the headless run is faster than real time)"""
    return [(float(m.group(1)) * 1000.0, int(m.group(2))) for m in re.finditer(r'SFXLOG (-?[\d.]+) snd (\d+) vol [\d.]+ voice', r.out)]


@sequence
def kill_messages(c):
    """user report: "Enemy mech destroyed" not said reliably - the port said it only when the player's own shot logged the
    kill. The engine (0x10016280, run once for every unit's death, any cause, any killer) posts 0x10031870 by the dead
    unit's class when its group is of the enemy alliance (0x10019040 == 1): mech 0x15 "Enemy mech destroyed" (voice 70),
    vehicle 0x16 (71), turret 0x19 (73), aircraft 0x17 (72). The voice queue (0x10031150) drops a new entry whose first
    voice matches one already QUEUED (not the one playing) when the voice's limit (table 0x1024e6a8; 70 not listed: -1)
    allows none. DOSBox YELLSCN1: "Enemy mech destroyed." with the player not firing"""
    # a death by damage the player didn't deal (MW2_TEST_AHIT: hits with no shooter), after the 16 s opener
    r = c.run(secs=24, env={'MW2_TEST_AHIT': '18:0:3:9:400', 'MW2_SFX_LOG': '1', 'MW2_SFX_SIMCLOCK': '1'})
    m = re.search(r'MSGV now=(\d+) voice=70 Enemy mech destroyed$', r.out, re.M)
    c(m is not None and 18000 <= int(m.group(1)) <= 18400, 'a kill not by the player is announced', m.group(0) if m else 'none')
    c(any(i == 70 and t >= 18000 for t, i in voice_plays(r)), 'voice 70 said', str(voice_plays(r)))
    # four at once: the first plays, the second waits, the other two duplicate the waiting one and are dropped
    r = c.run('_four', secs=26, env={'MW2_TEST_KILL_ALL': '18:0', 'MW2_SFX_LOG': '1', 'MW2_SFX_SIMCLOCK': '1'})
    said = [t for t, i in voice_plays(r) if i == 70]
    c(len(re.findall(r'MSGV now=\d+ voice=70 ', r.out)) == 4, 'four deaths, four messages posted')
    c(len(said) == 2 and said[1] > said[0] + 500, 'said twice, one after the other', str(said))
    c(len(re.findall(r'VQDROP now=\d+ voice=70', r.out)) == 2, 'two dropped as duplicates of the queued one')
    # by class: GOLDSCN1's turrets (class 3) are "Enemy target destroyed" (0x19, voice 73), its mechs voice 70
    r = c.run('_gold', secs=12, mission='GOLDSCN1', env={'MW2_TEST_KILL_ALL': '10:0'})
    c(re.search(r'MSGV now=\d+ voice=73 Enemy target destroyed', r.out) is not None and
      re.search(r'MSGV now=\d+ voice=70 Enemy mech destroyed', r.out) is not None, 'turrets: "Enemy target destroyed"; mechs: "Enemy mech destroyed"')


@sequence
def chain_fire_message(c):
    """user report: \\ said "chain fire" instead of "chain fire engaged". Engine 0x100437d4: 0x10031870(7 / 6, 1) -
    "Group fire" (voice 19) / "Chain fire" (18) and suffix 1 "engaged" (80) both ways, the two voices said in turn.
    DOSBox YELLSCN1 (no sound, the text): "Group fire engaged." at the first press, "Chain fire engaged." at the second.
    A message's suffix stays with it: autopilot then group fire at once says 65 80 19 80 (the port's single suffix slot
    had said 65 19 80)"""
    r = c.run(secs=UP + 14, keys=[(UP, 'backslash'), (UP + 3, 'backslash')], env={'MW2_SFX_LOG': '1', 'MW2_SFX_SIMCLOCK': '1'})
    c(re.search(r'MSGV now=\d+ voice=19 Group fire engaged\. \+80', r.out) is not None, '"Group fire engaged." (19 + 80)')
    c(re.search(r'MSGV now=\d+ voice=18 Chain fire engaged\. \+80', r.out) is not None, '"Chain fire engaged." (18 + 80)')
    v = [i for t, i in voice_plays(r) if t > UP * 1000 - 100]
    c(v == [19, 80, 18, 80], 'voices 19 80, 18 80', str(v))
    r = c.run('_queue', secs=UP + 14, keys=[(UP, 'a'), (UP + 0.1, 'backslash')], env={'MW2_SFX_LOG': '1', 'MW2_SFX_SIMCLOCK': '1'})
    v = [i for t, i in voice_plays(r) if t > UP * 1000 - 100]
    c(v[:4] == [65, 80, 19, 80], 'each suffix after its own line', str(v))


@sequence
def composed_messages(c):
    """audit of the engine's composed messages (message id + second part): MASC off posts nothing (0x10031870 with
    suffix 0 returns) and the MASC sounds are 0xcb on / 0xca off (MECBMAON / MECBMAOF, 0x1001aef0); an ammo critical says
    "Critical hit :" (54) + "internal ammo explosion detected" (48) with the alarm 0xd0 (0x100167b0 -> 0x100318a0(1));
    a starmate lost says "Point n" + "destroyed" (0x10031810(6, point)); the autopilot running out of nav points stops
    silently (0x10015d5c); the messages come from the simulation, the HUD off (F11) or not"""
    r = c.run(secs=12, env={'MW2_TEST_RNG': '9.9:12347', 'MW2_TEST_PHIT': '9:5:10:72,10:5:10:60'})
    c(re.search(r'MSGV now=\d+ voice=54 Critical hit :  internal ammo explosion detected\. \+48', r.out) is not None,
      'ammo critical: "Critical hit :" + "internal ammo explosion detected" (54 + 48)')
    # the sim's messages don't wait for the HUD: with it off (F11) the heat sequence is announced as it starts
    r = c.run('_hudoff', secs=UP + 3, keys=[(UP - 1, 'f11')], env={'MW2_TEST_HEAT': '%g:85' % UP})
    t_seq = r.msg_time('Shutdown sequence initiated')
    c(r.at(UP + 0.5).get('hud_off') == 1 and t_seq is not None and UP * 1000 <= t_seq <= UP * 1000 + 300,
      'HUD off: "Shutdown sequence initiated" at once', str(t_seq))
    r = c.run('_masc', secs=UP + 3, keys=[(UP, 'v'), (UP + 1.5, 'v')], mech='firemoth/frm00std', env={'MW2_SFX_LOG': '1', 'MW2_SFX_SIMCLOCK': '1'})
    snd = [(float(t) * 1000.0, int(i)) for t, i in re.findall(r'SFXLOG (-?[\d.]+) snd (\d+)', r.out)]
    if 'Not equipped with MASC' in r.out:
        c.note('test mech has no MASC: MASC part skipped')
    else:
        c(re.search(r'MSGV now=\d+ voice=61 MASC engaged\. \+80', r.out) is not None and 'MASC disengaged' not in r.out,
          '"MASC engaged." on, nothing off')
        c([i for t, i in snd if i in (0xca, 0xcb)] == [0xcb, 0xca], 'MECBMAON then MECBMAOF', str(snd))


def missile_paths(r):
    """MW2_MISSILE_TRACE lines: {id: [(t, x, y, z, homing)]}, and the target's [(t, x, z)]"""
    paths, tgt = {}, []
    for l in r.out.splitlines():
        f = l.split()
        if l.startswith('missile ') and len(f) >= 16:
            paths.setdefault(int(f[3]), []).append((float(f[1]), float(f[7]), float(f[8]), float(f[9]), int(f[15])))
        elif l.startswith('mtarget ') and len(f) >= 7:
            tgt.append((float(f[1]), int(f[4]), int(f[6])))
    return paths, tgt


@sequence
def missiles_track_strafing_target(c):
    """guided missiles home on a target moving sideways (0x10045910: acceleration unit(target - p) - unit(v), no gravity
    while guided): a Nova 300 m ahead slides east at 5 m/s (test hook MW2_TEST_STRAFE); an LRM 20 volley fired locked
    curves after it (every missile ends east of its launch, near the target) and damages it; the next weapon selected
    mid-volley drops the lock, but the volley's later missiles still home (the target latched in the weapon slot at the
    trigger, 0x100437a0 slot +0x18 / +0x1c). The same volley fired before the lock (0x80) flies straight and misses."""
    env = {'MW2_KILL_ACTOR': '0,9999', 'MW2_TEST_TARGET': '0', 'MW2_TEST_STANDOFF': 30000, 'MW2_TEST_HOLD_ACTOR': '0',
           'MW2_TEST_STRAFE': '0,500,9,30', 'MW2_MISSILE_TRACE': '1'}
    r = c.run(secs=15, env=env, hold=LRM_SELECT + [(11, 11.1, 'space'), (11.5, 11.6, 'enter')])
    c(r.at(10.9)['lock'] & 0x80, 'locked when fired', 'lock 0x%x' % r.at(10.9)['lock'])
    c(r.at(11.9)['lock'] & 0x80 == 0, 'lock dropped by the next weapon', 'lock 0x%x' % r.at(11.9)['lock'])
    paths, tgt = missile_paths(r)
    c(len(paths) >= 18, 'an LRM 20 volley in flight', '%d missiles' % len(paths))
    late = [k for k, p in paths.items() if p[0][0] > 11.7]
    c(len(late) >= 5, 'missiles leaving after the lock dropped', str(len(late)))
    c(all(p[0][4] == 0 for p in paths.values()), 'every missile of the volley homes on the latched target',
      str(sorted(set(p[0][4] for p in paths.values()))))
    for k, p in sorted(paths.items()):
        t_end, x_end, z_end = p[-1][0], p[-1][1], p[-1][3]
        tx = next((x for t, x, z in tgt if t >= t_end), tgt[-1][1] if tgt else 0)
        if not c(x_end - p[0][1] > 600, 'missile %d curves east after the target' % k, 'launch x %.0f end x %.0f' % (p[0][1], x_end)):
            break
        if not c(abs(x_end - tx) < 500, 'missile %d ends at the target' % k, 'end x %.0f target x %d' % (x_end, tx)):
            break
    h0, h1 = r.at(10.9)['tgt_health'], r.last['tgt_health']
    c(h1 < h0 - 20, 'the volley damages the strafing target', 'health %d -> %d' % (h0, h1))
    cyc = [(8 + 0.4 * i, 8.1 + 0.4 * i, 'enter') for i in range(7)]   # LRM selected at 10.4 s: no lock yet at 11
    r = c.run('_unlocked', secs=15, env=env, hold=cyc + [(11, 11.1, 'space')])
    c(r.at(10.9)['lock'] & 0x80 == 0, 'not yet locked when fired', 'lock 0x%x' % r.at(10.9)['lock'])
    paths, tgt = missile_paths(r)
    c(len(paths) >= 18, 'unlocked: a volley in flight', '%d missiles' % len(paths))
    c(all(q[4] == -2 for p in paths.values() for q in p), 'unlocked: no homing')
    c(all(abs(p[-1][1] - p[0][1]) < 400 for p in paths.values()), 'unlocked: straight on (no turn after the target)',
      str([round(p[-1][1] - p[0][1]) for p in paths.values()][:5]))
    c(r.last['tgt_health'] == r.at(10.9)['tgt_health'], 'unlocked: the volley misses the moving target',
      '%d -> %d' % (r.at(10.9)['tgt_health'], r.last['tgt_health']))


@sequence
def dos_weapon_fire_visible(c):
    """the DOS renderer draws the missiles (the pool MW2_SHT1's MISSLE1 by BTHG kind - the DOS archive has no MISSILE1 /
    MISSILE2 records) with their yellow exhaust disc, and in image enhancement the projectiles (type word 0x400) in red
    (colour 0xb): a Timber Wolf fires everything (group 1, NumLock) at a Nova 120 m ahead (DOSBox YELLSCN1 reference:
    docs/reference/dos_weapon_fire_vision_modes_dosbox_vs_port.png)"""
    env = {'MW2_KILL_ACTOR': '0,9999', 'MW2_TEST_TARGET': '0', 'MW2_TEST_STANDOFF': 30000, 'MW2_TEST_HOLD_ACTOR': '0',
           'MW2_GROUPS': '1:0,1,2,3,4,5,6,7,8,9'}
    box = (0.15, 0.1, 0.85, 0.7)
    glow = lambda r_, g, b: (r_ > 200) & (g > 150) & (b < 120)
    red = lambda r_, g, b: (r_ > 200) & (g < 60) & (b < 60)
    r = c.run(secs=12.5, env=env, hold=[(11, 11.1, 'numlock')], shots=[(10.8, 'before'), (11.3, 'after')], dos_look=True)
    g0, g1 = count_px(r.shots['before'], glow, box), count_px(r.shots['after'], glow, box)
    c(g1 > g0 + 100, 'DOS: the LRMs drawn (exhaust discs)', 'glow px %d -> %d' % (g0, g1))
    env['MW2_VISION'] = 2
    r = c.run('_ie', secs=12.5, env=env, hold=[(11, 11.1, 'numlock')], shots=[(10.8, 'before'), (11.3, 'after')], dos_look=True)
    r0, r1 = count_px(r.shots['before'], red, box), count_px(r.shots['after'], red, box)
    c(r1 > r0 + 100, 'DOS image enhancement: lasers and missiles red', 'red px %d -> %d' % (r0, r1))

@sequence
def titanium_sky_scrub(c):
    """TEXTURES TITANIUM (user: "titanium looks buggered", "skybox looks whack"): the Titanium build's scenes include no
    scrub patch (its SCN1 records leave out every xxxxSCRI), so none is drawn - BROWSCN1's Titanium BROWSCR1 cards were
    drawn as blue flat triangles on the ground, the 3D editions' cyan-keyed ORANWD weeds as cards; the sky dome with the
    engine's UV rule (mercsw 0x10001081: type > 0 U = u / w, V = (v - the polygon's least v) / h): TNW1 at the user's
    view near Nav Alpha (heading 310) shows the blue sky, the grey mountain ring above the horizon and the dark ground
    (TNW_GROU, mean 44 / 255) (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    def env(name, extra=None):
        e = {'MW2_CONFIG': titanium_cfg(name, tdir), 'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'), 'MW2_TEXTURES': 'titanium'}
        e.update(extra or {})
        return e
    shots = [(2.5, 'view')]
    r = c.run(secs=3, throttle=0, nostart=True, mission='BROWSCN1', shots=shots, env=env('titanium_sky_scrub'))
    m = re.search(r'titanium: \d+ of \d+ world objects with a Titanium model, (\d+) scrub-patch objects not drawn', r.out)
    c(m is not None and int(m.group(1)) > 0, 'BROW: its scrub patch not drawn with the Titanium textures', m.group(0) if m else 'no line')
    blue = count_px(r.shots.get('view'), lambda R, G, B: (B > R + 60) & (B > G + 60), (0, 0.55, 0.75, 0.85))
    c(0 <= blue < 10, 'BROW: no blue scrub cards on the ground', 'blue pixels %d' % blue)
    t = c.run('_tnw', secs=2, throttle=0, nostart=True, mission='TNW1SCN1', shots=[(1.5, 'view')],
              env=env('titanium_sky_scrub_tnw', {'MW2_START': '-10620,42573', 'MW2_START_HEADING': '310'}))
    v = t.shots.get('view')
    sky = count_px(v, lambda R, G, B: (B > R + 80) & (B > G + 80), (0.3, 0.2, 0.7, 0.4))
    grey = count_px(v, lambda R, G, B: (abs(R - G) < 12) & (abs(G - B) < 12) & (R > 90), (0, 0.15, 0.25, 0.48))
    dark = count_px(v, lambda R, G, B: (R < 90) & (G < 90) & (B < 90), (0.3, 0.55, 0.7, 0.8))
    c(sky > 2000, 'TNW1: the sky blue (sky=64,64,255 and the clouds)', 'blue pixels %d' % sky)
    c(grey > 1500, 'TNW1: the grey mountain ring (TNW_MNT*) above the horizon on the left', 'grey pixels %d' % grey)
    c(dark > 10000, 'TNW1: the dark Titanium ground (TNW_GROU)', 'dark pixels %d' % dark)
    c.note('%s; BROW blue %d; TNW1 sky %d, mountains %d, ground %d' % (m.group(0) if m else '?', blue, sky, grey, dark))



@sequence
def hidden_mechs_playable(c):
    """the shell's easter egg (FUN_000295c0: pilot "Enzo" 18 chassis, "Hobbes" 17, "Calvin" 16) is unlocked for good, in
    Instant Action and the campaigns' Mech Lab: Elemental, Tarantula and Battle Master IIC each load and walk in a mission"""
    for m in ('elementl/ele00std', 'tarantul/tar00std', 'bttlmstr/btm00std'):
        r = c.run('_' + m.split('/')[0], secs=14, throttle=1, mech=m)
        last = r.last
        c(last.get('pload', '').lower() == m.split('/')[1], '%s loads' % m, str(last.get('pload')))
        c(last.get('dead') == 0 and last.get('kmh', 0) > 5, '%s walks' % m, 'kmh %s' % last.get('kmh'))
    # the campaign Mech Lab offers them too (user request): the lab reaches all 18 chassis, with their designs
    shell = os.path.join(REPO, 'mw2shell')
    if os.path.exists(shell):
        os.makedirs(os.path.join(OUT, c.name), exist_ok=True)
        gd = install_dir(c.name + '_lab', None)
        cfg = os.path.join(OUT, c.name, 'lab.cfg')
        with open(cfg, 'w') as f:
            f.write('game=%s\n' % gd)
        for k, name in ((15, 'Elemental'), (16, 'Tarantula'), (17, 'Battle Master IIC')):
            e = {q: v for q, v in os.environ.items() if not q.startswith('MW2_')}
            e.update({'SDL_VIDEODRIVER': 'offscreen', 'SDL_AUDIODRIVER': 'dummy', 'MW2_CONFIG': cfg, 'MW2_SAVE_DIR': gd,
                      'MW2_SHELL_SCREEN': '9', 'MW2_SHELL_CLAN': '0', 'MW2_SHELL_FRAMES': '2', 'MW2_SHELL_LAB_CHASSIS': str(k),
                      'MW2_SHELL_SHOT': os.path.join(OUT, c.name, 'lab%d.ppm' % k)})
            p = subprocess.run([shell], env=e, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 * TSCALE)
            out = p.stdout.decode('latin-1')
            m = re.search(r'lab chassis %d (.+), (\d+) variants' % k, out)
            c(m is not None and m.group(1) == name and int(m.group(2)) > 0, 'Wolf campaign lab: %s with its designs' % name, m.group(0) if m else out[-200:])

@sequence
def autopilot_contact_deflect(c):
    """user decision (2026-10-20): the player's autopilot as the original at obstacles. DOS 0x24ca0 -> 0x2db80 does avoid,
    but its probe's broad phase (0x3a745) drops an object as an "overflow" (the carry of a signed 64-bit add) unless its
    centre lies ahead of the probe in x AND z, so it sees most things only once the probe's start is inside their sphere.
    DOSBox TNJ4 (docs/reference/autopilot_contact_deflect_dos.jpg; a 100 m probe called from the kph readout: 0 all the way
    to S_1BQDAS): straight at full speed until ~216 m from Nav Beta, 12 / 31 kph for ~2 s, turned away to the LEFT, 120
    then 147 kph along the face and on to the nav. TNJ4 from NAV1 toward NAV2: no deflection from range, the quarter
    throttle (0x100) only within the box's sphere (< 35 m from the south face z 166195), slowed for 0.5-4 s, turns left,
    never through the face, the pilot's throttle back (+0x17a) and NAV2 reached, autopilot still on. From the start to
    NAV1 (open desert, the mountains): no deflection (terrain never deflects)"""
    r = c.run(secs=40, keys=[(UP, '0'), (UP + 0.5, 'a')],
              env={'MW2_START': '-133566,141377', 'MW2_START_HEADING': -24.7, 'MW2_TEST_NAVSEEN': '1,3'},
              mission='TNJ4SCN1', dump='/0.1')
    face = 166195
    nav2 = (-153774, 185721)
    brg = lambda s: math.degrees(math.atan2(nav2[0] - s['x'], nav2[1] - s['z']))
    off = lambda s: (s['heading'] - brg(s) + 180) % 360 - 180
    seg = r.span(UP + 1.5, 39.9)
    first = next((s for s in seg if abs(s['throttle'] - 0.25) < 0.01 and s['z'] > face - 3500), None)
    c(first is not None, 'deflects at the box, inside its sphere (quarter throttle 0x100 within 35 m of the face)')
    if first is None:
        return
    t0 = first['t']
    pre = r.span(UP + 1.5, t0 - 0.05)
    blip = [s for s in pre if abs(s['throttle'] - 0.25) < 0.01]
    # from range the box (sphere centre x -140633) passes the DOS broad phase only while its centre is ahead in x as well
    # as z: the moment between the face coming within the probe's ~108 m and the mech passing x -140633 (2 checks)
    c(pre and len(blip) <= 3 and max(abs(off(s)) for s in pre) < 5 and pre[-1]['throttle'] > 0.9 and pre[-1]['kmh'] > 100,
      'straight at the nav and at full speed until then (no deflection from range)',
      '%d quarter-throttle samples before, off course %.1f deg' % (len(blip), max(abs(off(s)) for s in pre)))
    ep = r.span(t0, t0 + 6)
    c(min(s['heading'] for s in ep) < first['heading'] - 20, 'turns away to the left (as DOSBox)',
      'heading %.1f -> min %.1f' % (first['heading'], min(s['heading'] for s in ep)))
    slow = [s for s in ep if s['kmh'] < 60]
    c(slow and 0.5 <= slow[-1]['t'] - slow[0]['t'] <= 4.0, 'slowed for 0.5-4 s (DOSBox ~2 s at 12 / 31 kph)',
      '%.1f s' % (slow[-1]['t'] - slow[0]['t'] if slow else -1))
    c(max(s['z'] for s in r.span(t0, t0 + 4) if abs(s['x'] - first['x']) < 1500) < face, 'never through the face')
    back = next((s for s in r.span(t0, 39.9) if s['throttle'] > 0.9), None)
    c(back is not None and back['t'] - t0 < 5, "the pilot's throttle back within 5 s (+0x17a)", '%.1f s' % ((back['t'] - t0) if back else -1))
    arrive = next((s for s in r.span(t0, 39.9) if s['nav'] != 2), None)
    c(arrive is not None and arrive['t'] - t0 < 15 and all(s['autopilot'] == 1 for s in r.span(t0, arrive['t'])),
      'resumes and reaches NAV2 within 15 s, still engaged', '%.1f s' % ((arrive['t'] - t0) if arrive else -1))
    r = c.run('_desert', secs=78, keys=[(UP, '0'), (UP + 0.5, 'a')], mission='TNJ4SCN1', dump='/0.5')
    seg = r.span(14, 77)
    c(seg and min(s['throttle'] for s in seg) > 0.9, 'start to NAV1 and on: no deflection over open ground / mountains',
      str(sorted(set(round(s['throttle'], 2) for s in seg))))


@sequence
def airborne_move_3d(c):
    """an airborne unit's move is one 3D segment (engine 0x10019310 -> 0x1000b5e0: dx, (v + a t / 2) t, dz from the
    centre, extended along the move by (1 - |u.y|) R + |u.y| H - straight up: H): a Summoner (centre H 10.6 m) jetting
    straight up under AQUA's grounded dropship wing V_BDRPWS (a type 0 box from 16.85 m up) strikes its underside at
    once - the segment reaches 2 H above the feet - and is set back to the touching point (hit - H along the move: down,
    under the ground, so stood on it again), the vertical velocity reflected (r x |v| x -0.25): it never climbs more than
    a few metres while the tank lasts. The port's level test let it rise through the wing (a box the centre starts
    inside refuses only the horizontal move; free, 10 s of jets climb far above 17 m)"""
    r = c.run(secs=UP + 12, mech='summoner/smn00std', mission='AQUASCN1', hold=[(UP, UP + 10, 'j')], dump='/0.1',
              env={'MW2_START': '-72480,-54014,0', 'MW2_START_HEADING': 0})
    s0 = r.at(UP - 0.3)
    c(abs(s0['y']) < 50, 'starts on the ground under the wing', 'y %d' % s0['y'])
    up = r.span(UP + 0.5, UP + 9.5)
    peak = max(s['y'] for s in up)
    c(peak < 600, 'never rises into the wing (bottom 1685)', 'peak feet %d' % peak)
    c(up and up[-1]['fuel'] < up[0]['fuel'] - 500, 'the jets burn meanwhile', 'fuel %.0f -> %.0f' % (up[0]['fuel'], up[-1]['fuel']))
    c(abs(r.last['y']) < 50, 'on the ground once the tank is empty', 'y %d' % r.last['y'])


@sequence
def collision_list_order(c):
    """the collision list's order (walking takes the LAST entry hit whose sphere is entered before the hit so far,
    0x10010530; probes and the ground break ties by it): every entry is put at the head (0x1000db80 / 0x1000dc60), so the
    list runs from the last object created - the loader takes a record's chunks in order and an INCL's record there and
    then (0x10042c60), an OBJ inside a BLK..ENDB block only after the load (0x10024fe0, table order) - and a destroyed
    variant goes to the head when it appears (0x10025180 -> 0x10025450). YELLSCN1: the head is the last block object
    created, YELLDRP1's dropship (V_BDRPWS); structure 5 (Coolant-Piping) destroyed at 11 s - its destroyed variant
    heads the list; its intact S_BCHEMB, a child of the standing Processor, is NOT created again (its entry carries
    0x200, which 0x10025310 skips - see pump_tank_destroyed_stays_gone; this test had expected it back)"""
    r = c.run(secs=13, env={'MW2_CLIST_TRACE': '1', 'MW2_TEST_BLD_KILL': '11:5'}, dump='/1')
    m = re.search(r'clist order YELLSCN1: (\d+) parts, (\d+) objects seen, (\d+) parts unplaced', r.out)
    c(m is not None and m.group(3) == '0', 'every world part placed in the load order', m.group(0) if m else 'no trace')
    heads = [(float(h.group(1)), h.group(2)) for h in re.finditer(r'clist head at ([\d.]+)s: (\S+)\(', r.out)]
    c(heads and heads[0][1] == 'V_BDRPWS', 'at the load: the last block object created first', str(heads[:1]))
    late = [l for l in re.findall(r'clist head at ([\d.]+)s: (.*)', r.out) if float(l[0]) >= 11.0]
    names = re.findall(r'(\S+)\(\S+ seq', late[0][1]) if late else []
    # YELLARE6's chemical plant: SA1CHEMB created at the swap (0x10025180 -> 0x10025450); S_BCHEMB stays gone
    c(names[:1] == ['SA1CHEMB'] and 'S_BCHEMB' not in names, 'after the kill: the destroyed variant first, the intact piping gone', str(names))

@sequence
def probe_unit_pieces(c):
    """the AI's avoidance probes (0x100201c0 -> 0x100103e0) walk the whole collision list, where a unit is its pieces -
    the LOD objects 0x10026430 makes, each collision type 6 (0x1000ffd0(6)): a sphere (0x10029db0 / 0x100107b0), centred
    on the posed piece's box middle with the farthest vertex as radius; the probe's own unit skipped by its id (+0x14).
    The port had tested a unit as a circle of r1 + r2, at any height. YELLSCN1, the standing Timber Wolf: a probe at its
    centre height from 40 m ahead strikes one of its pieces; one 12 m above its centre passes over it"""
    r = c.run(secs=11, env={'MW2_PROBE_TRACE': '1', 'MW2_TEST_PROBE': '10:4000:0:6000,10.5:4000:1200:6000'}, dump='/1')
    res = re.findall(r'^testprobe (\d) at (-?\d+),(-?\d+) \(dz 4000 dy (\d+)\)', r.out, re.M)
    pieces = re.findall(r'^probe [\d.]+s actor -2: unit -1 piece (\S+) r (\d+) at (\d+) of 6000', r.out, re.M)
    lv = [x for x in res if x[3] == '0']
    hi = [x for x in res if x[3] == '1200']
    c(lv and lv[0][0] == '2', 'at the centre height: a unit (its piece)', str(res))
    c(pieces and all(p[0].upper().startswith('TW') for p in pieces), "the Timber Wolf's pieces", str(pieces))
    c(pieces and 4000 - 1500 < int(pieces[-1][2]) < 4000, 'struck at the piece sphere, short of the centre', str(pieces[-1:]))
    c(hi and hi[0][0] == '0', '12 m above its centre: nothing (a circle would have caught it)', str(res))

@sequence
def wedged_between_boxes(c):
    """a unit wider than a gap stays put pushing across it (engine 0x10019310: the impact's second try 0x10019a61, refused
    by ANOTHER object than the first, leaves the unit where it stood - 0x10019a6a / 0x10019a76; DOS 0x26990 the same,
    iStack_4c != iStack_60): SILV's 13.7 m alley between S_BINDPA and S_BINDPD (DOS MW2.PRJ boxes), a Rifleman (R 7.5 m)
    at full throttle facing south does not move; facing east, along the alley, it walks off. DOSBox, SILVSCN1 with the
    start nav patched to the alley and a Rifleman USERSTAR: the same (no motion facing 180 for 12 s, out of the alley
    facing 90). The SILV actor 8 stall of session 15 was this"""
    for h, moves in ((180, False), (90, True)):
        r = c.run('_%d' % h, secs=UP + 12, mech='rifleman/rfl00std', mission='SILVSCN1', keys=[(UP, '0')], dump='/0.5',
                  dos_look=True, nostart=True, env={'MW2_START': '111200,-22892,0', 'MW2_START_HEADING': h})
        a, b2 = r.at(UP + 1), r.at(UP + 11.5)
        d = math.hypot(b2['x'] - a['x'], b2['z'] - a['z'])
        if moves:
            c(d > 5000, 'facing %d: walks along the alley' % h, '%.0f cm' % d)
        else:
            c(d < 100, 'facing %d: stays put' % h, '%.0f cm' % d)

@sequence
def throttle_servo_dos_shortfall(c):
    """the DOS edition's throttle servo is integer (MW2.EXE 0x49b80: step = frame ticks x diff / 36 truncated, tau chop(0.2
    x 182) = 36): it stalls once ticks x |diff| < 36, short of the preset by 5 / 1024 of full at 30 fps (the port normalises
    to 30 fps); the 3D editions keep the 3Dfx float servo (0x1003b7e0). DOSBox YELLSCN1 (Timber Wolf, start heading 158,
    keys 4 / 8 / 0 = presets 3 / 7 / 9 from STOP): MCGA at cycles 40000 (~30 fps) reads 28 / 69 / 90 kph, 1024x768 (~5 fps,
    frames 30+ ticks: 1 unit short) 30 / 70 / 90 - the DOS edition the first, the enhanced edition the second, at 30 and
    60 fps"""
    import collections
    for ed, fps, want in (('dos', 30, (28, 69, 90)), ('dos', 60, (28, 69, 90)), ('enhanced', 30, (30, 70, 90))):
        env = {'MW2_KPH_TRACE': '1'}
        if fps != 30:
            env['MW2_TEST_FPS'] = fps
        r = c.run('_%s%d' % (ed, fps), secs=30, keys=[(5, '4'), (14, '8'), (21, '0')], dump='/1', nostart=True,
                  edition=ed, env=env)
        got = []
        for a, b in ((7.5, 14), (16.5, 21), (23.5, 30)):
            v = collections.Counter(int(m.group(2)) for m in re.finditer(r'^kph ([\d.]+)s (-?\d+) kph', r.out, re.M)
                                    if a < float(m.group(1)) <= b)
            got.append(v.most_common(1)[0][0] if v else None)
        c(tuple(got) == want, '%s edition at %d fps: presets 3 / 7 / 9 read %s kph' % (ed, fps, ' / '.join(map(str, want))), str(got))
@sequence
def walk_run_carried_offset_by_anim(c):
    """the carried walk / run offset comes from the ANIM record each mech names (BWD ANIM chunk -> 0x100401c0): the
    Marauder (MARAANIM, walk 4 -> run 15) carries its hips 15 cm (track 1) while running; the Stormcrow runs on DIREANIM
    (shared with the Warhawk) and carries under 1 (the 28 degrees of STRMANIM, like DAISANIM's 112 cm, belong to records
    no mech uses). DOSBox (patched MADDANIM in a copy, docs/reference/walk_run_carried_offset_dos_vs_port.png): DOS
    MW2.EXE 0x1aad2 adds the key deltas the same way"""
    r = c.run('_mrd', secs=14, mech='marauder/mrd00std', keys=[(0.2, '0')], nostart=True, dump='/0.25')
    run = [s for s in r.span(9.0, 14.0) if s.get('pseq') == 1]
    c(len(run) >= 10, 'Marauder running (sequence 1)', str([s.get('pseq') for s in r.span(9.0, 14.0)][:5]))
    c(run and all(abs(s['poff'] - 15.0) < 0.01 and s['poff_tr'] == 1 for s in run), 'Marauder: the hips carry 15 cm',
      str(sorted(set((s['poff'], s['poff_tr']) for s in run))))
    r2 = c.run('_stm', secs=14, mech='strmcrow/stm00std', keys=[(0.2, '0')], nostart=True, dump='/0.25')
    run2 = [s for s in r2.span(9.0, 14.0) if s.get('pseq') == 1]
    c(len(run2) >= 10, 'Stormcrow running (sequence 1)', str([s.get('pseq') for s in r2.span(9.0, 14.0)][:5]))
    c(run2 and all(s['poff'] < 1.0 for s in run2), 'Stormcrow (DIREANIM): under 1 carried', str(sorted(set(s['poff'] for s in run2))))
@sequence
def missile_lock_structure(c):
    """the missile lock on a targeted structure (kind 0x400): 0x10044560 refuses only no target and nav points (+0xdc &
    0x1100), so a thing locks like a unit - the range window, 16 degrees, 0x16a ticks - at its object's position (0x1001d2d0
    -> 0x100257a0, the model's origin); a locked LRM volley homes on that point (0x10045910 kind 0x400) and damages it.
    YELLSCN1's middle coolant tank (structure 6) from 120 m north of it, targeted with q (0x1001d870), LRM selected as in
    missile_lock"""
    cyc = [(2 + 0.4 * i, 2.1 + 0.4 * i, 'enter') for i in range(7)]
    r = c.run(secs=19, env={'MW2_START': '316839,262000', 'MW2_START_HEADING': 180, 'MW2_MISSILE_TRACE': '1', 'MW2_BLD_TRACE': '1'},
              keys=[(UP - 0.3, 'q'), (UP, 'q')], hold=cyc + [(12.5, 12.6, 'space')])
    s = r.at(UP)
    c(s['w'][s['sel']][0] == 0, 'LRM selected', 'sel %d' % s['sel'])
    c(r.at(12.4)['thing'] == 6, 'q targets the tank (structure 6)', 'thing %d' % r.at(12.4)['thing'])
    on = next((x['t'] for x in r.states if x['lock'] & 0x8000), None)
    lk = next((x['t'] for x in r.states if x['lock'] & 0x80), None)
    c(on is not None, 'on target (0x8000) with a structure')
    c(lk is not None, 'locked (0x80) on a structure')
    if on is not None and lk is not None:
        c(1.5 < lk - on < 2.6, 'lock ~2 s after acquiring (0x16a ticks)', '%.2f s' % (lk - on))
    c(r.at(12.4)['lock'] & 0x80, 'locked when fired', 'lock 0x%x' % r.at(12.4)['lock'])
    hom = [l for l in r.out.splitlines() if l.startswith('missile ') and l.endswith('homing -1006')]
    c(len(hom) > 0, 'the volley homes on the structure (MSIM_TGT_THING(6))', '%d trace lines' % len(hom))
    h0, h1 = r.at(12.4)['thing_hp'], r.at(18.4)['thing_hp']
    hits = [l for l in r.out.splitlines() if l.startswith('bld hit') and l.endswith('by -1')]
    c(len(hits) > 0 and (h1 < h0 or h1 == -1 or r.at(18.4)['thing'] == -1), 'the locked volley damages it',
      'hp %d -> %d, %d hits' % (h0, h1, len(hits)))

    # homing, not the aim: the torso turned 12 degrees west of the tank (inside the 16-degree cone), the tank targeted by
    # the test hook - a locked volley curves onto tank 6 (x 3163..3181 m); one fired before the lock flies straight
    # along the aim into tank 4 to its west
    def hits_x(rr):
        return [float(l.split(' at ')[1].split(',')[0]) for l in rr.out.splitlines() if l.startswith('bld hit') and l.endswith('by -1')]
    # (the Falcon Firemoth, actor 3, held where it starts: walking round the tanks it crossed the volley's path and, its
    # pieces' spheres being the shots' test (0x100103e0), took 3 of the missiles - agent G2)
    env = {'MW2_START': '316839,267000', 'MW2_START_HEADING': 188, 'MW2_TEST_THING': '9.5:6', 'MW2_BLD_TRACE': '1', 'MW2_TEST_HOLD_ACTOR': '3'}
    r = c.run('_off', secs=19, env=env, hold=cyc + [(12.5, 12.6, 'space')])
    c(r.at(12.4)['lock'] & 0x80, 'off-axis: locked when fired', 'lock 0x%x' % r.at(12.4)['lock'])
    hx = hits_x(r)
    c(len(hx) >= 10 and all(316352 <= x <= 318146 for x in hx), 'off-axis: the locked volley homes onto tank 6',
      '%d hits, x %s' % (len(hx), sorted(set(int(x) for x in hx))[:6]))
    r = c.run('_unlocked', secs=16, env=env, hold=cyc + [(10.5, 10.6, 'space')])
    c(r.at(10.4)['lock'] & 0x80 == 0, 'off-axis: not locked when fired early', 'lock 0x%x' % r.at(10.4)['lock'])
    hx = hits_x(r)
    c(not any(316352 <= x <= 318146 for x in hx), 'off-axis: the unlocked volley flies straight past tank 6',
      'x %s' % sorted(set(int(x) for x in hx))[:6])


@sequence
def jets_terminal_from_rest_and_running(c):
    """forward jets (GreyHome) until the tank is empty: the engine keeps ONE velocity vector (+0xf4 / +0xfc, 3Dfx
    0x10019310, DOS 0x26990) and the directional branch's thrust and drag act on all of it, the walking part included -
    the terminal speed is L = 2.5 x the top walking speed whether the jets start from a standstill or from a run (the
    port had dragged only the jets' share and added the walk on top: Nova 559 kph from FULL, user report 530). DOSBox
    ORANSCN1 (MCGA, cycles 40000, ZMBV), forward jets held for 13 s: Nova (NVA00STD) from rest 160 / 277 / 342 / 376 /
    391 / 399 kph at 1 / 2 / 3 / 4 / 5 / 6 s, then 403 until the fuel runs out ~10 s in; from FULL (159) 277 / 342 at 1 /
    2 s, then 403; Summoner (SMN00STD, FULL 132) 336 from rest and from FULL. With the tank empty the mech, carried off
    the falling ground, drops back with its speed held (the readout climbs by the vertical speed) and eases to the
    walking speed on landing"""
    for suf, mech, full, lo, hi, top in (('_nova_rest', 'nova/nva00std', False, 400, 406, 0),
                                          ('_nova_full', 'nova/nva00std', True, 400, 406, 160),
                                          ('_smn_rest', 'summoner/smn00std', False, 333, 339, 0),
                                          ('_smn_full', 'summoner/smn00std', True, 333, 339, 133)):
        r = c.run(suf, secs=24, keys=[(1, '0')] if full else [], hold=[(8, 21, 'home')], dump='/1', nostart=True,
                  mission='ORANSCN1', mech=mech, env={'MW2_KPH_TRACE': '1'})
        k0, k = kph_at(r, 7.0, 7.9), kph_at(r, 8.0, 17.9)
        term = kph_at(r, 15.0, 17.9)
        c(k0 and min(k0) == max(k0) == top, suf + ': before the jets %d kph' % top, str(sorted(set(k0))))
        c(term and lo <= min(term) and max(term) <= hi, suf + ': terminal %d..%d kph (DOSBox 403 / 336)' % (lo, hi),
          str(sorted(set(term))))
        c(k and max(k) <= hi, suf + ': never above the terminal while the jets push (the port had 559 from FULL)', str(max(k or [0])))
        if mech.startswith('nova') and not full:
            dos = ((1.0, 160), (2.0, 277), (3.0, 342), (4.0, 376), (5.0, 391))
            got = [(d, (kph_at(r, 8.0 + t - 0.05, 8.0 + t + 0.05) or [0])[-1]) for t, d in dos]
            c(all(abs(g - d) <= 12 for d, g in got), suf + ': climb within 12 kph of DOSBox', str(got))
        if full:
            got = [(d, (kph_at(r, 8.0 + t - 0.05, 8.0 + t + 0.05) or [0])[-1]) for t, d in ((1.0, 277), (2.0, 342))] if mech.startswith('nova') else []
            c(all(abs(g - d) <= 12 for d, g in got), suf + ': from FULL within 12 kph of DOSBox (277 / 342 at 1 / 2 s)', str(got))
            end = kph_at(r, 23.0, 24.0)
            c(end and min(end) == max(end) == top, suf + ': landed, back to the walking speed', str(sorted(set(end))))


@sequence
def jump_velocity_held_in_air(c):
    """airborne the velocity is not servoed (0x10019310 / DOS 0x26990: the walking approach is in the grounded branch
    only; the throttle servos run on): FULL pressed 1 s into a standing jump changes nothing until touch-down, then the
    speed eases up to FULL's. DOSBox ORANSCN1 Nova, j held 3 s, '0' at +1 s: the readout (the vertical speed only) is
    the same with and without the '0' until landing (129 kph at the top of the climb, 184 just before touch-down), then
    0 -> 159 within ~1 s; without it stays 0"""
    ra = c.run('_full', secs=24, keys=[(9, '0')], hold=[(8, 11, 'j')], dump='/1', nostart=True, mission='ORANSCN1',
               mech='nova/nva00std', env={'MW2_KPH_TRACE': '1'})
    rb = c.run('_none', secs=24, hold=[(8, 11, 'j')], dump='/1', nostart=True, mission='ORANSCN1', mech='nova/nva00std',
               env={'MW2_KPH_TRACE': '1'})
    a, b = kph_at(ra, 8.0, 20.5), kph_at(rb, 8.0, 20.5)
    c(a and a == b, 'in the air: the same readout with and without FULL', '%s / %s' % (a[::30], b[::30]))
    c(a and 120 <= max(kph_at(ra, 10.0, 11.5)) <= 135, 'the climb tops out ~129 kph (vertical, DOSBox 129)', str(max(kph_at(ra, 10.0, 11.5) or [0])))
    ea, eb = kph_at(ra, 23.0, 24.0), kph_at(rb, 23.0, 24.0)
    c(ea and min(ea) == max(ea) == 160, 'landed: eased up to FULL (160)', str(sorted(set(ea))))
    c(eb and max(eb) == 0, 'landed without the throttle: standing', str(sorted(set(eb))))
@sequence
def structure_bracket_zoomed(c):
    """user (2026-10-13, ORANSCN1, TEXTURES TITANIUM, zoomed): a structure targeted with q (TARGET_AT_RETICLE,
    0x1001d870) gets the corner brackets (0x10022830 -> 0x10022ea0) in the main view, not the nav marker - the target
    display showing it is not enough. ORANSCN1's pump station (structure 12, nav Alpha 70 m from it) from 93 m south,
    plain and zoomed x4 (Z held 1.5 s), then no mark once the target is reset (Ctrl+t: the selector empty, no nav
    selected - target_lost_no_marker); the same with the
    Titanium textures when the titanium/ folder is there. DOSBox SILVSCN1 (the other pump-station mission): q on a
    structure draws the red corners round it (docs/reference/structure_bracket_silv_dos.png)"""
    def check(r, what):
        c(r.at(10.5)['thing'] == 12, what + ': q targets the pump station (structure 12)', 'thing %d' % r.at(10.5)['thing'])
        c(r.at(10.5)['bracket'] == 2, what + ': a thing\'s corner brackets, not the nav marker', 'bracket %d' % r.at(10.5)['bracket'])
        c(r.at(12.5)['zoom'] > 3.9 and r.at(12.5)['bracket'] == 2, what + ': zoomed x4: still the brackets',
          'zoom %.2f bracket %d' % (r.at(12.5)['zoom'], r.at(12.5)['bracket']))
        c(r.at(14.5)['thing'] == -1 and r.at(14.5)['bracket'] == 0, what + ': target reset: no mark at all (the selector empty, 0x1001ce80 -> 0x10022830)',
          'thing %d bracket %d' % (r.at(14.5)['thing'], r.at(14.5)['bracket']))
    env = {'MW2_START': '89807,2080', 'MW2_START_HEADING': 0}   # BLKX 359 degrees (pump_tank_destroyed_stays_gone)
    keys = [(9.7, 'q'), (10, 'q'), (14, 'ctrl+t')]   # a plain structure: the second q (DAT_1024c8b0, target_q_twice)
    hold = [(10.6, 12.1, 'z')]
    r = c.run(secs=15, mission='ORANSCN1', env=env, keys=keys, hold=hold, dump='/0.5')
    check(r, 'original')
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: the Titanium run skipped')
        return
    e2 = dict(env, MW2_CONFIG=titanium_cfg('structure_bracket_zoomed_ti', tdir), MW2_SKYGND=os.path.join(tdir, 'SKYGND.PAR'), MW2_TEXTURES='titanium')
    t = c.run('_ti', secs=15, mission='ORANSCN1', env=e2, keys=keys, hold=hold, dump='/0.5')
    check(t, 'titanium')


@sequence
def zoomed_turn_taps(c):
    """user (2026-10-13): turning left / right while zoomed sometimes did not respond. The key ramp (0x10039ee0) steps
    on every frame a key is seen held - the first too - and zeroes a _delta control's value on the first frame none is;
    the port's ramp ran on a free-running 30 fps clock, so a press waited up to 1/30 s for its first step (a short tap
    between two ticks: no turn at all) and the turn ran on up to 1/30 s after the release - at 144 fps the same 0.05 s tap
    turned anywhere from 0 to ~1.2 degrees. Now the steps start on the press: zoomed x4, every 0.05 s tap turns the
    same (2 steps at 144 fps: 0.071 x 5 + 0.213 x 2 frames of 90 deg/s / 144 = 0.49 degrees; 60 fps: 0.071 x 2 + 0.213 x 1
    frame / 60 = 0.53) and the turn stops with the key"""
    for fps in (144, 60):
        taps = [(11.0 + 0.7 * i + 0.013 * i, 11.05 + 0.7 * i + 0.013 * i, 'left' if i % 2 else 'right') for i in range(8)]
        hold = [(9.2, 10.7, 'z')] + taps
        r = c.run('_%d' % fps, secs=17, env={'MW2_TEST_FPS': fps}, hold=hold, dump='/0.05')
        c(r.at(10.8)['zoom'] > 3.9, '%d fps: zoomed x4' % fps, 'zoom %.2f' % r.at(10.8)['zoom'])
        d = []
        for t0, t1, k in taps:
            a, b = r.before(t0 - 0.01)['heading'], r.at(t1 + 0.3)['heading']
            dd = b - a
            d.append(round(dd, 2))
        mag = [abs(x) for x in d]
        c(min(mag) > 0.3, '%d fps: every 0.05 s tap turns' % fps, str(d))
        c(max(mag) - min(mag) < 0.15, '%d fps: the same turn for every tap (the steps start on the press)' % fps, str(d))
        c(all((x > 0) == (i % 2 == 0) for i, x in enumerate(d)), '%d fps: right taps right, left taps left' % fps, str(d))
        # the turn stops with the key: one dump interval after the release no more change
        t0, t1, k = taps[3]
        h1, h2 = r.at(t1 + 0.05)['heading'], r.at(t1 + 0.3)['heading']
        c(abs(h2 - h1) < 0.02, '%d fps: no turn after the release' % fps, '%.2f -> %.2f' % (h1, h2))
        c.note('%d fps taps: %s' % (fps, d))


@sequence
def pump_tank_destroyed_stays_gone(c):
    """user (2026-10-13, ORANSCN1 = Wolf mission 2, Nova, image enhancement at nav Alpha): "building on second mission
    isn't fully destroyable". ORANARE1's pump-station tanks S_BPUMPE (structures 12 and 14, 100 hit points, destroyed
    variant SA1PUMPE + debris child SCCPUMPE) hang on the station base S_BPUMPB (OBJ parent 1). The swap (0x10025180)
    flags the intact entry 0x200 | 0x800 and 0x10025310 (DOS 0x362d0) creates again only hidden entries WITHOUT 0x200
    under a shown parent - the port showed every hidden child of a shown parent again, so a destroyed tank stayed drawn
    and solid but could no longer be targeted. Now: 12 destroyed -> its tank gone, the other still standing and targeted by
    q; 14 destroyed -> both gone, the base and SA1PUMPE pieces left. Placement (BLKX yaw 16.16, 0x10024e90, turned about
    the BLK centre, 0x10024ba0): ORANMA02's oranare1 at 359 degrees puts tank 12 at ~(897.9, 113.6) m - the port read
    359 x 65536 as degrees (~ -16) and had the station ~45 m off. DOSBox ORANSCN1 (start nav patched to 962,79 m, 297
    degrees, Mad Dog, image enhancement): the station's size and bearing match (docs/reference/pump_tank_dos_vs_port.png);
    one volley destroys the front tank and it is gone, the other stands until hit, then only the base is left"""
    env = {'MW2_START': '93000,10500', 'MW2_START_HEADING': 281.4, 'MW2_TEST_BLD_LIST': '1', 'MW2_BLD_TRACE': '1',
           'MW2_TEST_BLD_KILL': '9:12,12:14'}
    r = c.run(secs=13, mission='ORANSCN1', mech='nova/nva00std', env=env, keys=[(10.2, 'q'), (10.5, 'q')], dump='/0.5')
    m = re.search(r'^BLD 12 hp=100 type=6 x=(-?\d+) z=(-?\d+) PumpStation', r.out, re.M)
    c(m is not None and abs(int(m.group(1)) - 89790) < 300 and abs(int(m.group(2)) - 11380) < 300,
      'tank 12 placed by the 359-degree block (DOSBox view)', m.group(0) if m else 'no BLD line')
    sh = dict((int(a), b.split()) for a, b in re.findall(r'^bld shown ORANARE1 after (\d+): (.*)$', r.out, re.M))
    a12, a14 = sh.get(12, []), sh.get(14, [])
    c(a12 and 'S_BPUMPE/9' not in a12 and 'SA1PUMPE/8' in a12, 'tank 12 destroyed: its intact model gone, SA1PUMPE shown', ' '.join(a12))
    c('S_BPUMPE/13' in a12 and 'S_BPUMPB/1' in a12, 'tank 14 and the base still standing', ' '.join(a12))
    c(r.at(11.0)['thing'] == 14, 'q targets the remaining tank (structure 14)', 'thing %d' % r.at(11.0)['thing'])
    c(a14 and not any(x.startswith('S_BPUMPE/') for x in a14) and 'SA1PUMPE/12' in a14 and 'S_BPUMPB/1' in a14,
      'tank 14 destroyed: no tank left standing, the base remains', ' '.join(a14))
    c(r.at(12.5)['thing'] == -1 and r.at(12.5)['bld_destroyed'] == 2, 'the target dropped, two structures destroyed',
      'thing %d destroyed %d' % (r.at(12.5)['thing'], r.at(12.5)['bld_destroyed']))


@sequence
def blkx_block_yaw(c):
    """a world block's BLKX turn is 16.16 degrees (engine 0x10024e90: x DAT_10247678 = 1/65536) about the centre of the
    included record's BLK box, then its offset (0x10024ba0: T(-c), R, T(t), T(c)). The port read the raw value as
    degrees: -90 / +-45 / 270 came out as whole turns (no turn) and 359 as ~ -16. BROWSCN1's broware1 (-90): the
    Comm. Unit S_BFDHQA lies 68 m west of where the unturned block put it - DOSBox from (2580, 680) m at 285 degrees
    sees it long side on, as the port now does (docs/reference/blkx_rotation_brow_dos_vs_port.png)"""
    r = c.run(secs=2, mission='BROWSCN1', env={'MW2_TEST_BLD_LIST': '1'}, dump='/1')
    m = re.search(r'^BLD 0 hp=25 type=2 x=(-?\d+) z=(-?\d+) Comm\. Unit', r.out, re.M)
    c(m is not None and abs(int(m.group(1)) - 245300) < 300 and abs(int(m.group(2)) - 70770) < 300,
      'the Comm. Unit placed by the turned block', m.group(0) if m else 'no BLD line')

@sequence
def cockpit_sound_rules(c):
    """the engine's sound levels and positional rules. Positional sounds (0x10032bf0 -> 0x10032ad0): level 100 - 100
    (d / 500 m)^2, halved when the caller passes the cockpit-view flag (DAT_1024e678 - every caller: footsteps, gait,
    KICKCHNK, NONPSTRT, landing, AI weapons 0x10043b84, effects 0x10045d00 unless flag 0x4000; MECTURX2 the other way),
    and started distance x 0x1024a984 (1 / 182.15 ticks per cm: 331 m/s) late (0x100328e0: start time +0x3a8; DOS 0x40bc0
    x 0x168 / 65536). Standing before YELL's chem-plant pipe (~41 m) and shooting it: the impacts' sounds start
    ~0.125 s late, at 0.40 in the cockpit, 0.80 in the chase view. Non-positional engine sounds at volume 0x64 (0x10032b60) play at SFX_V64
    (0.80, the level of a positional sound at 0 m - DOSBox SB16 per channel: both 101/128):
    IREDMODE (l, 0x10007aa0); the satellite map (F3) beeps MECOMBEP 0xe8 both ways (descriptor 0x1024a6d0 +0xc / +0x10,
    0x10003521 / 0x100035c0)"""
    snd = lambda r: [(float(t), int(i), float(v), float(d) if d else 0.0) for t, i, v, d in
                     re.findall(r'SFXLOG (-?[\d.]+) snd (\d+) vol ([\d.]+)(?: delay ([\d.]+))?', r.out)]
    keys = [(UP - 0.3, 'q'), (UP, 'q'), (UP + 0.3, 'backslash'), (UP + 0.6, 'l'), (UP + 1.0, 'f3'), (UP + 1.5, 'f3')]
    r = c.run(secs=UP + 6, env={'MW2_START': '315212,244208', 'MW2_START_HEADING': 0, 'MW2_SFX_LOG': '1'}, keys=keys,
              hold=pulses(UP + 2, UP + 5, 'space', 1.0, 0.1))
    s = snd(r)
    pos = [x for x in s if x[3] > 0 and UP + 1.9 < x[0] < UP + 2.4 and x[1] != 259]   # the first volley's (not footsteps NONFOOT)
    c(pos and all(0.10 < x[3] < 0.16 for x in pos), 'the impacts ~41 m off start ~0.125 s late', str(pos[:4]))
    c(pos and all(x[2] < 0.42 for x in pos), 'halved in the cockpit (0.40)', str(sorted(set(x[2] for x in pos))))
    c([x[2] for x in s if x[1] == 0xb2] == [0.80], 'IREDMODE at SFX_V64', str([x for x in s if x[1] == 0xb2]))
    c([x[2] for x in s if x[1] == 0xe8] == [0.80, 0.80], 'the satellite map beeps MECOMBEP on and off', str([x for x in s if x[1] == 0xe8]))
    ext = {'MW2_START': '315212,244208', 'MW2_START_HEADING': 0, 'MW2_SFX_LOG': '1',   # the chase view (MW2_AUTOPILOT cockpit 0)
           'MW2_AUTOPILOT': '%g,-9,-999,0,%s,0,0' % (UP + 6, os.path.join(OUT, c.name + '_ext', 'final.ppm'))}
    r = c.run('_ext', secs=UP + 6, env=ext, keys=keys[:2], hold=pulses(UP + 2, UP + 5, 'space', 1.0, 0.1))
    pos = [x for x in snd(r) if x[3] > 0 and UP + 1.9 < x[0] < UP + 2.4 and x[1] != 259]
    c(pos and all(x[2] > 0.75 for x in pos), 'in the chase view not halved (0.80)', str(sorted(set(x[2] for x in pos))))

def userstar_with_mate(skel='timbrwlf', loadout='tbr00std'):
    """USERSTAR.BWD for a star of two: the player's GPS and a copy for an AI starmate (+13 leader 0, +14 2 = AI, as
    lance.c / the shell's FUN_0001d310 write it)"""
    d = bytearray(open(os.path.join(GTEST, 'USERSTAR.BWD'), 'rb').read())
    g = d.find(b'GPS\0')
    n = struct.unpack_from('<I', d, g + 4)[0]
    for off, nm in ((g + 36, skel), (g + 45, loadout)):
        b = nm.lower().encode()[:8]
        d[off:off + 9] = b + b'\0' * (9 - len(b))
    mate = bytearray(d[g:g + n])
    mate[13] = 0
    mate[14] = 2
    out = d[:g + n] + mate + d[g + n:]
    struct.pack_into('<I', out, 4, len(out))
    return bytes(out)


@sequence
def unit_state_messages(c):
    """0x10031870 / 0x10031810 posted by units' state changes. A starmate (its group led by the player, 0x10018f90)
    destroyed: 0x10016280 -> 0x10031810(6, its point +0xc): "Point 2" "destroyed" (voices 264, 0x23). An enemy unit back
    up after a shutdown or a node's power-down (object +0x14 bit 0x10, set in state 3): on reaching state 2 (0x1001a180
    case 2) with the player up (+0x15 & 0x20) 0x1a "Enemy power-up detected" (74); its start-up (state 0 -> 1,
    0x1001ab71) sounds NONPSTRT 0x104 at the mech for an enemy once the player is up (MECTURX2 for others). TEST ONLY:
    MW2_TEST_APOWER powers actor 0 (an enemy) down at 10 s and wakes it at 12 s: online 0x43e + rand(0x16a) ticks later"""
    r = c.run(secs=22, env={'MW2_TEST_APOWER': '10:0:0,12:0:1', 'MW2_SFX_LOG': '1'})
    m = re.search(r'MSGV now=(\d+) voice=74 Enemy power-up detected', r.out)
    c(m is not None and 12000 + 0x43e * 1000 / 182 - 100 <= int(m.group(1)) <= 12000 + (0x43e + 0x16a) * 1000 / 182 + 100,
      '"Enemy power-up detected" (74) once the woken enemy is online', m.group(0) if m else 'none')
    c(len(re.findall(r'voice=74 ', r.out)) == 1, 'said once')
    r = c.run('_mate', secs=14, files={'USERSTAR.BWD': userstar_with_mate()}, env={'MW2_DEBUG_SPAWN': '1'})
    sp = [l for l in r.out.splitlines() if l.startswith('spawn ')]
    mate = next((i for i, l in enumerate(sp) if l.split()[1].upper() == 'USERSTAR'), None)
    if not c(mate is not None, 'the starmate spawned', str(sp[:6])):
        return
    r = c.run('_mate2', secs=14, files={'USERSTAR.BWD': userstar_with_mate()}, env={'MW2_TEST_AHIT': '11:%d:3:9:400' % mate})
    m = re.search(r'MSGV now=(\d+) voice=264 Point 2 destroyed', r.out)
    c(m is not None and 11000 <= int(m.group(1)) <= 11500, '"Point 2 destroyed" (264 + 0x23) when the starmate dies',
      m.group(0) if m else str([l for l in r.out.splitlines() if 'MSGV' in l][:5]))




def titanium_viewer_shot(name, tdir, record, mission, yaw, heading, extra=None):
    """the single-mech viewer (glview RECORD MISSION) with TEXTURES TITANIUM, a still pose, the camera's orbit yaw and the
    mech's heading (TEST ONLY MW2_TEST_VIEWER), one picture after 1 s; returns (log, picture path)"""
    out = os.path.join(OUT, name, 'view.ppm')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    e = dict(os.environ)
    e.update({'MW2_CONFIG': titanium_cfg(name, tdir), 'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'), 'MW2_TEXTURES': 'titanium',
              'MW2_INSTALL_DIR': GTEST, 'SDL_VIDEODRIVER': 'offscreen', 'SDL_AUDIODRIVER': 'dummy', 'MW2_SIZE': '640x480',
              'MW2_EDITION': 'enhanced', 'MW2_NO_STARTUP': '1', 'MW2_TEST_VIEWER': '%g,10,%g' % (yaw, heading),
              'MW2_AUTOPILOT': '1,0,0,0,%s,0' % out})
    if extra:
        e.update(extra)
    cmd = [GLVIEW, os.path.join(GAME, '3d', 'models.prj'), os.path.join(GAME, '3d', 'textures.prj'), 'ati', record, mission]
    try:
        p = subprocess.run(cmd, env=e, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120 * TSCALE)
        log = p.stdout.decode('latin-1')
    except subprocess.TimeoutExpired as x:
        log = (x.stdout or b'').decode('latin-1') + '\n[TIMEOUT]'
    with open(os.path.join(OUT, name, 'log.txt'), 'w') as f:
        f.write(log)
    return log, out


@sequence
def titanium_model_frame_light(c):
    """the Titanium lights fall on its models' own-frame normals (mercsw 0x1002ecaa reads the vertex +0x40 normal made at
    load, 0x10003df0, which the transform 0x10004934 never turns): a mech lit the same whichever way it faces - the
    single-mech viewer's Titanium Timber Wolf (the record's Titanium model, as the Titanium engine has no other) under
    TNW1's side lights at heading 0 and -90 with the camera turned alike: the torso the same (normals turned as the 3D
    editions do, TEST ONLY MW2_TEST_TI_WORLDN: different) (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    box = (0.40, 0.28, 0.60, 0.40)   # the torso, in front of the background at both turns
    la, a = titanium_viewer_shot('titanium_model_frame_light_0', tdir, 'TIMBRWLF', 'TNW1', 0, 0)
    lb, b = titanium_viewer_shot('titanium_model_frame_light_90', tdir, 'TIMBRWLF', 'TNW1', 90, -90)
    c('titanium: viewer TIMBRWLF as its Titanium model' in la, 'the viewer shows the Titanium model', la[-300:])
    _, wa = titanium_viewer_shot('titanium_model_frame_light_w0', tdir, 'TIMBRWLF', 'TNW1', 0, 0, {'MW2_TEST_TI_WORLDN': '1'})
    _, wb = titanium_viewer_shot('titanium_model_frame_light_w90', tdir, 'TIMBRWLF', 'TNW1', 90, -90, {'MW2_TEST_TI_WORLDN': '1'})
    d, dw = mean_diff(a, b, box=box), mean_diff(wa, wb, box=box)
    c(0 <= d < 3, 'the torso lit the same at both headings (own-frame normals)', 'mean diff %.2f' % d)
    c(dw > 10, 'turned normals would light it differently (the check sees it)', 'mean diff %.2f' % dw)
    m = re.search(r'titanium: shade (\w+) amb (-?\d+) light (\d) dir (\d) vec (-?\d+) (-?\d+) (-?\d+) gi (\d+)', la)
    c(m is not None and m.groups() == ('LITE', '20', '1', '1', '100000', '20000', '0', '60'),
      'TNW1: the type 1 palette shade from its LITE (amb 20, directional 100000 20000 0) and globalintensity 60', m.group(0) if m else 'no line')
    c.note('torso mean diff %.2f (turned normals %.2f)' % (d, dw))


@sequence
def titanium_kinds_debris_lights(c):
    """TEXTURES TITANIUM, its colour kinds as mercsw 0x10053280 draws them: kind 2 has no case there (not drawn) - CYAN's
    tower at (-940, 1726) m is Titanium kind 2 lattice (SAATFBSI / SCFTFBSI 0x24c6) and leaves the sky open where the
    original's girders stand; a blown-off mech part flies as the Titanium mech's own piece; the first projectile of a
    volley carries a light (0x1006cd8b: an LRM's, its pool model starting with a kind 4 polygon, the palette colour / 128)
    (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    env = {'MW2_CONFIG': titanium_cfg('titanium_kinds', tdir), 'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'), 'MW2_TEXTURES': 'titanium',
           'MW2_START': '-93970,169600', 'MW2_START_HEADING': '0'}
    r = c.run(secs=2, throttle=0, nostart=True, mission='CYANSCN1', shots=[(1.5, 'tower')], env=env)
    env_o = dict(env)
    env_o['MW2_TEXTURES'] = 'original'
    o = c.run('_orig', secs=2, throttle=0, nostart=True, mission='CYANSCN1', shots=[(1.5, 'tower')], env=env_o)
    box = (0.30, 0.08, 0.70, 0.30)   # above the horizon, through the cockpit's top window
    bright = lambda R, G, B: (R.astype(int) + G + B) > 300
    nt, no = count_px(r.shots.get('tower'), bright, box), count_px(o.shots.get('tower'), bright, box)
    c(nt > 2 * no + 500, 'the kind 2 lattice not drawn: open sky where the original has girders', 'bright sky pixels %d (original %d)' % (nt, no))
    d = c.run('_debris', secs=6, nostart=True, mission='YELLSCN1',
              env={'MW2_CONFIG': titanium_cfg('titanium_kinds_debris', tdir), 'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'),
                   'MW2_TEXTURES': 'titanium', 'MW2_KILL_ACTOR': '0', 'MW2_TEST_PASSIVE': '1', 'MW2_TEST_AHIT': '4:0:3:11:40'})
    m = re.search(r'titanium: blown-off piece (\S+) as (\S+)', d.out)
    c(m is not None, 'a blown-off part drawn as the Titanium piece', m.group(0) if m else d.out[-300:])
    keys, hold = [(2.5, 'backslash')], [(2.8, 3.0, 'space')]
    l = c.run('_lrm', secs=4, throttle=0, nostart=True, mission='CYANSCN1', keys=keys, hold=hold,
              env=titanium_chase_env('titanium_kinds_lrm', tdir, 4))
    m = re.search(r'titanium: projectile light weapon (\d+) colour ([\d.]+) ([\d.]+) ([\d.]+)', l.out)
    c(m is not None, 'a projectile light (the volley\'s first missile)', l.out[-300:])
    c(m is not None and max(float(x) for x in m.groups()[1:]) > 0.25, 'its colour from the pool model\'s palette entry (not the 0.2 grey)',
      m.group(0) if m else '')
    c.note('%s; %s' % (m.group(0) if m else '-', 'sky pixels %d / %d' % (nt, no)))
@sequence
def palette_slots_3d(c):
    """the 3D editions' palette (0x1002ad50 / 0x1002acf0 / 0x1002ac00 -> 0x1002ac40): nothing fades on screen (the Glide
    driver's set-palette entry is empty); the HUD's table and the flat colours are reloaded from a PALG slot only when a
    fade that does not return ends. The time of day is evaluated once (0x100079d0 needs DAT_1024a980 = -1): YELLSCN1 from
    1740 s (dawn, slot 4 YELL_DU) stays dawn past 1750 s (day); light amplification at 20 s: slot 0xc (INFRARED) 0xb5
    ticks later; off at 22.5 s: the time of day re-evaluated (0x10007a50 resets DAT_1024a980) - day, slot 0, 0xb5 ticks
    later. HUD colour 13: YELL_DU (0,35,0) 008e00, INFRARED / YELL_DA (0,39,0) 009e00"""
    r = c.run(secs=24.5, keys=[(20, 'l'), (22.5, 'l')], env={'MW2_TEST_TOD_START': '1740'}, dump='/0.25')
    sl = lambda t: r.at(t).get('hudslot')
    c(sl(5) == 4 and r.at(5).get('hudc13') == 0x008e00, 'start: dawn (slot 4, YELL_DU)', 'slot %s c13 %06x' % (sl(5), r.at(5).get('hudc13', 0)))
    c(sl(19.5) == 4, 'the time of day does not run (1740 + 19 s is day)', 'slot %s' % sl(19.5))
    c(sl(20.75) == 4 and sl(21.25) == 12, 'light amplification: the HUD in slot 0xc after 0xb5 ticks', 'slot %s / %s' % (sl(20.75), sl(21.25)))
    c(sl(23.25) == 12 and sl(23.75) == 0 and r.at(23.75).get('hudc13') == 0x009e00,
      'off: back after 0xb5 ticks, to the time of day then (day, slot 0)', 'slot %s / %s c13 %06x' % (sl(23.25), sl(23.75), r.at(23.75).get('hudc13', 0)))


@sequence
def tod_hud_palette_dos(c):
    """the DOS edition draws the HUD in the VGA palette, so its colours follow the time-of-day fades (0x14f80 ->
    0x3eae0): YELLSCN1 from 1740 s - dawn (YELL_DU, colour 13 (0,35,0)); at the 10 s evaluation 1750 s is day: slot 0
    (YELL_DA, (0,39,0)) over 2730 ticks"""
    r = c.run(secs=29, env={'MW2_TEST_TOD_START': '1740'}, dump='/1', dos_look=True)
    a, b = r.at(8).get('hudc13'), r.at(28).get('hudc13')
    c(a == 0x008e00 and b == 0x009e00, 'the HUD follows the palette: dawn -> day', 'c13 %06x -> %06x' % (a or 0, b or 0))


@sequence
def red_flash_hud(c):
    """the damage flash (0x1002ad50(0x11, ..)): the DOS edition fades its whole palette - the HUD reddens; the 3D editions'
    HUD table is untouched by a fade (only the textured polygons take the (255, 8, 8) vertex colour) - the HUD keeps its
    green"""
    env = {'MW2_TEST_PARMOR': '3:0.25', 'MW2_TEST_PHIT': '4:3:4:40'}
    for dos in (False, True):
        r = c.run('_dos' if dos else '_3d', secs=4.3, env=env, nostart=True, shots=[(3.9, 'pre'), (4.17, 'in')], dos_look=dos)
        g0, g1 = hud_green(r.shots['pre']), hud_green(r.shots['in'])
        if dos:
            c(g1 < g0 * 0.5, 'DOS: the HUD fades toward red', 'green px %d -> %d' % (g0, g1))
        else:
            c(g0 > 300 and g1 > g0 * 0.85, '3D: the HUD keeps its colours', 'green px %d -> %d' % (g0, g1))


@sequence
def death_card_world_fixed(c):
    """the death explosion's card DEATH0 (colour 0xbfb9: 0x10026e80 draws 0xb000 as a 3D polygon through 0x1002c880, not
    the 0x3000 billboard) is spawned unrotated (0x10045c30) and has one polygon, drawn from its front only (0x1002b9b0):
    a world-fixed card in the x / y plane seen from +z - spawned 30 m ahead with the torso turned 85 degrees either way
    from heading 158, one side shows it, the other does not"""
    fire = lambda r_, g, b: (r_ > 235) & (g > 200) & (b < 215) & (r_ - b > 40)   # the card's white-hot yellow
    n = {}
    for tw in (85, -85):
        r = c.run('_%d' % (tw + 100), secs=2.65, twist=tw, nostart=True, env={'MW2_FX_TEST': '0d:2.5'}, shots=[(2.6, 's')])
        n[tw] = count_px(r.shots['s'], fire, (0.38, 0.1, 0.62, 0.45))
    c(n[85] > 200 and n[-85] < 20, 'seen from one side only', 'fire px %d / %d' % (n[85], n[-85]))


@sequence
def damage_shading(c):
    """the 3D editions light a mech's location pieces (type word 0x100) by their damage nibble n (0x1002b9b0): x (1 - n /
    15), or x (1 + n / 15) capped at full when PLNT +52 (DAT_1024e428) is set - ORANSCN1 (+52 = 0): a damaged Mad Dog
    is darker with the engine's sign than with the other"""
    hits = '4:0:3:11:2,4.1:0:2:11:2,4.2:0:4:11:2,4.3:0:5:11:2,4.4:0:6:11:2,4.5:0:7:11:2,4.6:0:8:11:2,4.7:0:1:11:1'
    m = {}
    for sign in (0, 1):
        r = c.run('_%d' % sign, secs=9, mission='ORANSCN1', nostart=True, size='800x500', shots=[(8.9, 's')],
                  env={'MW2_TEST_PASSIVE': '1', 'MW2_TEST_IMMOBILE': '0.5:0', 'MW2_TEST_AIM': '0,2500,0', 'MW2_TEST_AHIT': hits,
                       'MW2_TEST_DSIGN': sign})
        w, h, px = ppm(r.shots['s'])
        tot = 0
        for y in range(int(0.32 * h), int(0.66 * h)):
            row = px[(y * w + int(0.375 * w)) * 3:(y * w + int(0.625 * w)) * 3]
            tot += sum(row)
        m[sign] = tot
    c(m[0] < m[1] * 0.93, 'damaged pieces darker with sign 0', 'sum %d vs %d' % (m[0], m[1]))


@sequence
def titanium_kind2_pool(c):
    """TEXTURES TITANIUM: a colour kind 2 polygon is drawn only where mercsw draws it - not in the normal view (0x10053280
    has no kind 2 case: CYAN's lattice tower leaves the sky open, see titanium_kinds_debris_lights), but in image
    enhancement (0x10055a5d draws every polygon's outline over a black fill: standing 30 m from the tower its black faces
    hide the world's edges right of it); the projectiles and debris chunks take the Titanium archive's own MW2_SHT1 pool
    models (kind 3 = MISSLE1 there, debris the type 0x0b XPLO entries CHUNKER1 / CHUNKER2) (skipped without the titanium/
    folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    env = {'MW2_CONFIG': titanium_cfg('titanium_kind2_pool', tdir), 'MW2_SKYGND': os.path.join(tdir, 'SKYGND.PAR'), 'MW2_TEXTURES': 'titanium',
           'MW2_START': '-93970,169600', 'MW2_START_HEADING': '0', 'MW2_VISION': '2'}
    r = c.run(secs=2, throttle=0, nostart=True, mission='CYANSCN1', shots=[(1.5, 'wire')], env=env)
    e2 = dict(env)
    e2['MW2_TEXTURES'] = 'original'
    o = c.run('_orig', secs=2, throttle=0, nostart=True, mission='CYANSCN1', shots=[(1.5, 'wire')], env=e2)
    box = (0.62, 0.40, 0.95, 0.60)
    edge = lambda R, G, B: ((R.astype(int) + G + B) > 60) & (G < 100)
    nt, no = count_px(r.shots.get('wire'), edge, box), count_px(o.shots.get('wire'), edge, box)
    c(0 <= nt < 500, 'image enhancement: the kind 2 lattice drawn (its black faces hide the world behind)', 'edge px %d (original look %d)' % (nt, no))
    l = c.run('_lrm', secs=4, throttle=0, nostart=True, mission='CYANSCN1', keys=[(2.5, 'backslash')], hold=[(2.8, 3.0, 'space')],
              env=titanium_chase_env('titanium_kind2_pool_lrm', tdir, 4))
    m = re.search(r'titanium: pool models (\S+) (\S+) (\S+) (\S+) (\S+) (\S+) (\S+), debris (\S+) (\S+)', l.out)
    c(m is not None and m.group(4) == 'MISSLE1' and m.group(7) == 'MISSLE1', 'the Titanium pool: LRM and SRM both MISSLE1', m.group(0) if m else l.out[-300:])
    c(m is not None and (m.group(8), m.group(9)) == ('CHUNKER1', 'CHUNKER2'), 'debris: the pool\'s type 0x0b chunks', m.group(0) if m else '')
    c('titanium: shot drawn as pool model' in l.out, 'a shot drawn with the Titanium pool model', l.out[-300:])
    c.note('%s; edge px %d / %d' % (m.group(0) if m else '-', nt, no))
@sequence
def tod_start_3d_pinned(c):
    """the 3D editions' start time (0x10007870): the dword at 0x1007b068 + 4 x INIT +8 in MW2.DLL - 0..3 the boundary
    table (JACK 0: 5 h of 6000 -> 1250, dawn); past it .bss as FirstEnvironment() finds it: YELL 1500 -> the camera-shake
    list (0x1007c718, 0) -> night, slot 8; CYAN 100 -> a KEATING file name's ".SFL" (0x4c46532e) -> 230 of 1000, dawn;
    MARO 300 -> 530 of 900, day; PROV -20000 -> .text 0x100677e8 (0x07dc7805) -> 39157 of 50000, dusk. The DOS look
    keeps INIT +8 as seconds (YELL 1500 s, dawn)"""
    want = {'YELLSCN1': (8, 0), 'CYANSCN1': (4, 0x4c46532e), 'MAROSCN1': (0, 0x4c46532e), 'PROVSCN1': (4, 0x07dc7805),
            'JACKSCN1': (4, 1250)}
    for scene, (slot, start) in want.items():
        r = c.run('_' + scene, secs=0.3, mission=scene, nostart=True, env={'MW2_DEBUG_HUDPAL': '1'})
        m = re.search(r'hud palette: \w+ slot (\d+) \(\w+\) start (-?\d+)', r.out)
        c(m is not None and int(m.group(1)) == slot and int(m.group(2)) == start, scene + ': slot %d, start %d' % (slot, start),
          m.group(0) if m else 'no line')
    r = c.run('_dos', secs=0.3, nostart=True, dos_look=True, env={'MW2_DEBUG_HUDPAL': '1'})
    m = re.search(r'hud palette: \w+ slot (\d+) \(\w+\) start (-?\d+)', r.out)
    c(m is not None and int(m.group(1)) == 4 and int(m.group(2)) == 1500, 'DOS look: YELL from 1500 s (dawn)', m.group(0) if m else 'no line')


@sequence
def effect_card_pool_slots(c):
    """effect cards take the first free pool slot of their type (0x10045d00; the pool in MW2_SHT1's XPLO order, 0x1003ed70)
    and that slot's own model: muzzle flash type 0x17 slot 0 MZL0_1 has both windings, slot 1 MZL0_2 one polygon
    (0,1,2,3) - drawn from one side only (0x1002b9b0). The flash spawned unrotated 30 m ahead (MW2_FX_TEST type:secs:n -
    n - 1 of the type first, behind the player)"""
    def card(r, slot):
        return [(int(m.group(1)), int(m.group(2))) for m in re.finditer(r'FXCARD now=\d+ type 17 slot %d polys (\d+) drawn (\d+)' % slot, r.out)]
    r1 = c.run('_s0', secs=2.6, twist=85, nostart=True, env={'MW2_FX_TEST': '17:2.5:1'}, dump='/1')
    r2 = c.run('_s1', secs=2.6, twist=85, nostart=True, env={'MW2_FX_TEST': '17:2.5:2'}, dump='/1')
    r3 = c.run('_s1b', secs=2.6, twist=-85, nostart=True, env={'MW2_FX_TEST': '17:2.5:2'}, dump='/1')
    a, b, d = card(r1, 0), card(r2, 1), card(r3, 1)
    c(a and a[0] == (2, 1), 'slot 0 (MZL0_1): two polygons, seen', str(a[:1]))
    c(b and b[0] == (1, 0), 'slot 1 (MZL0_2): one polygon, not seen from this side', str(b[:1]))
    c(d and d[0] == (1, 1), 'slot 1 from the other side: seen', str(d[:1]))


@sequence
def locked_lrm_hits_trainer(c):
    """user report "missiles are not hitting enemies properly.. they seem to land near their feet" (TNJ trainers at
    600-750 m, locked LRMs, missile camera): the guidance (0x10045910; DOS 0x50d50) steers at the unit OBJECT's position
    +0x50..+0x58, whose height is the feet + the MGEO centre height (the move 0x10019310 stores object y = ground + controller
    +0xcc at 0x10019fad; +0xcc = MGEO int[0], 0x100400b0) - not the feet. DOSBox TNJ6 (the instructor moved 700 m north of
    the start in a copy of MW2.PRJ): the locked LRM 10 volley flies level at hip height and bursts in the Mad Dog's middle.
    Here TNJ6's instructor (Mad Dog, centre 550) 700 m south of the player on open ground: every missile strikes the mech
    about 5.5 m up (none at the feet, none on the ground) and damages it; the same with the instructor walking sideways
    at 5 m/s (MW2_TEST_STRAFE)"""
    env = {'MW2_START': '-18364,79502', 'MW2_START_HEADING': 180, 'MW2_TEST_TARGET': '0', 'MW2_TEST_HOLD_ACTOR': '0',
           'MW2_MISSILE_TRACE': '1'}
    sel = [(6 + 0.4 * i, 6.1 + 0.4 * i, 'enter') for i in range(4)]   # TNJ6 Mad Dog MDG02TRN (LRM 10 x2): sel 1 -> 5
    for suffix, extra in (('', {}), ('_walking', {'MW2_TEST_STRAFE': '0,500,9,30'})):
        r = c.run(suffix, secs=18, env=dict(env, **extra), mission='TNJ6SCN1', hold=sel + [(11, 11.1, 'space')])
        c(r.at(10.9)['lock'] & 0x80, suffix + ' locked when fired', 'lock 0x%x' % r.at(10.9)['lock'])
        paths, tgt = missile_paths(r)
        c(len(paths) >= 10, suffix + ' an LRM 10 volley in flight', '%d missiles' % len(paths))
        unit = [l.split() for l in r.out.splitlines() if l.startswith('mhit ') and ' unit ' in l]
        other = [l for l in r.out.splitlines() if l.startswith('mhit ') and ' unit ' not in l]
        ups = sorted(int(f[9]) for f in unit)
        c.notes.append('%s: %d unit hits, %d ground / objects; groups %s; heights %s' % (suffix or 'standing', len(unit), len(other),
                                                                                      sorted(set(int(f[5]) for f in unit)), ups))
        c(len(unit) >= 9, suffix + ' the volley strikes the trainer', '%d unit hits, %d other' % (len(unit), len(other)))
        c(len(other) <= 1, suffix + ' no ground impacts in front of it', '\n'.join(other[:3]))
        c(ups and ups[0] > 250, suffix + ' every hit well above the feet (> 2.5 m; centre 5.5 m)', str(ups))
        h0, h1 = r.at(10.9)['tgt_health'], r.last['tgt_health']
        c(h1 < h0 - 10, suffix + ' the volley damages the trainer', 'health %d -> %d' % (h0, h1))


@sequence
def archive_jade_falcon(c):
    """agent SH: the Jade Falcon Archive as DOSBox shows it (docs/reference/dos_archive_jf_*.png, 2026-10-13): the text box
    FUN_0003c410(.., 0x58, 0x46, 0x1d2, 0xde) as Wolf's, a word wrapped when it and its following spaces pass 466 px (Clan
    Origins page 3: "...and his Clans." ends a paragraph at 463 px and stays); BACK (FUN_00027160 case 3) on a later
    sub-page -> the entry's first, then -> the page the link came from, and on the Archive's first page it leaves for
    the hall as EXIT does (-2 out of FUN_00026b50). Pixel compare outside the DOS pointer (parked at 600, 60)"""
    from PIL import Image
    import numpy as np
    shell = os.path.join(os.path.dirname(GLVIEW), 'mw2shell')
    mech2 = os.path.join(os.path.dirname(GAME.rstrip('/')), 'MECH2')
    d = os.path.join(OUT, 'archive_jade_falcon')
    os.makedirs(d, exist_ok=True)
    ref = os.path.join(REPO, 'docs', 'reference')
    for name, env, refn in (('first', dict(MW2_ARCH_PAGE='0,0'), 'dos_archive_jf_0.png'),
                            ('origins3', dict(MW2_ARCH_PAGE='2,2'), 'dos_archive_jf_origins_3.png'),
                            ('back1', dict(MW2_ARCH_PAGE='2,2', MW2_ARCH_FROM='1,0', MW2_ARCH_BACK='1'), 'dos_archive_jf_back1.png'),
                            ('back2', dict(MW2_ARCH_PAGE='2,2', MW2_ARCH_FROM='1,0', MW2_ARCH_BACK='2'), 'dos_archive_jf_back2.png'),
                            ('back3', dict(MW2_ARCH_PAGE='2,2', MW2_ARCH_FROM='1,0', MW2_ARCH_BACK='3'), None)):
        out = os.path.join(d, name + '.ppm')
        e = dict(os.environ, MW2_SHELL_SCREEN='5', MW2_SHELL_CLAN='1', MW2_SHELL_FRAMES='5', MW2_SHELL_SHOT=out,
                 SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy', MW2_CONFIG=os.path.join(d, 'x.cfg'), MW2_BITMAP_TEXT='1', **env)
        r = subprocess.run([shell, mech2], env=e, capture_output=True, text=True, timeout=120)
        if refn is None:
            c('archive back: page 1 sub 0 next 1' in r.stdout, 'BACK on the first page leaves the Archive (EXIT)', r.stdout[-300:])
            continue
        if not os.path.exists(out):
            c(False, '%s rendered' % name)
            continue
        a = np.array(Image.open(out).convert('RGB')).astype(int)
        b = np.array(Image.open(os.path.join(ref, refn)).convert('RGB')).astype(int)
        diff = np.abs(a - b).max(2) > 6
        diff[45:95, 590:640] = False   # the DOS mouse pointer
        c(int(diff[0:440].sum()) == 0, '%s matches DOSBox (%s)' % (name, refn), '%d px differ' % int(diff[0:440].sum()))


@sequence
def input_sinks(c):
    """agent SH: the throttle and the torso as the engine's analog sinks (0x10039ee0 / 0x1003a0a0, control table
    0x10256540), per frame at 30 fps (182 / 30 ticks): a held key adds (ticks x 3) << shift to the velocity, the value
    (-1..1) by the velocity. Throttle (shift 6, 0..1024 = floor(512 (v + 1))): from rest '=' held 0.3 s (9-10 frames)
    -> 40-49 %, full on the 15th frame (0.5 s; DOSBox YELLSCN1: the same ramp at 17 fps full in ~0.65 s, at 4 fps a 1 s
    hold 65 %). torso_pan (shift 3, +-90): '.' 0.5 s (15-16 frames) -> 24.0-27.2 degrees (DOSBox at 4 fps: 1 / 4 / 8 / 14.8
    degrees a frame = this ramp). torso_tilt (shift 4, +-60, positive down): Up tilts down. The mouse (MOUSE.DLL: a 1024
    box, a mickey a unit; MOUSE.MAP torso_pan + Left/Right, torso_tilt - Up/Down): 128 counts right -> 22.5 degrees
    (180 / 1024 a count; DOSBox unlocked moves 1/8 unit a pixel: 128 px -> 2.8 degrees measured), back 64 -> 11.25;
    64 counts down -> the torso up 7.5 degrees (120 / 1024 a count; DOSBox: the mouse down tilts up)"""
    t0 = UP
    hold = [(t0, t0 + 0.3, 'equal'), (t0 + 1.5, t0 + 2.0, 'period'), (t0 + 2.5, t0 + 2.6, 'kp5'), (t0 + 3.0, t0 + 3.3, 'up'),
            (t0 + 3.8, t0 + 3.9, 'kp5')]
    mouse = '%g:0:0,%g:128:0,%g:-64:0,%g:0:64' % (t0 + 3.9, t0 + 4.5, t0 + 5.5, t0 + 6.5)
    r = c.run(secs=t0 + 7.5, throttle=-9, hold=hold, env={'MW2_SINK_TRACE': '1', 'MW2_MOUSE': mouse}, dump='/0.25')
    sk = []
    for l in r.out.splitlines():
        if l.startswith('SINK '):
            sk.append(dict((k, float(v.split(',')[0])) for k, v in (kv.split('=') for kv in l.split()[1:])))
    def at(t):
        for d in sk:
            if d['t'] >= t:
                return d
        return sk[-1] if sk else {}
    if not sk:
        c(False, 'SINK trace')
        return
    thr = [d['thr'] for d in sk]
    c(0.39 <= (at(t0 + 0.5)['thr'] + 1) / 2 <= 0.50, "'=' 0.3 s from rest: 40-49 %", '%.3f' % ((at(t0 + 0.5)['thr'] + 1) / 2))
    s6 = 3 * (182 / 30) * 64 / 65536
    n_full = next(n for n in range(1, 40) if s6 * n * (n + 1) / 2 >= 2)
    c(n_full == 15, 'full throttle on the 15th frame held (0.5 s)', str(n_full))
    # the sinks give the demands (dtw / dpi); the torso's twist / pitch follow through the servos (torso_servo)
    tw = at(t0 + 2.4)['dtw']
    c(23.5 <= tw <= 27.5, "'.' 0.5 s: torso 24-27 degrees (shift 3)", '%.2f' % tw)
    p = at(t0 + 3.6)['dpi']
    c(-12 <= p <= -6, 'Up 0.3 s tilts the torso down (shift 4: 7-10 degrees)', '%.2f' % p)
    c(abs(at(t0 + 4.2)['dtw']) < 0.01 and abs(at(t0 + 4.2)['dpi']) < 0.01, 'Keypad 5 recentres', '%.2f %.2f' % (at(t0 + 4.2)['dtw'], at(t0 + 4.2)['dpi']))
    c(abs(at(t0 + 5.2)['dtw'] - 22.5) < 0.05, 'mouse 128 counts right: 22.5 degrees (180 / 1024 a count)', '%.2f' % at(t0 + 5.2)['dtw'])
    c(abs(at(t0 + 6.2)['dtw'] - 11.25) < 0.05, 'mouse back 64: 11.25', '%.2f' % at(t0 + 6.2)['dtw'])
    c(abs(at(t0 + 7.2)['dpi'] - 7.5) < 0.05, 'mouse 64 down: the torso up 7.5 degrees (120 / 1024 a count)', '%.2f' % at(t0 + 7.2)['dpi'])


@sequence
def engine_sound_levels(c):
    """the engine's levels and pans for the non-positional sounds (0x10032b60 / 0x10032ba0: AIL volume, pan 0..127; the
    DOS mixer 0x79110: (volume + 1) / 128 per channel, the far side x T[pan] / T[127 - pan], T[i] = 2i up to 62 -> a
    centred sound is full on both sides; port scale 0.8 = volume 100). DOSBox YELLSCN1 SB16, each channel fitted: every
    volume-0x64 sound 101/128 like MECTURX2 at 0 m. Cockpit view: the display toggle click 0xdc (0x10037880) only once
    up, 0.80; MECBSYRX 0xce 0.80 pan 0x2f (+0.27); the lock tones (0x1001e6c0) MECBYHLX 0xcf once as acquiring starts
    (0.80, pan 0x1f +0.52), MECXLOCK 0xfe once on locking (0.80, pan 0x5f -0.50); the player's weapon (0x1001bc30) at
    volume 0x32 = 0.40, pan by location (LA 0x4f -0.25, RA 0x2f +0.27, LT centred); the player's footsteps (positional
    at the mech, halved in the cockpit) 0.40. Chase view: weapon and footsteps 0.80 centred, no lock / state tones."""
    snd = lambda r: [(float(t), int(i), float(v), float(p) if p else 0.0) for t, i, v, p in
                     re.findall(r'SFXLOG (-?[\d.]+) snd (\d+) vol ([\d.]+)(?: delay [\d.]+)?(?: pan (-?[\d.]+))?', r.out)]
    cyc = [(UP + 0.4 * i, UP + 0.1 + 0.4 * i, 'enter') for i in range(7)]   # to the LRM once up: acquiring starts then
    r = c.run(secs=UP + 7, env={'MW2_KILL_ACTOR': '0,9999', 'MW2_TEST_TARGET': '0', 'MW2_TEST_STANDOFF': 16000, 'MW2_TEST_HOLD_ACTOR': '0',
                            'MW2_SFX_LOG': '1'}, keys=[(4.5, 'f5'), (UP + 0.2, 'f5')], hold=cyc + [(UP + 5.8, UP + 5.9, 'space')])
    s = snd(r)
    up = next((x['now'] / 1000.0 for x in r.states if x.get('pow') == 2), None)
    clk = [x for x in s if x[1] == 0xdc]
    c(len(clk) == 1 and clk[0][0] > UP and clk[0][2] == 0.80 and clk[0][3] == 0, 'the display click only once up, 0.80 centred', str(clk))
    bz = [x for x in s if x[1] == 0xce]
    c(len(bz) == 1 and bz[0][2] == 0.80 and bz[0][3] == 0.27, 'MECBSYRX 0.80 pan +0.27 (0x2f)', str(bz))
    trk, lck = [x for x in s if x[1] == 0xcf], [x for x in s if x[1] == 0xfe]
    c(len(trk) == 1 and trk[0][2] == 0.80 and trk[0][3] == 0.52, 'MECBYHLX once, 0.80 pan +0.52 (0x1f)', str(trk))
    c(len(lck) == 1 and lck[0][2] == 0.80 and lck[0][3] == -0.50, 'MECXLOCK once, 0.80 pan -0.50 (0x5f)', str(lck))
    if trk and lck and up:
        c(up <= trk[0][0] and 1.5 < lck[0][0] - trk[0][0] < 2.6, 'acquiring after online, the lock ~2 s later', 'up %.2f %s %s' % (up, trk, lck))
    lrm = [x for x in s if x[1] == 189 and x[0] > UP + 5.7]
    c(lrm and lrm[0][2] == 0.40 and lrm[0][3] == 0, 'the LRM (left torso) volley 0.40 centred', str(lrm[:2]))
    keys, hold = [(UP + 2.5, '4')], [(UP + 0.5, UP + 0.6, 'space'), (UP + 1.5, UP + 1.6, 'space')]   # chain fire: weapons 0, 1
    r = c.run('_cockpit', secs=UP + 6, env={'MW2_SFX_LOG': '1'}, keys=keys, hold=hold)
    s = snd(r)
    la = [x for x in s if x[1] == 199]
    c(len(la) >= 2 and la[0][2] == 0.40 and la[0][3] == -0.25 and la[1][2] == 0.40 and la[1][3] == 0.27,
      'cockpit: ER large lasers 0.40, left arm pan -0.25 (0x4f), right arm +0.27 (0x2f)', str(la[:2]))
    fs = [x for x in s if x[1] == 214]
    c(fs and all(x[2] == 0.40 and abs(x[3]) <= 0.1 for x in fs), 'cockpit: footsteps MECFSMED 0.40 about centred (heard from the eye)', str(fs[:3]))
    ext = {'MW2_SFX_LOG': '1', 'MW2_AUTOPILOT': '%g,-9,-999,0,%s,0,0' % (UP + 6, os.path.join(OUT, c.name + '_ext', 'final.ppm'))}
    r = c.run('_ext', secs=UP + 6, env=ext, keys=keys, hold=hold)
    s = snd(r)
    la = [x for x in s if x[1] == 199]
    c(len(la) >= 2 and all(x[2] == 0.80 and abs(x[3]) <= 0.05 for x in la[:2]), 'chase view: the lasers at the mech, 0.80 (the camera behind)', str(la[:2]))
    fs = [x for x in s if x[1] == 214]
    c(fs and all(x[2] == 0.80 for x in fs), 'chase view: footsteps 0.80', str(fs[:3]))
    c(not [x for x in s if x[1] in (0xce, 0xcf, 0xfe)], 'chase view: no MECBSYRX / lock tones (0x1001e6c0 needs the cockpit view)')


@sequence
def archive_grey_topics(c):
    """agent ARC, user report "can't click on anything not white" (Wolf Archive first page): the grey topics are plain
    text, not locked links. FUN_00026b50 opens ARCHWO / ARCHJF (0x7d464 + clan) at entry 1 with nothing from the pilot;
    FUN_00027510 / FUN_0003ca70 make a word a link only when \\aNN marks it (drawn white) - "The Exodus Years", "Codex
    Listing" etc. have no link and no page in the file. DOSBox 2026-10-19 (docs/reference/dos_archive_w15_*.png,
    dos_archive_jf15_*.png, a pilot 15 missions in, Nova Colonel): clicking every grey topic leaves the page unchanged
    (identical to the first page with a new pilot, dos_archive_0.png / dos_archive_jf_0.png), a white one follows its
    link. Also the hidden links on the dots of Wolf "8." and Falcon "4." / "13." (the white dots in DOSBox)"""
    from PIL import Image
    import numpy as np
    shell = os.path.join(os.path.dirname(GLVIEW), 'mw2shell')
    mech2 = os.path.join(os.path.dirname(GAME.rstrip('/')), 'MECH2')
    d = os.path.join(OUT, 'archive_grey_topics')
    os.makedirs(d, exist_ok=True)
    ref = os.path.join(REPO, 'docs', 'reference')
    def y(line):
        return 70 + 12 * line + 5
    # (clan, name, clicks, the expected targets (0-based pages, -1 none), the DOSBox reference after the clicks)
    wolf_grey = [(x, y(l)) for l in (6, 8, 11, 12) for x in (130, 200, 260)]
    jf_grey = [(x, y(l)) for l in (7, 8, 9, 11, 12, 14, 15, 16, 17, 18) for x in (130, 190)]
    for clan, name, clicks, want, refn in (
            (0, 'wolf_grey', wolf_grey, [-1] * len(wolf_grey), 'dos_archive_w15_0.png'),
            (0, 'wolf_link', wolf_grey[:2] + [(140, y(7))], [-1, -1, 17], 'dos_archive_w15_link.png'),
            (1, 'jf_grey', jf_grey, [-1] * len(jf_grey), 'dos_archive_jf15_0.png'),
            (1, 'jf_link', jf_grey[:2] + [(150, y(10))], [-1, -1, 22], 'dos_archive_jf15_link.png'),
            (0, 'wolf_dot', [(96, y(12))], [67], None),
            (1, 'jf_dot4', [(95, y(8))], [66], None),
            (1, 'jf_dot13', [(99, y(17))], [65], None)):
        out = os.path.join(d, name + '.ppm')
        e = dict(os.environ, MW2_SHELL_SCREEN='5', MW2_SHELL_CLAN=str(clan), MW2_SHELL_FRAMES=str(len(clicks) + 3), MW2_SHELL_SHOT=out,
                 SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy', MW2_CONFIG=os.path.join(d, 'x.cfg'), MW2_BITMAP_TEXT='1',
                 MW2_ARCH_CLICK=';'.join('%d,%d' % xy for xy in clicks))
        r = subprocess.run([shell, mech2], env=e, capture_output=True, text=True, timeout=120)
        got = [int(m) for m in re.findall(r'archive click: \d+,\d+ -> (-?\d+)', r.stdout)]
        c(got == want, '%s: clicks -> %s' % (name, want), str(got))
        if refn is None:
            continue
        if not os.path.exists(out):
            c(False, '%s rendered' % name)
            continue
        a = np.array(Image.open(out).convert('RGB')).astype(int)
        b = np.array(Image.open(os.path.join(ref, refn)).convert('RGB')).astype(int)
        diff = np.abs(a - b).max(2) > 6
        diff[45:95, 590:640] = False   # the DOS mouse pointer
        c(int(diff[0:440].sum()) == 0, '%s matches DOSBox (%s)' % (name, refn), '%d px differ' % int(diff[0:440].sum()))


@sequence
def scrub_patch_not_billboard(c):
    """the scrub records' ground patches with colour 0xbxxx (TNW1SCR1 0xbc38, RUSTSCR1 / SABLSCR1 0xbc39) are textured
    polygons lying on the ground (0x10026e80 case 0xb000 -> 0x1002c880: slot 0x100 + low byte, unlit), not colour type 3
    billboards: at the start of the Wolf training missions 1 / 2 (no shots) and RUSTSCN1 the 3D look shows no square
    fire cards standing on the ground (they had been drawn as screen squares of bank-0 bitmap 0xc3, an explosion)"""
    fire = lambda r_, g, b: (r_ > 180) & (g > 70) & (b < 90) & (r_ - b > 120)
    for m in ('TNW1SCN1', 'TNW2SCN1', 'RUSTSCN1'):
        r = c.run('_' + m, secs=1.6, throttle=0, mission=m, nostart=True, size='960x540', shots=[(1.5, 's')])
        n = count_px(r.shots['s'], fire, (0, 0.4, 1, 0.8))
        c(0 <= n < 40, '%s: no fire cards on the ground at the start' % m, 'fire px %d' % n)


@sequence
def stop_and_reverse_momentum(c):
    """user report "stopping is almost instant" (TNJ1 Firemoth, FRM00TRN, on the flat at heading 0). STOP (1) from FULL:
    the throttle servo T 36.2 ticks, the drive servo T 90.5 and the velocity's 1/45 a tick (msim_drive_step) - DOSBox TNJ1
    (SVGA, ~20 fps, readout 150 at heading ~39): 93 / 84 / 74 / 61 / 49 / 40 / 33 / 22 / 12 % at 0.14 / 0.25 / 0.38 /
    0.51 / 0.64 / 0.74 / 0.85 / 1.05 / 1.35 s, 0 at 2.4 s (the port at 30 fps ~0.1 s behind: the engine's per-frame
    steps are larger at DOSBox's frame rate). REVERSE_DIRECTION (BSP / `) from FULL: the throttle is unsigned (input +8);
    the flag only asks for the reverse walk (0x1000b3a0), and the drive turns x -0.5 once that sequence plays (+0x84 == 2,
    0x1001a90a) - DOSBox: 153 kph held 1.7 s while the stride goes run -> walk -> reverse, then 133 / 114 / 93 / 61 / 39
    in 0.35 s and backwards. The port had flipped the drive at the key: stopped and backing within 1 s"""
    env = {'MW2_START': '16441,12000', 'MW2_KPH_TRACE': '1'}
    r = c.run(secs=10, keys=[(1, '0'), (6, '1')], dump='/0.1', nostart=True, mission='TNJ1SCN1', mech=None, env=env)
    full = r.before(5.95)['kmh']
    f = lambda t: r.at(6 + t)['kmh'] / full if full else 0
    c(abs(full - 107.3) < 1, 'FULL: 107.3 km/h true (readout 160 at heading 0)', '%.1f' % full)
    c(f(0.25) > 0.85, 'not instant: still over 85 %% at 0.25 s', '%.2f' % f(0.25))
    c(0.55 < f(0.5) < 0.78 and 0.30 < f(0.75) < 0.55 and 0.17 < f(1.05) < 0.34, 'the DOSBox curve: ~61-68 %% at 0.5 s, ~40-49 %% at 0.75, ~22-29 %% at 1.05',
      '%.2f %.2f %.2f' % (f(0.5), f(0.75), f(1.05)))
    c(f(1.5) < 0.15 and f(3.5) < 0.01, 'stopped by ~2.5 s', '%.2f %.2f' % (f(1.5), f(3.5)))
    glide = math.hypot(r.at(9.9)['x'] - r.at(6.0)['x'], r.at(9.9)['z'] - r.at(6.0)['z'])
    c(1800 < glide < 3000, 'glides ~23 m after STOP', '%d cm' % glide)
    r2 = c.run('_rev', secs=10, keys=[(1, '0'), (6, 'backspace')], dump='/0.1', nostart=True, mission='TNJ1SCN1', mech=None, env=env)
    full = r2.before(5.95)['kmh']
    held = [s['kmh'] for s in r2.span(6.0, 7.4)]
    c(held and min(held) > full - 0.5, 'reverse: FULL speed held while the stride turns (DOSBox 1.7 s)', '%.1f..%.1f' % (min(held), max(held)) if held else '')
    flip = next((s['t'] for s in r2.states if s['t'] > 6 and s['kmh'] < full * 0.9), None)
    c(flip is not None and 7.4 < flip < 8.2, 'the drive turns as the reverse sequence starts (1.4-2.2 s after the key)', str(flip))
    neg = next((s['t'] for s in r2.states if s['t'] > 6 and s['kmh'] < 0), None)
    c(neg is not None and flip is not None and neg - flip < 1.0, 'then backwards within 1 s (DOSBox 0.45 s at ~13 fps)', '%s %s' % (flip, neg))
    c(abs(r2.last['kmh'] + full * 1017 / 1024 / 2) < 3 or r2.last['kmh'] < -45, 'backing at half speed', '%.1f' % r2.last['kmh'])


@sequence
def nav_first_press(c):
    """user report: TNJ1 "N goes to Beta". The player's target selector starts as nothing (+0xdc = 0x1000, 0x100237fe);
    NEXT_NAVPOINT / PREV_NAVPOINT from nothing restart at the first entry going forward (0x1001cca0: step 1), the first
    shown nav point - Nav Alpha (TNJ1's nav 0 "Bay" is not shown). DOSBox TNJ1: the target viewer empty at the start,
    N -> "Nav Alpha 528m", N -> "Nav Beta"; then Shift+N steps back"""
    r = c.run(secs=4.5, keys=[(1, 'n'), (2, 'n'), (3, 'shift+n')], dump='/0.5', nostart=True, mission='TNJ1SCN1', mech=None)
    c(r.at(0.5)['nav'] == -1, 'nothing selected at the start', str(r.at(0.5)['nav']))
    c(r.at(1.5)['nav'] == 1, 'first N: Nav Alpha (nav 1)', str(r.at(1.5)['nav']))
    c(r.at(2.5)['nav'] == 2, 'second N: Nav Beta', str(r.at(2.5)['nav']))
    c(r.at(3.5)['nav'] == 1, 'Shift+N: back to Alpha', str(r.at(3.5)['nav']))
    r2 = c.run('_prev', secs=2, keys=[(1, 'shift+n')], dump='/0.5', nostart=True, mission='TNJ1SCN1', mech=None)
    c(r2.at(1.5)['nav'] == 1, 'Shift+N from nothing: also the first (0x1001cca0 forces step 1)', str(r2.at(1.5)['nav']))


@sequence
def firemoth_readout_heading(c):
    """user question "Fire Moth in training goes 150 kph - correct?": yes. FULL = 10 MP x 105.49 / 64 cm/tick (16.48 x
    182 = 108 km/h true), the readout x 1.5 of the DOS estimate (4 max + mid + min) / 4 of |vx|, |vy|, |vz| (0x32085,
    0x10030330), so it depends on the heading: 160 at 0 / 90 degrees, up to 165 near 14 degrees, down to 143 at 45. DOSBox
    TNJ1 SVGA: 165 a few degrees off the start heading, 150 at ~39 degrees"""
    for hd, want in ((0, 160), (14, 165), (39, 150)):
        r = c.run('_%d' % hd, secs=10, keys=[(1, '0')], dump='/1', nostart=True, mission='TNJ1SCN1', mech=None,
                  env={'MW2_KPH_TRACE': '1', 'MW2_START': '16441,12000', 'MW2_START_HEADING': str(hd)})
        got = kph_at(r, 8.5, 10)
        c(got and min(got) == max(got) == want, 'heading %d: %d kph' % (hd, want), str(sorted(set(got))))


@sequence
def ai_jump_glide(c):
    """an AI mech out of weapons with jets (0x10020880) jumps (manoeuvre 4, 0x1001fdd0: straight up 20 m, legs and
    throttle untouched) then manoeuvre 5 (0x1001fe30): the forward or the backward jet (0x100208e0 sets +0x21 = !+0x20,
    never neither) - a directional jet gives no lift (0x10019310), so it glides at about the top of the jump and moves
    horizontally; done on the ground. The port had left neither jet set while not closing: the jets lifted it straight
    up through the whole manoeuvre (PLUM's Jenner 253 m, FUCH's Nova 225 m in sweep_ai). YELL's Jenner (actor 2)
    disarmed at 12 s, jets kept"""
    env = {'MW2_TEST_DISARM': '12:2', 'MW2_START': '330000,262000', 'MW2_START_HEADING': 180, 'MW2_TEST_RNG': '11.9:1',
           'MW2_TEST_WATCH': '1:2', 'MW2_AI_TRACE': '2'}
    r = c.run(secs=30, env=env, dump='/0.1')
    mans = [(float(m.group(1)), int(m.group(2))) for m in re.finditer(r'^ai ([\d.]+)s actor 2 \w+ man (-?\d+) ', r.out, re.M)]
    t4 = next((t for t, m in mans if m == 4), None)
    t5 = next((t for t, m in mans if m == 5), None)
    c(t4 is not None and t5 is not None and t5 > t4, 'manoeuvre 4 then 5', '4 at %s, 5 at %s' % (t4, t5))
    if t4 is None or t5 is None:
        return
    end5 = next((t for t, m in mans if t > t5 and m != 5), r.states[-1]['t'])
    hs = [(s['t'], s['a_y'] - s['a_ground'], s['a_x'], s['a_z']) for s in r.states if 'a_y' in s and t4 - 0.5 <= s['t'] <= end5 + 1]
    top = max(h for _, h, _, _ in hs)
    c(1900 <= top <= 3500, 'tops out about the jump\'s 20 m (no climb in 5)', 'max %.0f cm' % top)
    g = [(x, z) for t, _, x, z in hs if t5 <= t <= end5]
    moved = math.hypot(g[-1][0] - g[0][0], g[-1][1] - g[0][1]) if len(g) > 1 else 0
    c(moved > 500, 'the directional jet moves it in 5', '%.0f cm' % moved)


def ai_trace(r, actor):
    """MW2_AI_TRACE lines of one actor: [(t, x, z, y, heading, man, speed, rev)] (every 10th step)"""
    pos, out = {}, []
    for m in re.finditer(r'^ai ([\d.]+)s actor %d at (-?\d+),(-?\d+) y (-?\d+) immobile \d+ heading (-?[\d.]+)' % actor, r.out, re.M):
        pos[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5)))
    for m in re.finditer(r'^ai ([\d.]+)s actor %d \w+ man (-?\d+) \(prev -?\d+\) thr [\d.]+( REV)? speed (-?\d+)' % actor, r.out, re.M):
        if m.group(1) in pos:
            x, z, y, h = pos[m.group(1)]
            out.append((float(m.group(1)), x, z, y, h, int(m.group(2)), float(m.group(4)), bool(m.group(3))))
    return out


@sequence
def ai_ground_jets_build_up(c):
    """AI directional jets on the ground (0x10019310: one velocity; the jets firing hold it - no walking approach - and
    the directional thrust and drag act on all of it, terminal 2.5 x the top speed; no lift with a directional jet held).
    YELL's Jenner (actor 2) sent into manoeuvre 11 (0x1001fe10: the LEFT jet, +0xc = 0, 362 ticks) at 10 s: it slides
    left along the ground, its speed building over the 2 s. The port had restarted each grounded frame from the walking
    speed (the thrust never built up) and pushed the side jets along the heading on the ground"""
    env = {'MW2_START': '330000,262000', 'MW2_START_HEADING': 180, 'MW2_TEST_RNG': '9.9:1', 'MW2_TEST_MAN': '10:2:11',
           'MW2_AI_TRACE': '2', 'MW2_TEST_WATCH': '1:2'}
    r = c.run(secs=16, env=env, dump='/0.1')
    tr = ai_trace(r, 2)
    s11 = [e for e in tr if e[5] == 11]
    c(len(s11) >= 5, 'manoeuvre 11 runs ~2 s', '%d trace rows' % len(s11))
    if len(s11) < 5:
        return
    t0, x0, z0, _, h0 = s11[0][:5]
    hr = math.radians(h0)
    lat = []   # displacement to the LEFT of the heading (-cos h, +sin h), cm
    for e in s11:
        lat.append(-(e[1] - x0) * math.cos(hr) + (e[2] - z0) * math.sin(hr))
    steps = [b - a for a, b in zip(lat, lat[1:])]
    c(lat[-1] > 2000, 'slides left over 20 m in the manoeuvre', '%.0f cm (rows %s)' % (lat[-1], ['%.0f' % v for v in lat]))
    c(len(steps) >= 3 and steps[-1] > steps[0] * 1.5, 'its speed builds (thrust accumulates)', ' '.join('%.0f' % s for s in steps))
    ys = [s['a_y'] - s['a_ground'] for s in r.states if 'a_y' in s and t0 <= s['t'] <= s11[-1][0]]
    c(ys and max(ys) < 100, 'no lift with the side jet held', 'max %.0f cm' % (max(ys) if ys else -1))


@sequence
def ai_reverse_momentum(c):
    """the AI's reverse (input +0x2f, manoeuvres 8 / 10) only asks for the reverse sequence (0x1000b3a0 +0x88 = 2); the
    drive turns x -0.5 once it PLAYS (0x1001a90a +0x84 == 2), as the player's (stop_and_reverse_momentum): from a run the
    Jenner keeps moving forward while its stride goes run -> walk -> reverse, then backs. The port had flipped the
    drive at once (from 20 m/s: below zero within 0.7 s). YELL's Jenner (actor 2) running at the player, its first
    manoeuvre 8 (full reverse, ~8.3 s)"""
    env = {'MW2_START': '330000,262000', 'MW2_START_HEADING': 180, 'MW2_AI_TRACE': '2'}
    r = c.run(secs=12, env=env)
    tr = ai_trace(r, 2)
    s8 = [e for e in tr if e[5] == 8]
    c(len(s8) >= 5, 'manoeuvre 8 runs', '%d trace rows' % len(s8))
    if len(s8) < 5:
        return
    t0, v0 = s8[0][0], s8[0][6]
    c(v0 > 500, 'moving forward when the reverse starts', '%.0f cm/s' % v0)
    late = next((e for e in s8 if e[0] >= t0 + 0.6), None)
    c(late is not None and late[6] > v0 * 0.5, 'forward drive kept while the stride turns to the reverse walk',
      '%s: %s' % ('%.2f s' % (late[0] - t0) if late else '-', ' '.join('%.2f:%.0f' % (e[0], e[6]) for e in s8[:8])))
    c(min(e[6] for e in s8) < -200, 'then backs', 'min %.0f cm/s' % min(e[6] for e in s8))




@sequence
def shot_hits_piece_spheres(c):
    """a shot against a unit is tested against its pieces' bounding spheres only (0x10044f60 -> 0x100103e0): each piece is
    a type 6 object (0x10026430 -> 0x1000ffd0(6)) with no test of its own in 0x1024b3a8, so 0x10010650 -> 0x100107b0 takes
    the sphere's entry (0x10029db0: projection - radius; centre the vertex box's middle, radius the farthest vertex,
    0x10029060) as the hit - no polygon test; the nearest piece wins and its location (+0x16, OBJL) takes the damage
    (0x100171d0). YELLSCN1's Nova (actor 0, standing; its polygons end 7.7 m up; hips / torso spheres reach 9.6 m):
    a step 9 and 9.5 m above its feet still strikes it, on the centre torso group (3); 10 m up passes over; 5 m up strikes
    it (the port had tested the polygons: nothing above 7.7 m)"""
    seg = ','.join('9:0:%d:0' % dy for dy in (500, 900, 950, 1000))
    r = c.run(secs=10, env={'MW2_TEST_SHOTSEG': seg, 'MW2_TEST_IMMOBILE': '0:0'})
    res = {int(m.group(4)): (int(m.group(1)), int(m.group(2)), int(m.group(3)))
           for m in re.finditer(r'^testshot (\d) unit (-?\d+) loc (\d+) z -?\d+ \(dy (\d+) dx 0\)', r.out, re.M)}
    c(len(res) == 4, 'four test steps', str(res))
    c(res.get(500, (0,))[0] == 1 and res[500][1] == 0, '5 m up: the Nova struck', str(res.get(500)))
    c(res.get(900) == (1, 0, 3) and res.get(950) == (1, 0, 3), '9 / 9.5 m up (over its polygons): its hips / torso sphere, '
      'the centre torso', '%s %s' % (res.get(900), res.get(950)))
    c(res.get(1000, (1,))[0] == 0, '10 m up: passes over', str(res.get(1000)))


@sequence
def tnj6_slide_round_instructor(c):
    """TNJ6's Mad Dog driven straight ahead at FULL into the standing instructor (TNJ6's Mad Dog at -18364,9502, 148 cm to the
    left of the line; spheres 500 + 500): it creeps round the right of it as TNJ1's Firemoth does (0x1000ba20 projection,
    n x max(1, dv / 2), the drive's 1/45 a tick). DOSBox (MCGA, the readout until back at 79 kph) is frame-rate chaotic:
    4.5-6.2 s at 40-55 fps (cycles 30000-40000), 10.1 s at ~34 fps (22000), 8.0 s at ~27 fps (18000), 6 s at ~17 fps
    (12000) - the "~5 s" of session 15 was a 40 fps reading; the port's 30 fps reference: ~10.4 s. The second try's
    velocity x 2 (0x10019a61, 0x102473d4) is not the gap: the projection lands ON the sphere, so the retest is decided by
    rounding (DOS 0x200d0's integers: inside ~12 % of the time; the 3D editions' floats ~half) - modelled at the 3D
    editions' rate it made TNJ6 6.9 s and TNJ1 6.3 s against DOSBox's 8 s"""
    r = c.run(secs=24, throttle=1, mission='TNJ6SCN1', dump='/0.25')
    slow = [s for s in r.span(8.0, 24.0) if s['kmh'] < 30]
    c(slow and 9.0 < slow[0]['t'] < 10.5, 'stopped by the instructor at ~9.5 s', str([(s['t'], round(s['kmh'])) for s in r.span(9.0, 10.5)]))
    if not slow:
        return
    t0, t1 = slow[0]['t'], slow[-1]['t']
    c(8.0 <= t1 - t0 <= 11.5, 'in contact 8-11.5 s (DOSBox at 27-34 fps: 8.0-10.1 s)', '%.2f..%.2f' % (t0, t1))
    xs = [s['x'] for s in r.span(t0, t1)]
    c(xs and xs[-1] > xs[0] + 500, 'round the instructor to the right (x grows)', '%s -> %s' % (xs[:1], xs[-1:]))
    end = r.at(23.5)
    c(end.get('kmh', 0) > 50 and end.get('z', 0) > 9502 + 3000, 'then on past it at full speed', 'kmh %s z %s' % (end.get('kmh'), end.get('z')))


@sequence
def airborne_press_face_drops(c):
    """an airborne unit pressing a near-vertical face (TAN's cliff, normal y 0.07; Nova jetting 3 s into it, DOS MW2.PRJ):
    each impact sets the velocity to r x |v| x -0.25 (3D 0x1000b5e0 / 0x10247144, DOS 0x1fca0: -(approx |v|) >> 2) - the
    original's own sign: the fall is turned into a slow rise and the push back into the face, both quartered at every
    impact, so the hang lasts a few frames and the unit drops. |v| is the frame's END velocity (0x10019310 adds a t
    before the move: param_1[0x40..0x42]) and the whole vector is held in the air - the part across the heading too (the
    port had used the start-of-frame vy and dropped the across part in the air). After the jets: down on the ground
    within 3.5 s, never rising while falling"""
    r = c.run(secs=UP + 14, mech='nova/nva00std', mission='TAN_SCN1', keys=[(UP, '0')], hold=[(UP + 4, UP + 7, 'j')], dump='/0.1',
              dos_look=True, nostart=True, env={'MW2_START': '-56983,-118833', 'MW2_START_HEADING': '36.9'})
    air = r.span(UP + 7, UP + 13)
    top = max(air, key=lambda s: s['y']) if air else None
    c(top and top['y'] > 1800, 'up the cliff face on the jets', str(top and top['y']))
    if not top:
        return
    fall = [s for s in air if s['t'] >= top['t']]
    down = [s for s in fall if s['y'] <= s.get('gnd', s['y']) + 30]
    c(down and down[0]['t'] - (UP + 7) < 3.5, 'back on the ground within 3.5 s of the jets', str(down[:1]))
    falling = [s for s in fall if not down or s['t'] < down[0]['t']]
    c(all(b['y'] <= a['y'] + 20 for a, b in zip(falling, falling[1:])), 'no hover: never rising while it falls',
      str([(s['t'], s['y']) for s in falling[:8]]))




@sequence
def torso_servo(c):
    """the player's torso servos (0x100190d0: controller +4 twist / +0x14 pitch, T = 0.2 s x 181 = 36.2 ticks; stepped a frame
    by 0x1003b7e0 in 0x10019310; DOS 0x266e0 / 0x49b80: T 36 in 16.16 degrees): the sinks set the demands c[2] / c[6], the
    twist c[3] and pitch c[7] (view, aim, compass band) follow by frame ticks / T of the gap a frame (30 fps: 6.07 / 36.2 =
    0.168). '.' held 1 s: the twist lags the demand and goes on easing after release; Keypad 5 held 0.2 s: the demands 0
    at once, the twist / pitch ease to 0 (0.832 a frame) - DOSBox YELLSCN1 (ZMBV, 10.4-tick frames): the compass band
    38 -> 50 degrees over ~0.4 s after Period's release (the engine's ramp + servo at those frames: 39.8 -> 51), Keypad 5
    held: 31 -> 22 -> 16 -> 12 -> 9 (0.71 = 1 - 10.4 / 36.2 a frame) to 0; a sub-frame tap (xdotool) is not seen at all"""
    t0 = UP
    hold = [(t0, t0 + 1.0, 'period'), (t0 + 0.5, t0 + 0.8, 'up'), (t0 + 2.5, t0 + 2.7, 'kp5')]
    for ed in ('enhanced', 'dos'):
        r = c.run('_' + ed, secs=t0 + 4, throttle=-9, hold=hold, env={'MW2_SINK_TRACE': '1'}, edition=ed)
        sk = []
        for l in r.out.splitlines():
            if l.startswith('SINK '):
                sk.append(dict((k, float(v.split(',')[0])) for k, v in (kv.split('=') for kv in l.split()[1:])))
        if not sk:
            c(False, '%s: SINK trace' % ed)
            continue
        def at(t):
            return next((d for d in sk if d['t'] >= t), sk[-1])
        rel = at(t0 + 1.05)
        c(rel['twist'] < rel['dtw'] - 3, '%s: at the release the twist lags its demand' % ed, '%.2f / %.2f' % (rel['twist'], rel['dtw']))
        c(abs(at(t0 + 2.4)['twist'] - at(t0 + 2.4)['dtw']) < 0.05 and abs(at(t0 + 2.4)['dtw'] - rel['dtw']) < 1e-3,
          '%s: the demand held after release, the twist eased onto it' % ed, '%.2f / %.2f' % (at(t0 + 2.4)['twist'], at(t0 + 2.4)['dtw']))
        k = 1 - (182.0 / 30) / (36.0 if ed == 'dos' else 36.2)
        ease = [d for d in sk if t0 + 2.55 <= d['t'] <= t0 + 3.0]
        ratios = [b['twist'] / a['twist'] for a, b in zip(ease, ease[1:]) if a['twist'] > 0.5 and b['dtw'] == 0]
        c(len(ratios) >= 5 and all(abs(x - k) < 0.01 for x in ratios), '%s: Keypad 5 eases the twist %.3f a frame' % (ed, k),
          ' '.join('%.3f' % x for x in ratios[:8]))
        pr = [b['pitch'] / a['pitch'] for a, b in zip(ease, ease[1:]) if abs(a['pitch']) > 0.5 and b['dpi'] == 0]
        c(len(pr) >= 3 and all(abs(x - k) < 0.01 for x in pr), '%s: and the pitch' % ed, ' '.join('%.3f' % x for x in pr[:8]))
        c(abs(at(t0 + 3.9)['twist']) < 0.05 and abs(at(t0 + 3.9)['pitch']) < 0.05, '%s: fully recentred' % ed,
          '%.3f %.3f' % (at(t0 + 3.9)['twist'], at(t0 + 3.9)['pitch']))


@sequence
def jet_fuel_edition(c):
    """the full jet tank c[0x30] by edition: 3Dfx MW2.DLL 0x712 = 1810 ticks (0x10041beb at load, the refill cap 0x1001a63c,
    the Jets bar 0x1001c783), DOS MW2.EXE 0x71c = 1820 (0x4ce84, 0x27c7d, the bar 0x314b9): a Summoner starts with the
    edition's full tank, J burns it, the refill stops at it"""
    for ed, full in (('enhanced', 1810), ('dos', 1820)):
        r = c.run('_' + ed, secs=UP + 9, mech='summoner/smn00std', edition=ed, hold=[(UP, UP + 1, 'j')], dump='/0.5')
        s0 = r.at(UP - 3)
        c(abs(s0['fuel'] - full) < 0.5, '%s: starts with %d ticks' % (ed, full), '%.1f' % s0['fuel'])
        c(r.at(UP + 1.5)['fuel'] < full - 100, '%s: J burns it' % ed, '%.1f' % r.at(UP + 1.5)['fuel'])
        c(r.last['fuel'] <= full + 1e-3, '%s: the refill capped at %d' % (ed, full), '%.1f' % r.last['fuel'])


@sequence
def target_q_twice(c):
    """DAT_1024c8b0 (DOS 0x96290): 0 at the load, cleared by every cycling key (0x1001cca0: t / r / e / f / n), set by
    TARGET_AT_RETICLE (0x1001d870) only as it ends, left by RESET_TARGETTING (0x1001ce80); while it is 0 0x1001d2d0 lets the
    player take only things flagged 0x400 / 0x1000. So a plain structure needs a second q, or a first one after q / Ctrl+t.
    DOSBox SILVSCN1, the reticle on the Airbase Hanger (878 m): q nothing, q "Airbase Hanger", t "Wolf Marauder", q still
    the Marauder, q the hangar, Ctrl+t nothing, q the hangar. Here ORANSCN1's pump station (structure 12) from 93 m"""
    env = {'MW2_START': '89807,2080', 'MW2_START_HEADING': 0}
    keys = [(10, 'q'), (10.5, 'q'), (11, 't'), (11.5, 'q'), (12, 'q'), (12.5, 'ctrl+t'), (13, 'q')]
    r = c.run(secs=13.6, mission='ORANSCN1', env=env, keys=keys, dump='/0.1')
    seq = [(r.at(t + 0.2)['tgt'], r.at(t + 0.2)['thing']) for t, _ in keys]
    c(seq[0] == (-1, -1), 'the first q of the mission: nothing (a plain structure)', str(seq[0]))
    c(seq[1] == (-1, 12), 'the second q: the pump station', str(seq[1]))
    c(seq[2][0] >= 0 and seq[2][1] == -1, 't: a mech', str(seq[2]))
    c(seq[3] == seq[2], 'q after t: still the mech', str(seq[3]))
    c(seq[4] == (-1, 12), 'q again: the pump station', str(seq[4]))
    c(seq[5] == (-1, -1), 'Ctrl+t: none', str(seq[5]))
    c(seq[6] == (-1, 12), 'q after Ctrl+t: the pump station at once', str(seq[6]))


@sequence
def target_things_cycle(c):
    """t cycles the units, then the things 0x1001d2d0 lets the player take with DAT_1024c8b0 cleared - flagged 0x400 /
    0x1000, not 0x800, not destroyed, within 1750 m unless 0x1000 - then round again (0x1001cca0). DOSBox AMY_SCN1: t x 10
    -> Star1-Mech1, Star1-Mech2, Star2-Mech1, Star2-Mech2, four EngagementSpheres (GT flag 0x400; 1.28k / 593 m / 1.72k /
    1.30k), Star1-Mech1, Star1-Mech2; the port: actors 0-3, structures 11-14, actor 0. YELLSCN1's Chem.Plant (flags 0)
    never comes up"""
    keys = [(20 + 0.5 * i, 't') for i in range(10)]
    r = c.run(secs=25.5, mission='AMY_SCN1', keys=keys, dump='/0.1', nostart=True)
    seq = [(r.at(t + 0.2)['tgt'], r.at(t + 0.2)['thing']) for t, _ in keys]
    units = [x[0] for x in seq[:4]]
    things = [x[1] for x in seq[4:8]]
    c(all(u >= 0 for u in units) and len(set(units)) == 4 and all(x[1] == -1 for x in seq[:4]), 'the four mechs first', str(seq[:4]))
    c(things == [11, 12, 13, 14] and all(x[0] == -1 for x in seq[4:8]), 'then the four engagement spheres (flag 0x400)', str(seq[4:8]))
    c(seq[8] == seq[0] and seq[9] == seq[1], 'then round again', str(seq[8:]))


@sequence
def target_lost_no_marker(c):
    """the HUD's marks follow the player's one selector (+0xdc, 0x10022830 / 0x10022960 / 0x100222e0): a unit, a thing or
    a nav point - and nothing when it is empty (0x1000: at the load 0x100237fe, Ctrl+t, a target lost - the upkeep
    0x1001d510 when it is destroyed). DOSBox: nothing marked after a kill (YELLSCN1 tank, Falcon kills), none at the
    start. Taking a unit replaces a selected nav point. YELLSCN1: nothing at the start; n -> the nav's mark; t -> a mech
    (the nav forgotten); the mech destroyed -> no mark (the port had fallen back to the nav)"""
    r = c.run(secs=14, keys=[(10, 'n'), (11, 't')], env={'MW2_KILL_ACTOR': '0,12.5'}, dump='/0.1')
    c(r.at(9.8)['bracket'] == 0 and r.at(9.8)['nav'] == -1, 'nothing marked at the start', 'bracket %d nav %d' % (r.at(9.8)['bracket'], r.at(9.8)['nav']))
    c(r.at(10.5)['bracket'] in (3, 4) and r.at(10.5)['nav'] >= 0, 'n: the nav point marked', 'bracket %d nav %d' % (r.at(10.5)['bracket'], r.at(10.5)['nav']))
    c(r.at(11.5)['tgt'] == 0 and r.at(11.5)['bracket'] in (1, 4), 't: the mech (actor 0)', 'tgt %d bracket %d' % (r.at(11.5)['tgt'], r.at(11.5)['bracket']))
    c(r.at(13.5)['tgt'] == -1 and r.at(13.5)['bracket'] == 0, 'destroyed: no mark, not the nav point\'s', 'tgt %d bracket %d nav %d' % (r.at(13.5)['tgt'], r.at(13.5)['bracket'], r.at(13.5)['nav']))

@sequence
def camera_listener_sounds(c):
    """positional sounds are heard from the CAMERA (PTR_DAT_1024e1cc position, yaw +0xc with the glance), not from the
    player's mech (0x10032ad0 / 0x10032c10; 3D distance; the vector as the caller makes it - effects 0x10045fca, weapons
    0x10043b59, footsteps / gait 0x1000b370, object loops 0x10032cc0 pass camera - position: their pan is mirrored).
    The chem-plant pipe ~41 m ahead (cockpit_sound_rules): its impacts ~centred; with the view glanced right (Keypad 9,
    +70) they pan hard RIGHT (pan 15 of 127 = +0.77; DOSBox YELLSCN1: LRMs fired straight ahead, glanced right - the
    explosion L 0.095 R 0.435). The in-game menu's sounds are positional at the zero vector (0x10065f6d / 0x100662a9: yaw
    45, not halved): YELLSCN1 heading 158 - pan 111 = -0.75 at 0.80 (DOSBox MECGCLX1 L 0.790 R 0.197); glanced right
    (yaw 228) about centred. The chase view: a restart's MECTURX2 (0x1001ac01) from the camera behind - halved, started
    the camera's distance / 331 m/s late; footsteps late too"""
    snd = lambda r: [(float(t), int(i), float(v), float(d) if d else 0.0, float(p) if p else 0.0) for t, i, v, d, p in
                     re.findall(r'SFXLOG (-?[\d.]+) snd (\d+) vol ([\d.]+)(?: delay ([\d.]+))?(?: pan (-?[\d.]+))?', r.out)]
    env = {'MW2_START': '315212,244208', 'MW2_START_HEADING': 0, 'MW2_SFX_LOG': '1'}
    r = c.run(secs=UP + 6, env=env, keys=[(UP - 0.3, 'q'), (UP, 'q'), (UP + 0.3, 'backslash')],
              hold=pulses(UP + 1, UP + 2, 'space', 1.0, 0.1) + [(UP + 2.5, UP + 6, 'kp9')] + pulses(UP + 3.5, UP + 5, 'space', 1.0, 0.1))
    s = snd(r)
    imp = lambda t0, t1: [x for x in s if x[3] > 0 and t0 < x[0] < t1 and x[1] == 239]   # the lasers' impact effect sound
    a, b = imp(UP + 0.9, UP + 1.5), imp(UP + 3.4, UP + 4.2)
    c(a and all(abs(x[4]) <= 0.1 for x in a), 'impacts straight ahead: about centred', str(a[:3]))
    c(b and all(x[4] >= 0.7 and x[2] < 0.42 for x in b), 'glanced right: the impacts pan right (mirrored), halved', str(b[:3]))
    c(r.at(UP + 3.0)['cam_yaw'] - r.at(UP + 3.0)['heading'] > 60, 'the glance turned the camera', 'cam_yaw %.1f heading %.1f' % (r.at(UP + 3.0)['cam_yaw'], r.at(UP + 3.0)['heading']))
    r = c.run('_menu', secs=16, keys=[(10, 'esc'), (10.5, 'down'), (11, 'enter'), (11.5, 'esc'), (12, 'esc'),
                                      (13.5, 'esc'), (14, 'down'), (14.5, 'esc')], hold=[(13, 15, 'kp9')], env={'MW2_SFX_LOG': '1'})
    m = [(x[1], x[2], x[4]) for x in snd(r) if x[1] in (0xdb, 0xe1)]
    c(len(m) == 3 and m[0] == (0xdb, 0.80, -0.75) and m[1] == (0xe1, 0.80, -0.75), 'menu move / page sounds at 0.80, pan 111 (-0.75)', str(m))
    c(len(m) == 3 and m[2][0] == 0xdb and m[2][1] == 0.80 and abs(m[2][2]) <= 0.1, 'glanced right: the menu sound about centred', str(m))
    ext = {'MW2_SFX_LOG': '1', 'MW2_AUTOPILOT': '%g,-9,-999,0,%s,0,0' % (22, os.path.join(OUT, c.name + '_ext', 'final.ppm'))}
    r = c.run('_ext', secs=22, env=ext, keys=[(UP, '3'), (13, 's'), (15, 's')])
    s = snd(r)
    tu = [x for x in s if x[1] == 0xf7 and x[0] > 14]
    c(len(tu) == 1 and tu[0][2] == 0.40 and 0.05 < tu[0][3] < 0.15, 'chase view: the restart MECTURX2 halved, ~30 m late (~0.09 s)', str(tu))
    fs = [x for x in s if x[1] == 0xd6 and UP + 1 < x[0] < 13]
    c(fs and all(x[2] == 0.80 and 0.05 < x[3] < 0.15 for x in fs), 'chase view: footsteps 0.80, late by the camera distance', str(fs[:3]))


@sequence
def death_sound_order(c):
    """MECTRDXX 0xf6 (0x1001e6c0's state-4 sound) never plays: the HUD update (0x1001e340 / DOS 0x32c86) returns before it
    in states 4 / 5. The death's sound is the death explosion's (effect 0x0d: 0xf6, the same sample as MECEXPX2), positional,
    halved by camera mode 0 (0x1004643b) - and the camera mode is switched by the player's input handler, which runs before
    the unit updates (0x1000908e / 0x10009093): a weapon death and the 3D editions' self-destruct (case 7, seen first by
    the next frame's handler) explode with the camera already external (0.80); the DOS self-destruct (at once, inside the
    unit update) still in the cockpit (DOSBox YELLSCN1 Ctrl+Alt+x: L 0.394 R 0.356 with the voice)"""
    snd = lambda r: [(float(t), int(i), float(v)) for t, i, v in re.findall(r'SFXLOG (-?[\d.]+) snd (\d+) vol ([\d.]+)', r.out)]
    def first_246(r, t0):
        x = [v for t, i, v in snd(r) if i == 0xf6 and t >= t0 - 0.05]
        return x
    r = c.run(secs=14, keys=[(UP, 'ctrl+alt+x')], env={'MW2_SFX_LOG': '1'})
    x = first_246(r, UP)
    c(len(x) == 1 and x[0] == 0.80, 'enhanced self-destruct: one 0xf6 (the explosion), not halved', str(x))
    r = c.run('_dos', secs=14, keys=[(UP, 'ctrl+alt+x')], env={'MW2_SFX_LOG': '1'}, edition='dos')
    x = first_246(r, UP)
    c(len(x) == 1 and x[0] == 0.40, 'DOS self-destruct: the explosion halved (still in the cockpit)', str(x))
    c(r.at(UP + 1)['camode'] == 1, 'DOS self-destruct: then the orbit camera', 'camode %d' % r.at(UP + 1)['camode'])
    r = c.run('_hit', secs=13, env={'MW2_TEST_PHIT': '10:3:11:200', 'MW2_SFX_LOG': '1'})
    x = first_246(r, 9.5)
    c(len(x) == 1 and x[0] == 0.80, 'a weapon death: one 0xf6 (the explosion), not halved', str(x))



@sequence
def scrub_3dfx_alpha(c):
    """RENDERER = 3DFX: the scrub billboards' bank-0 bitmaps are ARGB4444 in the 3Dfx archive (TAN_RK's clear texels are
    0x0fff - teal when read as RGB555). The 3Dfx engine stores no format: Texus reads a CEL as ARGB1555 (0xb) for a
    polygon drawn without flag 2, ARGB4444 (0xc) with it (0x10067240, 0x10066a30), and the billboard always draws with
    flag 2 (0x1002a8e0 -> 0x1002c880 param_8 = 1), alpha-blended. TNJ1: no teal squares on the ground"""
    r = c.run(secs=1.5, mission='TNJ1SCN1', nostart=True, shots=[(1, 'scrub')], size='640x400', look3dfx=True)
    s = r.at(1)
    c(s.get('billboards') == 234, 'TNJ1 (3Dfx): 234 scrub billboards', 'billboards %s' % s.get('billboards'))
    teal = count_px(r.shots.get('scrub'), lambda R, G, B: (B > R + 50) & (G > R + 40) & (B > 100) & (G > 100), (0, 0.45, 1, 0.8))
    c(0 <= teal < 30, 'no teal-bordered squares (the clear texels transparent)', 'teal px %d (the RGB555 reading: ~1400)' % teal)


@sequence
def titanium_textures_off_shade(c):
    """TEXTURES TITANIUM with OBJECT TEXTURES off (MW2SND.CFG +0x14 = 0): the textured kinds 5-7 take the branch of
    kind 2 (mercsw 0x1001b466 for types 0x2000 / 0x5000-0x7000): an object whose type is in the switch mask 0x1009e9a8
    becomes kind 1 - ramp (w >> 8) & 0xf, intensity 0xa0 for a mech (type 0x100) / class 0x50, else 0xd0, the flat shade
    of 0x1002f5e3, unlit (0x1001b47a..0x1001b4f3) - not its texture's average colour. CYANSCN1 from behind the mech
    (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    snd = bytearray(open(os.path.join(GTEST, 'MW2SND.CFG'), 'rb').read())
    snd[0x14:0x18] = b'\0\0\0\0'
    shots = [(2.5, 'view')]
    env = titanium_chase_env('titanium_textures_off_shade', tdir, 3)
    env['MW2_DEBUG_TISHADE'] = '1'
    r = c.run(secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots, env=env, files={'MW2SND.CFG': bytes(snd)})
    env2 = titanium_chase_env('titanium_textures_off_shade_on', tdir, 3)
    env2['MW2_DEBUG_TISHADE'] = '1'
    o = c.run('_on', secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots, env=env2)
    m = re.findall(r'^titanium: (\d+) textured polygons as the kind 1 shade', r.out, re.M)
    mo = re.findall(r'^titanium: (\d+) textured polygons as the kind 1 shade', o.out, re.M)
    c(m and int(m[-1]) > 100, 'OBJECT off: the mech\'s textured polygons drawn as the kind 1 shade', 'lines %s' % m[-1:])
    c(not mo or int(mo[-1]) == 0, 'OBJECT on: none', 'lines %s' % mo[-1:])
    box = (0.35, 0.45, 0.65, 0.85)
    d = mean_diff(r.shots.get('view'), o.shots.get('view'), box=box)
    c(d > 5, 'the mech drawn differently (flat shade, not the textures)', 'mean diff %.1f' % d)
    c.note('shade polygons %s; mech box mean diff %.1f' % (m[-1:] , d))


@sequence
def textures_off_by_class(c):
    """OBJECT TEXTURES off (MW2SND.CFG +0x14 = 0) in the 3D editions: an object of type 0x100 / 0x200 / class 0x50 (the
    mech: the actor layer's type 0) flat in its texture's average (0x1002a768, 0x1002d330). The switch class reaches the
    shader as -class; it was rounded toward zero (int(-1 + 0.5) = 0), so class 1 never switched and class 2 was read as 1.
    CYANSCN1 from behind the mech, against OBJECT on"""
    snd = bytearray(open(os.path.join(GTEST, 'MW2SND.CFG'), 'rb').read())
    snd[0x14:0x18] = b'\0\0\0\0'
    shots = [(2.5, 'view')]
    ap = lambda n: {'MW2_AUTOPILOT': '3,0,-999,0,%s,0,0' % os.path.join(OUT, n, 'final.ppm')}
    r = c.run(secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots, env=ap('textures_off_by_class'),
              files={'MW2SND.CFG': bytes(snd)})
    o = c.run('_on', secs=3, throttle=0, nostart=True, mission='CYANSCN1', shots=shots, env=ap('textures_off_by_class_on'))
    d = mean_diff(r.shots.get('view'), o.shots.get('view'), box=(0.35, 0.45, 0.65, 0.85))
    sky = mean_diff(r.shots.get('view'), o.shots.get('view'), box=(0, 0, 1, 0.3))
    c(d > 1.5, 'OBJECT off: the mech flat (its textures\' averages; 0.0 with the old rounding)', 'mech box mean diff %.1f' % d)
    c.note('mech box mean diff %.1f, sky %.1f' % (d, sky))


@sequence
def titanium_ppc_card_pool(c):
    """TEXTURES TITANIUM: the PPC / gauss cards from the Titanium archive's own MW2_SHT1 pool - mercsw's projectile maker
    takes the first free pool entry of the weapon's kind for every kind (0x1006ca96..0x1006cac3), the 16-bit engine has
    only its own archive: PPCSHOT1 0x3520 / GAS0_0 0x35a0 -> Titanium bank-0 slots 0x52 / 0x5a (the 3D pool: 0x0b /
    0x0d), drawn by mercsw's billboard 0x1004ef4c: its mode-0 corners / UVs and the vertex colour 0xc8 (texels x 200 / 255).
    A Warhawk (4 ER PPC) firing, CYANSCN1 (skipped without the titanium/ folder)"""
    tdir = titanium_dir()
    if not tdir:
        c.note('no titanium/ folder: skipped')
        return
    env = titanium_chase_env('titanium_ppc_card_pool', tdir, 4)
    env['MW2_AUTOPILOT'] = env['MW2_AUTOPILOT'].replace(',0,-999,0,', ',0,-999,1,')
    r = c.run(secs=4, throttle=0, nostart=True, mission='CYANSCN1', mech='warhawk/whk00std', shots=[(2.6, 'ppc')], env=env)
    m = re.search(r'titanium: cards PPC slot 0x(\w+) half (\d+), gauss slot 0x(\w+) half (\d+)', r.out)
    c(m is not None and (m.group(1), m.group(3)) == ('52', '5a'), 'the Titanium pool\'s cards: PPC 0x52, gauss 0x5a', m.group(0) if m else r.out[-300:])
    c('titanium: card drawn from the Titanium pool, bank 0 slot 0x52, billboard 0x1004ef4c tint 0xc8' in r.out,
      'a PPC shot drawn with the Titanium card, its billboard and 0xc8 tint (0x1004f33a)', r.out[-300:])


@sequence
def tod_start_3d_fresh_load(c):
    """the 3D editions' start for INIT +8 = 1570 (CHED, TEAL: radio slot 1 +13) and 6750 (RED_, TAUP: the Glide GrState
    +8): 0, night (slot 8), as a freshly loaded MW2.DLL holds it - and every mission of a play session is a fresh load:
    SimMain (0x10008850) runs its loop while DAT_1024ac64 < 3 and nothing ever sets DAT_1024ac64 back (0 in the file;
    only = 3 / + 1 / + 2 / + 0x29a), so the host must load the DLL again for the next mission"""
    for scene in ('CHEDSCN1', 'TEALSCN1', 'RED_SCN1', 'TAUPSCN1'):
        r = c.run('_' + scene, secs=0.3, mission=scene, nostart=True, env={'MW2_DEBUG_HUDPAL': '1'})
        m = re.search(r'hud palette: \w+ slot (\d+) \(\w+\) start (-?\d+)', r.out)
        c(m is not None and int(m.group(1)) == 8 and int(m.group(2)) == 0, scene + ': start 0, night (slot 8)',
          m.group(0) if m else 'no line')


@sequence
def jets_while_turning(c):
    """the jets firing with the leg input +0xc not 0 (engine 0x100196cc..0x10019711): no lift (vertical acceleration 0,
    -g while rising) and no directional thrust - the turn is input / 1024 x 90 x 2 (0x1001a180, 0x102473d4) with no speed
    term while fuel is left, 0 with the jets held on an empty tank. DOSBox HEIDSCN1 (its own player record HEIDUSER: a
    Nova with jets), standing: Left 1 s turns 79 deg, J + Left 1 s 158 deg with the kph readout 0 and the altitude
    ladder unmoved; J alone climbs (27 / 46 kph). Here a Summoner, ORANSCN1, standing: left arrow alone ~90 deg/s; J +
    left ~180 deg/s on the ground; J alone climbs; J + left while rising: -g as with the jets off"""
    hold = [(UP, UP + 2, 'left'), (UP + 4, UP + 6, 'j'), (UP + 4, UP + 6, 'left'), (UP + 8, UP + 9, 'j'),
            (UP + 9, UP + 11, 'j'), (UP + 9, UP + 11, 'left')]
    r = c.run(secs=UP + 13, mission='ORANSCN1', mech='summoner/smn00std', hold=hold, dump='/0.1')
    def rate(t0, t1):
        d = r.at(t1)['heading'] - r.at(t0)['heading']
        d = (d + 180) % 360 - 180
        return abs(d) / (t1 - t0)
    g0 = r.at(UP - 0.5)['y']
    w, j = rate(UP + 0.5, UP + 1.5), rate(UP + 4.5, UP + 5.5)
    c(80 < w < 95, 'the legs alone: ~90 deg/s', '%.1f' % w)
    c(170 < j < 185, 'jets + legs: doubled, ~180 deg/s', '%.1f' % j)
    c(max(s['y'] for s in r.span(UP + 4, UP + 6.5)) - g0 < 5, 'jets + legs: no lift', '%+.0f cm' % (max(s['y'] for s in r.span(UP + 4, UP + 6.5)) - g0))
    top = r.at(UP + 9)['y']
    c(top - g0 > 200, 'J alone climbs', '%+.0f cm' % (top - g0))
    v0, v1 = r.at(UP + 9.2)['vy'], r.at(UP + 9.7)['vy']
    f0, f1 = r.at(UP + 11.2)['vy'], r.at(UP + 11.7)['vy']   # released, still rising: the planet's -g (ORANSCN1 0.6 g)
    c(v1 < v0 and abs((v1 - v0) - (f1 - f0)) < 0.05, 'then J + legs while rising: -g as with the jets off (no lift)',
      'vy %.2f -> %.2f, released %.2f -> %.2f' % (v0, v1, f0, f1))


@sequence
def jet_refill_dos_rule(c):
    """the refill with the jets released (DOS 0x27c63 / 3Dfx 0x1001a5ad): c[0x30] += the frame's whole ticks >> 2. The
    DOS look at the agreed 30 fps: 182 / 30 = 6.07 ticks a frame -> 1 a frame, 30 a second; the 3D looks unchanged at
    ticks / 4 = 45.5 a second. Summoner, J held 2 s, then the refill over 4 s"""
    for ed, per_s in (('enhanced', 45.5), ('dos', 30.0)):
        r = c.run('_' + ed, secs=UP + 9, mech='summoner/smn00std', edition=ed, hold=[(UP, UP + 2, 'j')], dump='/0.5')
        a, b = r.at(UP + 4)['fuel'], r.at(UP + 8)['fuel']
        k = (b - a) / 4.0
        c(abs(k - per_s) < 1.0, '%s: refills %.1f a second' % (ed, per_s), '%.1f -> %.1f: %.2f/s' % (a, b, k))


@sequence
def target_cycle_unit_rules(c):
    """the unit side of the player's target checks (0x1001d050): a unit powered down (+0x14 bit 0x10: shut down, or
    resting / held by a node) is refused while DAT_1024c8b0 is 0 - after t / r / e / f; the team test is only e's / f's.
    DOSBox YELLSCN1 t x 6 from 22 s: Falcon Nova 1.11k, Falcon Jenner 1.02k, Falcon Firemoth 1.01k, round again - the
    resting Firemoth (table 2, 1.40 km) never (the dropship gone by then); a q on it with the reticle held there: refused
    by the first q (DAT_1024c8b0 0), taken by the second"""
    keys = [(22 + 0.5 * i, 't') for i in range(6)]
    r = c.run(secs=25.2, keys=keys, dump='/0.1', nostart=True)
    seq = [(r.at(t + 0.2)['tgt'], r.at(t + 0.2)['thing']) for t, _ in keys]
    c([x[0] for x in seq] == [0, 2, 3, 0, 2, 3] and all(x[1] == -1 for x in seq), 't: Nova, Jenner, Firemoth, round again (not the resting actor 1)', str(seq))
    r = c.run('_q', secs=4, keys=[(2, 'q'), (2.5, 'q'), (3, 't')], dump='/0.1', nostart=True, env={'MW2_TEST_AIM': '1,30000,0'})
    a, b, t = r.at(2.2)['tgt'], r.at(2.7)['tgt'], r.at(3.2)['tgt']
    c(a == -1 and b == 1, 'q on the resting Firemoth: refused, then taken by the second q', '%d %d' % (a, b))
    c(t != 1 and t >= 0, 't from it: on to the next live unit', str(t))


@sequence
def target_enemy_friendly_sides(c):
    """e (0x1001db60: the nearest of the 0x40008 walk - alliance 1 only; none or beyond 1750 m: the old selector back) and
    f (0x1001db20(1): one step of the 0x20008 walk - the player's side only - not the nearest; none: no target).
    DOSBox TNJ1SCN1 (the neutral Instructor, the resting Timber Wolf): e nothing, f nothing, t the Instructor, e still
    the Instructor, f nothing"""
    keys = [(20, 'e'), (21, 'f'), (22, 't'), (23, 'e'), (24, 'f')]
    r = c.run(secs=24.6, mission='TNJ1SCN1', keys=keys, dump='/0.1')
    seq = [r.at(t + 0.3)['tgt'] for t, _ in keys]
    c(seq == [-1, -1, 1, 1, -1], 'e -, f -, t Instructor, e keeps it, f clears', str(seq))



@sequence
def glance_own_sounds_eye(c):
    """the cockpit camera (the sounds' listener) sits at the eye node: 0x1002e0f0 -> 0x1002eb50 on the EYEO object's world
    matrix (+0x3c) with (0, controller +0xc4, 0); a glance only adds yaw / pitch (0x1002e750; DOS 0x395d0 / 0x39c50) - no
    other pivot. So the player's own near sounds pan by where that eye sits against the unit position (+0x50): the Mad
    Dog's eye is ~2 m BEHIND it, the Timber Wolf's ~2.4 m ahead. DOSBox YELLSCN1 (its player is a Mad Dog), walking at
    throttle 5: footsteps (camera - unit, 0x1000b370) centred; Keypad 9 (PageUp) held: right (R / (L + R) 0.62-0.67),
    Keypad 7 (Home): left (0.32-0.38); a restart's MECTURX2 (unit - camera, 0x1001ac01) under Keypad 9: left, Keypad 7:
    right. The Timber Wolf the other way round"""
    snd = lambda r: [(float(t), int(i), float(p) if p else 0.0) for t, i, v, d, p in
                     re.findall(r'SFXLOG (-?[\d.]+) snd (\d+) vol ([\d.]+)(?: delay ([\d.]+))?(?: pan (-?[\d.]+))?', r.out)]
    for mech, sign in (('maddog/mdg00std', 1), ('timbrwlf/tbr00std', -1)):
        nm = mech.split('/')[0]
        for g, gs in (('kp9', 1), ('kp7', -1)):
            r = c.run('_%s_%s' % (nm, g), secs=UP + 8, mech=mech, env={'MW2_SFX_LOG': '1'}, keys=[(UP, '5')], hold=[(UP + 3, UP + 7, g)])
            s = snd(r)
            fs0 = [x[2] for x in s if x[1] in (214, 215, 216, 217) and x[0] < UP + 3]
            fs = [x[2] for x in s if x[1] in (214, 215, 216, 217) and UP + 3.3 < x[0] < UP + 7]
            c(fs0 and all(abs(p) <= 0.1 for p in fs0), '%s: footsteps centred without a glance' % nm, str(fs0[:3]))
            c(fs and all(p * sign * gs > 0.5 for p in fs), '%s %s: footsteps pan %s' % (nm, g, 'right' if sign * gs > 0 else 'left'), str(fs[:3]))
        r = c.run('_%s_restart' % nm, secs=UP + 10, mech=mech, env={'MW2_SFX_LOG': '1'}, keys=[(UP, 's'), (UP + 4, 'ctrl+s')], hold=[(UP + 3, UP + 9, 'kp9')])
        tu = [x[2] for x in snd(r) if x[1] == 0xf7 and x[0] > UP + 3.5]
        c(len(tu) == 1 and tu[0] * sign < -0.5, '%s kp9: the restart MECTURX2 pans %s' % (nm, 'left' if sign > 0 else 'right'), str(tu))


@sequence
def blkx_without_blk(c):
    """BLKX (0x10024e90) only stores a pending placement; the next BLK processed (0x10024ba0) takes it and resets it, and
    INCL (0x10042c60 -> 0x1003de10) processes the included record at once. PINKSCN1's last chunks are a BLKX (yaw 90, offset
    354960 0 410427) and a second INCL of pinkscri - a record with no BLK (its 9 OBJ are the SCRG scrub patch): nothing
    takes the BLKX, so the record is neither turned nor moved, and its patch is placed at the camera's grid point
    (0x10011570, absolute). The port had moved it 355 / 410 km away - no scrub in PINKSCN1; DOSBox PINKSCN1: scrub
    rocks all round the start"""
    r = c.run(secs=3, mission='PINKSCN1', nostart=True, mech=None, env={'MW2_STATE_DUMP': '1'})
    m = re.findall(r'^SCRG now=\d+ at (-?\d+),(-?\d+) piece (-?\d+),(-?\d+)', r.out, re.M)
    c(m and all(abs(int(a) - int(x)) < 20000 and abs(int(b) - int(z)) < 20000 for a, b, x, z in m),
      'the scrub patch at the camera grid point (not moved by the BLKX)', str(m[:2]))



@sequence
def radar_blip_rules(c):
    """the radar's blips (0x10003960): units but the player with +0x14 & 0x1400 and none of 0x4816 - destroyed (2 / 4) or
    powered down (0x10: shut down, resting / held by a node) are not shown - and the things with flags & 0x1400 (the
    dropships, gunships, spheres, MARO's patrol and hover cars) and none of 0x1e, by side (0x10019040 / 0x10019060).
    DOSBox MAROSCN1 (direct launch): no red blips in the first 30 s - the resting Falcon Nova / Mad Dog at 480 / 590 m are
    not shown - and green blips for the moving cars; the port had shown every armed unit and no things"""
    r = c.run(secs=12, mission='MAROSCN1', mech=None, env={'MW2_RADAR_LOG': '1'})
    m = [(float(t), int(u), int(b)) for t, u, b in re.findall(r'^RADAR t=([\d.]+) units (\d+) things (\d+)', r.out, re.M)]
    c(m and all(u == 1 for t, u, b in m), 'units: only the moving hover limo (the resting enemies hidden)', str(m[:3]))
    c(m and all(b == 6 for t, u, b in m if t < 10), 'things: the six cars (flags 0x400)', str(m[:3]))


@sequence
def bank1_texture_frames(c):
    """the bank-1 frame groups (0x1002cf10 / 0x1002d060 / 0x1002ce50): BMPJ cels gather in the current group until a BMID,
    every BMID takes that group - MAROMAP1's run a9fir201..217 serves 0x9b..0x9f (BSEC rates 10..18 ticks) - frame 0
    first, then a frame every rate ticks, round the group. The port had kept only the first BMID of a run (with the
    run's LAST cel) and drew 0x9c..0x9f untextured: MARO's FIR1_3 (0x8f9e) a grey card, SMO2_2 (0x8fb6) too; 34 such
    slots in every mission's map (fires, smoke, flags). DOSBox MAROSCN1: the fire changes shape shot to shot"""
    r = c.run(secs=3.2, mission='MAROSCN1', mech=None, nostart=True, env={'MW2_ANIM_LOG': '0x9e'})
    a = [(float(t), int(f), int(n), int(rt)) for t, f, n, rt in re.findall(r'^ANIM t=([\d.]+) slot 0x9e frame (\d+) of (\d+) rate (\d+)', r.out, re.M)]
    c(a and all(n == 17 and rt == 16 for t, f, n, rt in a), 'slot 0x9e: 17 frames, 16 ticks each', str(a[:2]))
    c(len(set(f for t, f, n, rt in a)) == 17, 'every frame shown in 3 s (17 x 16 ticks = 1.5 s a round)', str(sorted(set(f for t, f, n, rt in a))))
    c(a and all(f == int(t * 182 / 16 + 1e-6) % 17 or f == (int(t * 182 / 16) - 1) % 17 for t, f, n, rt in a),
      'frame = (ticks / 16) mod 17 from the mission start', str(a[:4]))


TNJ1_NAVS = {1: (-14863, 46816, 3000), 2: (3436, 65623, 3000), 3: (16733, 46814, 3000), 4: (-688, 21962, 2000)}   # x, z, radius (cm)


@sequence
def autopilot_nav_to_nav_and_voice_queue(c):
    """user (2026-10-20): "disabling autopilot puts throttle to 0 - stopped ... you want it to hit nav a, then go to b on
    its own. no stopping" and "I don't hear the autopilot engage / disable until after all of the instructor audio plays".
    Engine 0x10015c00: on arrival - the selected nav's horizontal distance (+0xc0) under its radius (NAVP +0x10, TNJ1 30 m,
    no 60 m minimum) - the selector steps to the next nav (0x1001dac0) and the autopilot walks on at the pilot's throttle;
    only past the last nav is it off with the throttle 0. DOSBox TNJ1: Nav Alpha -> Beta and Gamma -> Delta at 150-165 kph
    without a stop. The voice: "Autopilot" (65) "engaged" (80) is an ordinary queued message (0x10031870 -> 0x10031150,
    priority -1: voice 65 has no record in 0x1024e6a8), and mission radio (0x100318c0, priority 0x50) is put ahead of it,
    also radio queued after the key - DOSBox TNJ1: a during trn1_01S, "Autopilot engaged" after trn1_02S and trn1_03S"""
    r = c.run(secs=92, keys=[(UP, '0'), (UP + 0.5, 'a')], mission='TNJ1SCN1', dump='/0.1',
              env={'MW2_SFX_LOG': '1', 'MW2_SFX_SIMCLOCK': '1', 'MW2_RADIO_TRACE': '1', 'MW2_TEST_PASSIVE': '1'})
    seq, sw = [], []
    for p, q in zip(r.states, r.states[1:]):
        if q['nav'] != p['nav'] and q['nav'] >= 0:
            seq.append(q['nav'])
            if p['nav'] in TNJ1_NAVS:
                x, z, rad = TNJ1_NAVS[p['nav']]
                sw.append((p['nav'], math.hypot(q['x'] - x, q['z'] - z), rad))
    c(seq[:4] == [1, 2, 3, 4], 'Alpha, Beta, Gamma, Delta in turn', str(seq))
    t4 = next((s['t'] for s in r.states if s['nav'] == 4), None)
    run_ = [s for s in r.states if UP + 1 <= s['t'] <= (t4 or 0)]
    c(run_ and all(s['autopilot'] == 1 and s['throttle'] > 0.9 for s in run_), 'never stops or disengages at Alpha, Beta, Gamma',
      str(sorted(set((s['autopilot'], round(s['throttle'], 2)) for s in run_))))
    c(len(sw) >= 3 and all(d < rad + 400 for n, d, rad in sw), 'each nav taken within its own radius (+ one 0.1 s step)',
      ' '.join('%d:%.0fm' % (n, d / 100) for n, d, rad in sw))
    v = [(t, i) for t, i in voice_plays(r) if i in (65, 80)]
    radio = [(float(t), int(n)) for n, t in re.findall(r'radio node (\d+) at (\d+) ms', r.out)]
    pcm = [float(t) * 1000.0 for t in re.findall(r'SFXLOG (-?[\d.]+) pcm \d+ vol [\d.]+ voice', r.out)]
    c(v[:2] and [i for t, i in v[:2]] == [65, 80], '"Autopilot" "engaged" said', str(v))
    if v:
        t65 = v[0][0]
        after_key = [t for t, n in radio if UP * 1000 < t < t65]
        c(after_key, 'radio queued after the key (nav arrivals) ...', str(radio))
        c(t65 > max(pcm), '... and every radio line queued before the voice plays first', 'voice %.0f, radio starts %s' % (t65, pcm))


@sequence
def autopilot_last_nav_and_visited(c):
    """engine 0x10015c00: the autopilot goes to the nav in the selector; arriving at the LAST nav (the step wraps to a lower
    index, 0x10015d4d) turns it off silently with the throttle 0 and nothing selected - DOSBox TNJ1 n x 4 (Nav Delta), 0, a:
    AUTOPILOT and the nav gone on arrival, 165 -> 0 kph. The step to the next nav does not skip visited ones (only the first
    pick, with nothing selected, steps past flag 0x20): with Beta already visited, Alpha -> Beta. All visited: the
    autopilot engages (HUD "AUTOPILOT") but has no nav - no steering, the throttle kept; a leg turn disengages it and the
    throttle stays (0x1001a4dc: only +0xbc cleared; DOSBox TNJ1 Left: 163-165 kph after)"""
    r = c.run(secs=30, keys=[(UP, 'n'), (UP + 0.3, 'n'), (UP + 0.6, 'n'), (UP + 0.9, 'n'), (UP + 1.2, '0'), (UP + 1.5, 'a')],
              mission='TNJ1SCN1', dump='/0.1')
    on = [s for s in r.states if s['autopilot'] == 1]
    c(on and all(s['nav'] == 4 for s in on), 'straight to the selected Nav Delta', str(sorted(set(s['nav'] for s in on))))
    off = next((s for s in r.states if s['t'] > UP + 2 and s['autopilot'] == 0), None)
    c(off is not None and off['nav'] == -1 and off['throttle'] == 0, 'off at Delta, throttle 0, nothing selected', str(off and (off['nav'], off['throttle'])))
    if off:
        x, z, rad = TNJ1_NAVS[4]
        c(math.hypot(off['x'] - x, off['z'] - z) < rad + 400, 'within Delta\'s 20 m', '%.0f m' % (math.hypot(off['x'] - x, off['z'] - z) / 100))
        c(r.at(off['t'] + 4)['kmh'] < 1, 'stopped')
    c(r.msg_time('Autopilot disengaged') is None, 'silently')
    r = c.run('_visited', secs=30, keys=[(UP, '0'), (UP + 0.5, 'a')], mission='TNJ1SCN1', dump='/0.1', env={'MW2_TEST_NAVSEEN': '2'})
    seq = [q['nav'] for p, q in zip(r.states, r.states[1:]) if q['nav'] != p['nav']]
    c(seq[:2] == [1, 2], 'Alpha, then the visited Beta (not skipped to Gamma)', str(seq))
    r = c.run('_allseen', secs=UP + 7, keys=[(UP, '0'), (UP + 0.5, 'a')], hold=[(UP + 4, UP + 4.3, 'left')], mission='TNJ1SCN1',
              dump='/0.1', env={'MW2_TEST_NAVSEEN': '1,2,3,4', 'MW2_START': '655,31181', 'MW2_START_HEADING': -40})   # open ground on the way to Alpha (not the compound)
    a, b = r.at(UP + 1), r.at(UP + 3.9)
    c(a['autopilot'] == 1 and b['autopilot'] == 1 and b['nav'] == -1, 'engaged with no nav', '%d %d nav %d' % (a['autopilot'], b['autopilot'], b['nav']))
    c(abs(a['heading'] - b['heading']) < 0.5 and b['throttle'] > 0.9, 'no steering, throttle kept', '%.1f -> %.1f thr %.2f' % (a['heading'], b['heading'], b['throttle']))
    e = r.at(UP + 6)
    c(e['autopilot'] == 0 and e['throttle'] > 0.9 and e['kmh'] > 80, 'a turn disengages it; the throttle stays', 'ap %d thr %.2f kmh %.0f' % (e['autopilot'], e['throttle'], e['kmh']))


@sequence
def system_keys_text_input(c):
    """port (macOS keys, 2026-10-20 report): SDL text input is off in the simulation (on macOS it sent z + arrows into the
    press-and-hold accent picker); the system's shortcuts (Mission Control's Ctrl+arrows) are switched off while the
    mission is played in front and back on in the MAIN MENU, while paused and at exit; system_shortcuts=allow keeps them
    (the switch is macOS only, the state is logged on every system)"""
    keys = [(10, 'esc'), (11, 'esc'), (12, 'alt+p'), (13.5, 'p')]
    r = c.run(secs=15, keys=keys, hold=[(UP - 1, UP, 'z'), (UP - 0.5, UP, 'left')])   # (the offscreen window has the focus)
    c(re.search(r'^TEXTINPUT active=0$', r.out, re.M) is not None, 'text input off once the window is up', '')
    sk = re.findall(r'^SYSKEYS now=(-?\d+) (on|off) text=(\d)', r.out, re.M)
    c([s for _, s, _ in sk] == ['off', 'on', 'off', 'on', 'off', 'on'], 'shortcuts off playing, on in the MAIN MENU, off, on paused, off, on at exit', str(sk))
    c(len(sk) == 6 and 0 < int(sk[2][0]) - int(sk[1][0]) <= 100 and 0 < int(sk[4][0]) - int(sk[3][0]) <= 100,
      'back off on the first frame after the MAIN MENU / pause (the simulation stood still there)', str(sk))
    c(all(t == '0' for _, _, t in sk), 'text input still off at every switch (z / arrows held)', str(sk))
    cfg = os.path.join(OUT, c.name + '_allow.cfg')
    with open(cfg, 'w') as f:
        f.write('renderer=enhanced\ntextures=original\nsystem_shortcuts=allow\n')
    r2 = c.run('_allow', secs=11, env={'MW2_CONFIG': cfg})
    c(not re.search(r'^SYSKEYS', r2.out, re.M), 'system_shortcuts=allow: never switched off', '')




@sequence
def target_nearest_enemy_no_cycle(c):
    """e (TARGET_NEAREST_ENEMY, 0x1001db60 / DOS 0x2acf0) does not cycle: every press walks the 0x40008 ring one round
    from the first entry (0x1001cca0 with step 0) and takes the nearest alliance-1 unit within 1750 m - armed or not.
    User report 2026-10-20 (Wolf mission 2 "Flame Tongue", ORANSCN1: the two trainees - "unarmed" to the user, but
    ORANENS1 gives them ktf00std / frm00std; they flee - and the Trainer Maddog run at Nav Alpha). DOSBox ORANSCN1 (autopilot to Nav Alpha, stop, e t t t t e e x 3, 1 s apart): e Trainer Maddog, t
    Kitfox, Firemoth, Maddog, Kitfox, e Maddog, Maddog, Firemoth; t Nova, Maddog 354m, Kitfox 384m, Firemoth 307m, e
    Firemoth 319m / 343m / 368m; t Nova 1.54k, Maddog 378m, Kitfox 445m, Firemoth 523m, e Maddog 420m / 435m - e the
    nearest each time, the unarmed trainees included; t cycles all four"""
    seq = ('e t t t t e e ' * 3).split()
    keys = [(10, 'a'), (11, '0'), (40, '1')] + [(43 + i, k) for i, k in enumerate(seq)]
    r = c.run(secs=43 + len(seq) + 0.5, mission='ORANSCN1', keys=keys, dump='/0.1')
    s0 = r.at(42)
    w = [int(x) for x in str(s0['uwpn']).split(';')]
    c(w[0] > 0 and w[1] > 0 and w[2] > 0, 'ORANENS1: the trainees carry their standard loadouts (ktf00std, frm00std) - they flee, not unarmed', str(w))
    es, ts = [], set()
    for i, k in enumerate(seq):
        st = r.at(43 + i + 0.2)
        if k == 't':
            ts.add(st['tgt'])
            continue
        rg = [float(x) for x in str(st['erng']).split(';')]
        cand = [(d, q) for q, d in enumerate(rg) if d >= 0]
        es.append((st['tgt'], min(cand)[1] if cand else -1, ['%.0f' % d for d in rg[:3]]))
    c(es and all(t == n for t, n, _ in es), 'every e takes the nearest enemy (no cycling)', str(es))
    c({0, 1, 2} <= ts, 't cycles the trainer and both unarmed trainees', str(sorted(ts)))
    # near the Training Firemoth's start (76989, 1767): it wakes and is the nearest - e takes the unarmed trainee
    r = c.run('_fm', secs=16, mission='ORANSCN1', keys=[(12 + 0.5 * i, 'e') for i in range(6)], dump='/0.1',
              env={'MW2_START': '74000,-2000'})
    got = [(r.at(12.2 + 0.5 * i)['tgt'], r.at(12.2 + 0.5 * i)['erng']) for i in range(6)]
    near = []
    for t, e in got:
        rg = [float(x) for x in str(e).split(';')]
        cand = [(d, q) for q, d in enumerate(rg) if d >= 0]
        near.append(min(cand)[1] if cand else -1)
    c([g[0] for g in got] == near and 2 in near, 'e from beside the Firemoth: the unarmed trainee, as the nearest', str([(g[0], n) for g, n in zip(got, near)]))


@sequence
def dos_fire_cards_face_camera(c):
    """the fire / smoke cards (world objects of class 0x10 / 0x60: FIRn_m, SMOn_m - one textured quad, colour 0x7f96..0x7fb8
    DOS / 0x8f96.. 3D, bank-1 slots 0x96..0xb8). Two engine rules the port lacked: (1) every world object's model is built
    with its vertices x the OBJ scale (3Dfx 0x10025450 -> 0x100420b0 / 0x10042140, DOS 0x36480 -> 0x4fea0 / 0x4fef0): MARO's
    cards are 4-6 x their model (FIR1_3 scale 4: 54.88 x 48 m); (2) every frame the camera update turns each card about the
    vertical to face the eye, yaw = heading(eye - card) (0x1002ea80, DOS 0x39fa0). DOSBox MAROSCN1 at the start: a big
    orange flame (FIR1_3 0x7f9e, slot 0x9e a9fir201..217) left of the near tan building, over the dark building behind it -
    the port's DOS look drew nothing there (a 13.7 m card, edge-on and back-facing). Checked in DOSBox with patched data
    copies: FIR1_3's winding reversed -> no flame; its OBJ yaw +-27 deg -> unchanged; SMO2_2 moved into the street -> the
    same smoke column in DOSBox and the port"""
    r = c.run(secs=3.0, mission='MAROSCN1', mech=None, nostart=True, dos_look=True, size='640x480', shots=[(2.8, 'card')],
              env={'MW2_FACING_LOG': '1'})
    f = [(n, int(x), int(z), float(ex), float(ez), float(y), int(w), int(h), int(t, 16)) for n, x, z, ex, ez, y, w, h, t in
         re.findall(r'^FACING (\S+) pos=(-?\d+),(-?\d+) eye=(-?\d+),(-?\d+) yaw=(-?[\d.]+) w=(\d+) h=(\d+) type=(0x[0-9a-f]+)', r.out, re.M)]
    c(len(f) == 10 and sum(1 for q in f if q[8] == 0x10) == 6 and sum(1 for q in f if q[8] == 0x60) == 4,
      'MAROWLD1: 6 fire (0x10) and 4 smoke (0x60) cards face the camera', str([(q[0], hex(q[8])) for q in f]))
    fir = [q for q in f if q[0] == 'FIR1_3']
    c(fir and fir[0][6] == 5488 and fir[0][7] == 4800, 'FIR1_3 built at its OBJ scale 4: 5488 x 4800 cm (model 1372 x 1200)', str(fir))
    bad = [(q[0], q[5], round(math.degrees(math.atan2(q[3] - q[1], q[4] - q[2])), 2)) for q in f
           if abs((q[5] - math.degrees(math.atan2(q[3] - q[1], q[4] - q[2])) + 180) % 360 - 180) > 0.1]
    c(f and not bad, 'each card: yaw = the heading of (eye - card), from +z towards +x (0x10030120 table)', str(bad or f[:2]))
    flame = count_px(r.shots['card'], lambda R, G, B: (R > 180) & (G > 60) & (B < 120) & (R - B > 100), (0.60, 0.30, 0.73, 0.62))
    c(flame > 300, 'DOS look: the flame left of the near building (DOSBox MAROSCN1 x 643-736, y 261-440 of 1024 x 768)', str(flame))
    r = c.run('_3d', secs=1.0, mission='MAROSCN1', mech=None, nostart=True, env={'MW2_FACING_LOG': '1'})
    f3 = re.findall(r'^FACING (FIR1_3) .* w=(\d+) h=(\d+)', r.out, re.M)
    c(len(re.findall(r'^FACING ', r.out, re.M)) == 10 and f3 and f3[0][1:] == ('5488', '4800'), '3D editions: the same cards, scale and facing', str(f3))




@sequence
def trainees_start_heading_and_probe(c):
    """ORANSCN1 ("Flame Tongue"): the resting trainees (ORANENS1, formation "pumping") wake when the player comes within
    650 m of oranGOU1 and go to orangoe1 (table 1 node 2, kind 0x100). Two port differences made them run differently
    from DOS: (1) a follower's start heading is its slot's yaw as it stands (0x10009990 -> 0x10018dd0 -> 0x10018ec0: only
    the slot's x / z go through the leader's matrix) - 0 here, the leader -100: the Kitfox and the Firemoth turn RIGHT
    for their goal at ~108 (DOSBox, unit headings through the kph readout: Kitfox 0 at ~6.5 s, then 54 / 66 / 78 / 87;
    Firemoth 36..49), the Maddog left (-201 .. -247); (2) the AI avoidance probe's length is the unit's VELOCITY x 0.12987
    (0x100201c0 -> 0x10030330; DOS 0x2db80), at least 0.3 x 5000 cm - the port had used top speed x throttle, a 40 m probe
    from a standstill that turned the Maddog away from an object 37 m ahead into a 55 m loop west (DOSBox: it heads off
    south-east, x 934 -> 955 m, z 117 -> 69 m). Player stood at (110000, 30000) (within the radius from the start)"""
    env = {'MW2_START': '110000,30000'}
    runs = {}
    for a in (0, 1, 2):
        runs[a] = c.run('_%d' % a, secs=16, mission='ORANSCN1', dump='/0.25', env=dict(env, MW2_WATCH_ACTOR=a))
    hd = {a: runs[a].at(3.0).get('w_hd') for a in runs}
    c(hd[0] == -100.0 and hd[1] == 0.0 and hd[2] == 0.0, 'start headings: the leader -100 (nav), the followers 0 (their slot yaw)', str(hd))
    k9, f9 = runs[1].at(9.0).get('w_hd', 999), runs[2].at(9.0).get('w_hd', 999)
    c(20 < k9 < 110 and 20 < f9 < 110, 'Kitfox and Firemoth turn right (through north) for their goal', 'Kitfox %s Firemoth %s' % (k9, f9))
    m = runs[0].span(5.0, 16.0)
    minx = min(x['w_x'] for x in m) if m else 0
    c(m and minx > 93500 - 1000, 'the Maddog does not loop west (no early avoidance turn from a standstill)', 'min x %s' % minx)
    c(m and m[-1]['w_x'] > 100000 and m[-1]['w_z'] < 6000, 'and heads off east-south-east', 'end %s,%s' % (m[-1]['w_x'], m[-1]['w_z']) if m else '')
    # the trainees seen from Nav Alpha (agent TG's DOSBox: Firemoth 307 m nearest, Maddog 354 m; later Maddog 378, Kitfox
    # 445, Firemoth 523 - the Firemoth passes nearest, then outruns the others)
    keys = [(10, 'a'), (11, '0'), (40, '1')]
    r = c.run('_alpha', secs=60, mission='ORANSCN1', keys=keys, dump='/1')
    rows = []
    for t in range(42, 61):
        rg = [float(x) for x in str(r.at(t + 0.05).get('erng', '-1;-1;-1')).split(';')[:3]]
        rows.append((t, rg))
    fnear = [t for t, g in rows if min(g) >= 0 and g[2] < g[0] and g[2] < g[1]]
    late = rows[-1][1]
    c(fnear, 'the Firemoth is the nearest trainee for a while', str([(t, [round(x / 100) for x in g]) for t, g in rows[::3]]))
    c(late[0] < late[1] < late[2], 'then Maddog < Kitfox < Firemoth', str([round(x / 100) for x in late]))


@sequence
def ai_probe_dos_broad_phase(c):
    """the DOS AI's avoidance probe has the DOS broad-phase carry quirk too (one avoidance function 0x2db80 for the AI and
    the player: 0x2df30 -> 0x1c3f0 -> 0x3a745): a thing is seen from range only when its centre lies ahead in x AND z of
    the probe. DOS look only; the 3D editions keep the float broad phase (0x100201c0 -> 0x10029db0). SILVSCN1's Falcon
    Direwolf walks west (heading -96.9) past an object centred at (-114, 2402): the 3D editions' probe sees it ~39 m off
    and turns left round it; DOSBox (unit 5's x / z / heading through the kph readout): heading -96 / -97 throughout,
    x 69 -> -18 -> -88 m with z 21 -> 9 -> 0 m - straight past, as the port's DOS look"""
    env = {'MW2_WATCH_ACTOR': '4'}
    d = c.run('_dos', secs=27, mission='SILVSCN1', dump='/0.25', env=env, dos_look=True)
    e = c.run('_3d', secs=27, mission='SILVSCN1', dump='/0.25', env=env)
    dh = [x.get('w_hd') for x in d.span(16.0, 27.0)]
    eh = [x.get('w_hd') for x in e.span(16.0, 27.0)]
    c(dh and all(-99.0 < h < -94.0 for h in dh), 'DOS look: straight on at ~-97 (no early turn)', '%s..%s' % (min(dh), max(dh)) if dh else '')
    c(eh and min(eh) < -110.0, '3D look: turns left round the object', 'min %s' % (min(eh) if eh else None))
    end = d.at(26.5)
    c(end.get('w_x', 0) < -4000 and 300 < end.get('w_z', 0) < 1500, 'DOS look: past it on its line (DOSBox x -18 m / z 9 m at ~23-25 s)',
      '%s,%s' % (end.get('w_x'), end.get('w_z')))


@sequence
def hud_layout_wide(c):
    """port (user request 2026-10-21): hud_layout=wide (the default) on a window wider than 4:3 moves the left-hand
    instruments (radar, altitude tape, target display, AUTOPILOT) to the window's left edge and the right-hand ones
    (weapon list, damage display / viewport, throttle bar, kph) to its right edge by the side margin - size and distance
    from the edge as from the 4:3 area's edge -; the compass, reticle and bottom bars stay centred. original: the 4:3
    layout centred. A 4:3 window: no change. The GRAPHICS page's HUD layout item switches it at once and keeps it in
    mw2port.cfg (MW2_HUD_RECTS: the elements' rectangles in window pixels)"""
    def rects(r, layout=None):
        blocks, cur = [], None
        for ln in r.out.splitlines():
            m = re.match(r'^HUDRECTS t=([\d.]+) window (\d+)x(\d+) layout (\w+)', ln)
            if m:
                cur = {'_t': float(m.group(1)), '_layout': m.group(4)}
                blocks.append(cur)
                continue
            m = re.match(r'^HUDRECT (\w+) (-?\d) (-?\d+) (-?\d+) (-?\d+) (-?\d+)', ln)
            if m and cur is not None:
                cur[m.group(1)] = (int(m.group(2)),) + tuple(int(m.group(k)) for k in range(3, 7))
        return [b for b in blocks if layout is None or b['_layout'] == layout]
    got = {}
    for size in ('1920x1080', '2560x1080', '1440x1080'):
        for lay in ('wide', 'original'):
            r = c.run('_%s_%s' % (size, lay), secs=2, size=size, nostart=True,
                      env={'MW2_HUD_RECTS': '1', 'MW2_HUD_LAYOUT': lay, 'MW2_TEST_TARGET': '0'})
            b = rects(r)
            c(b and b[0]['_layout'] == lay, '%s %s: HUD rectangles dumped' % (size, lay), str(b[:1]))
            got[(size, lay)] = b[0] if b else {}
    for size, margin in (('1920x1080', 240), ('2560x1080', 560), ('1440x1080', 0)):
        w, o = got[(size, 'wide')], got[(size, 'original')]
        names = [n for n in o if not n.startswith('_')]
        c(len(names) >= 12 and set(names) == set(k for k in w if not k.startswith('_')), '%s: the same elements in both layouts' % size, str(names))
        bad = []
        for n in names:
            side, ox0, oy0, ox1, oy1 = o[n]
            _, wx0, wy0, wx1, wy1 = w[n]
            want = side * margin
            if abs((wx0 - ox0) - want) > 1 or abs((wx1 - ox1) - want) > 1 or wy0 != oy0 or wy1 != oy1:
                bad.append((n, o[n], w[n]))
        c(not bad, '%s: left-hand elements %d px left, right-hand %d px right, size and height kept, centre ones unmoved' % (size, margin, margin), str(bad))
    o = got[('1920x1080', 'original')]
    c(o and abs((o['compass'][1] + o['compass'][3]) / 2.0 - 960) <= 2 and abs(o['reticle'][1] - 960) <= 1, '16:9: compass and reticle centred', str(o.get('compass')))
    w = got[('1920x1080', 'wide')]
    c(w and abs(w['radar'][1] - got[('1440x1080', 'wide')]['radar'][1]) <= 1 and
      abs((1920 - w['throttle'][3]) - (1440 - got[('1440x1080', 'wide')]['throttle'][3])) <= 1,
      '16:9 wide: radar as far from the left edge and throttle bar from the right edge as at 4:3', '%s %s' % (w.get('radar'), w.get('throttle')))
    # the GRAPHICS page: Esc, 2 (Graphics), 5 (HUD layout: Wide -> Original), 0 (Accept), Esc (close)
    r = c.run('_menu', secs=6, size='1920x1080', nostart=True, keys=[(1, 'esc'), (1.5, '2'), (2, '5'), (2.5, '0'), (3, 'esc')],
              env={'MW2_HUD_RECTS': '1', 'MW2_HUD_LAYOUT': 'wide'})
    b = rects(r)
    lays = [(x['_t'], x['_layout']) for x in b]
    c(b and b[0]['_layout'] == 'wide' and b[-1]['_layout'] == 'original', 'HUD layout on the GRAPHICS page: wide, then original once accepted', str(lays))
    if b and b[-1]['_layout'] == 'original':
        c(b[-1]['radar'][1:] == got[('1920x1080', 'original')]['radar'][1:], 'switched: the radar back in the 4:3 area', str(b[-1]['radar']))
    with open(os.path.join(OUT, c.name + '_menu', 'mw2port.cfg')) as f:
        cfg = f.read()
    c(re.search(r'^hud_layout=original$', cfg, re.M) is not None, 'kept in mw2port.cfg (hud_layout=original)', cfg.replace('\n', ' | '))




@sequence
def htal_bar_scale(c):
    """H T A L (F6; engine 0x1001b230 / 0x1001b890, DOS 0x2f190 / 0x2f790): every bar is armour x half the lower window's
    height (0x8000 of its 80 rows: 40 at 640x480) / the mech's largest STARTING FRONT armour (0x1024c620 / DOS 0x96664),
    truncated, drawn as an inclusive line run (+1 row); the bars' x left to right head, 3, 2, 1, 5, 4, 7, 6 (0x1007c340..).
    DOSBox AQUASCN1, the Mad Dog (largest front 23 x 4, CT / legs), 1024x768: H 26, side torsos / arms 45, CT / legs 65,
    rears 20 px; an ER PPC (17) on the CT -12 px. The port had drawn 1.125 units per MEK point (H 16 px) with every pair
    mirrored. Here two ER PPC hits on location 2 (1-based, the CT's DOS spread partner) at 9.3 s: the RIGHT torso pair
    keeps 45 px, its green part 64 - 34 = 30 x 40 / 92 -> 13 + 1 rows = 21 px"""
    r = c.run(secs=10.5, throttle=0, twist=0, mission='AQUASCN1', edition='dos', mech='maddog/mdg00std', size='1024x768',
              keys=[(9.0, 'f6')], shots=[(10.2, 'h')], env={'MW2_TEST_PHIT': '9.3:2:21:2'})
    w, h, px = ppm(r.shots['h'])
    def bar(x):   # green rows, then yellow / red rows, from the hanging line (y 600 = 375.6 x 1.6) down
        g = y = 0
        for yy in range(598, 680):
            o = (yy * w + x) * 3
            R, G, B = px[o], px[o + 1], px[o + 2]
            if G > 100 and R < 100: g += 1
            elif R > 150 and G > 100 and B < 80 or R > 150 and G < 80: y += 1
        return g, y
    cols = {'H': 856, 'LTf': 871, 'LTr': 877, 'CTf': 887, 'CTr': 893, 'RTf': 903, 'RTr': 909, 'LA': 926, 'RA': 942, 'LL': 962, 'RL': 978}
    b = {k: bar(x) for k, x in cols.items()}
    tot = {k: g + y for k, (g, y) in b.items()}
    c(abs(tot['H'] - 26) <= 1 and abs(tot['CTf'] - 65) <= 1 and abs(tot['LL'] - 65) <= 1 and abs(tot['LTf'] - 45) <= 1
      and abs(tot['LA'] - 45) <= 1 and abs(tot['CTr'] - 20) <= 1, 'full lengths as DOSBox (H 26, CT / legs 65, sides / arms 45, rears 20)', str(tot))
    c(b['LTf'][1] == 0 and b['CTf'][1] == 0, 'the left pair and the CT untouched', str(b))
    c(abs(b['RTf'][0] - 21) <= 2 and abs(tot['RTf'] - 45) <= 1, 'location 2 hit: the RIGHT pair green 64 - 34 = 30 x 40 / 92 -> 21 px, the rest yellow', str(b['RTf']))



@sequence
def walk_keys_hold_airborne(c):
    """user (2026-10-20): "when jump jetting in the air ... the walking animations continue to happen". The engine holds
    the walk keys while the jets fire with fuel left (0x1001a180 0x1001a688 clears object +0x80 bit 0) and while the feet
    end the frame more than the centre height c[0x33] over the ground (0x10019310 0x10019df2; DOS 0x26990 / 0x27785 the
    same); the key player 0x1000dec0 then only sets the bit again: key and pose held, the frame's time dropped, no key
    starts (no footstep), and it plays on after touch-down. DOSBox HEIDSCN1 (HEIDUSER Nova), walking at 6, J held 1 s,
    side camera: the legs keep one pose from the first jet frame to touch-down (21 m up), then walk on. Here the Nova in
    ORANSCN1 walking at 6, J held 1 s; and YELL's disarmed Jenner (actor 2) jumping on its own (ai_jump_glide)"""
    r = c.run(secs=UP + 12, keys=[(UP, '6')], hold=[(UP + 4, UP + 5, 'j')], dump='/0.05', mission='ORANSCN1',
              mech='nova/nva00std', env={'MW2_SFX_LOG': '1'})
    walk = [s['pkey'] for s in r.span(UP + 2, UP + 4)]
    c(len(set(walk)) > 10, 'walking: the keys play', '%d poses' % len(set(walk)))
    held = [s for s in r.span(UP + 4.1, UP + 12) if s.get('aheld')]
    air = [s for s in r.span(UP + 4.1, UP + 12) if s['y'] - s['gnd'] > 600]
    c(air and all(s.get('aheld') for s in air), 'airborne above the centre height: held every frame', '%d / %d' % (len(held), len(air)))
    jet = [s for s in r.span(UP + 4.1, UP + 4.9)]
    c(jet and all(s.get('aheld') for s in jet), 'jets firing (low down too): held', str([round(s['y'] - s['gnd']) for s in jet[:4]]))
    if held:
        keys = set(round(s['pkey'], 3) for s in held)
        c(len(keys) <= 2, 'one pose all through the hold (the frame it begins at most)', str(sorted(keys)))
        t0, t1 = held[0]['t'], held[-1]['t']
        steps = [float(m.group(1)) for m in re.finditer(r'SFXLOG (-?[\d.]+) snd 214\b', r.out)]
        c(steps and not [t for t in steps if t0 + 0.06 < t < t1], 'no footstep in the air', '%s in %.2f..%.2f' % (steps, t0, t1))
        after = [s['pkey'] for s in r.span(t1 + 0.3, t1 + 2.5)]
        c(len(set(after)) > 10, 'landed: the keys play on', '%d poses' % len(set(after)))
        c(steps and [t for t in steps if t > t1], 'landed: footsteps again', str(steps[-3:]))
        low = [s for s in r.span(t1 - 1, t1 + 0.3) if not s.get('aheld') and 1 < s['y'] - s['gnd'] <= 500]
        c(all(not s.get('aheld') for s in low), 'falling within the centre height, jets off: playing', '%d frames' % len(low))
    env = {'MW2_TEST_DISARM': '12:2', 'MW2_START': '330000,262000', 'MW2_START_HEADING': 180, 'MW2_TEST_RNG': '11.9:1',
           'MW2_TEST_WATCH': '1:2', 'MW2_ANIM_TRACE': '2'}
    a = c.run('_ai', secs=30, env=env)
    rows = [(float(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4)), float(m.group(5)), float(m.group(6)))
            for m in re.finditer(r'^anim ([\d.]+)s actor 2 held (\d) vy \S+ fuel \S+ jet (-?\d) y (-?[\d.]+) ground (-?[\d.]+) throttle \S+ key (-?[\d.]+)', a.out, re.M)]
    up = [x for x in rows if x[3] - x[4] > 600]
    c(up and all(x[1] for x in up), 'AI: held above its centre height', '%d rows' % len(up))
    runs, cur = [], []
    for x in rows:
        if x[1]: cur.append(x[5])
        elif cur: runs.append(cur); cur = []
    c(runs and all(len(set(rn[1:])) <= 1 for rn in runs), 'AI: one pose through each hold', str([sorted(set(rn))[:3] for rn in runs][:4]))


@sequence
def dos_sky_banded_smooth(c):
    """port option (user request 2026-10-21) dos_sky=banded | smooth, the DOS renderer only. banded (default) = MW2.EXE:
    the haze band (0x3c000 -> 0x59e77 with dither 0x8000) in the palette's 0xE0-0xEE, each step a flat band and a
    1-pixel checkerboard of it and the next - DOSBox RUSTSCN1 / YELLSCN1 at 1024 x 768 (start pose) matches the port row
    for row (docs: /tmp/SK captures); smooth: the same colours blended - no checkerboard, a monotonic gradient of many
    colours, each flat band's middle row the banded colour; both follow the palette (light amplification's INFRARED slot).
    The GRAPHICS page's first row in a DOS mission is DOS sky (Banded / Smooth), kept in mw2port.cfg (dos_sky=).
    Since session 16 (GD) the option is dos_shading=dithered | smooth (DOS shading: Dithered / Smooth) and covers the
    Gouraud polygons too (dos_shading_dither); dos_sky= is still read"""
    import numpy as np
    def sky(path):   # the sky between the cockpit struts: x 440-600, rows 170-360 of 1024 x 768 (RUST: all band)
        if not path or not os.path.exists(path):
            return None
        w, h, px = ppm(path)
        return np.frombuffer(px, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)[170:360, 440:600].astype(np.int32)
    def checker_rows(a):   # rows of exactly two colours alternating pixel by pixel
        n = 0
        for row in a:
            k = np.unique(row.reshape(-1, 3), axis=0)
            if len(k) == 2 and (row[0::2] == row[0]).all() and (row[1::2] == row[1]).all() and (row[0] != row[1]).any():
                n += 1
        return n
    def flat_mids(a):   # the middle row of each run of 4+ uniform rows
        uni = [(row == row[0]).all() for row in a]
        mids, k = [], 0
        while k < len(uni):
            if uni[k]:
                j = k
                while j < len(uni) and uni[j] and (a[j][0] == a[k][0]).all():
                    j += 1
                if j - k >= 4:
                    mids.append((k + j) // 2)
                k = j
            else:
                k += 1
        return mids
    base = dict(secs=3, throttle=0, nostart=True, mission='RUSTSCN1', size='1024x768', dos_look=True)
    res = {}
    for mode, extra in (('banded', {}), ('smooth', {'MW2_DOS_SKY': 'smooth'}), ('banded_amp', {'MW2_VISION': '1'}),
                        ('smooth_amp', {'MW2_DOS_SKY': 'smooth', 'MW2_VISION': '1'})):
        r = c.run('_' + mode, shots=[(2.6, 'sky')], env=extra, **base)
        res[mode] = sky(r.shots.get('sky'))
        c(res[mode] is not None, mode + ': picture')
    if any(v is None for v in res.values()):
        return
    for tag in ('', '_amp'):
        b, s = res['banded' + tag], res['smooth' + tag]
        nb = len(np.unique(b.reshape(-1, 3), axis=0))
        ns = len(np.unique(s.reshape(-1, 3), axis=0))
        cb, cs = checker_rows(b), checker_rows(s)
        c(3 <= nb <= 15, 'banded%s: the palette steps only (at most 15 colours)' % tag, '%d colours' % nb)
        c(cb >= 40, 'banded%s: the checkerboard rows between the flat bands (as DOS)' % tag, '%d rows' % cb)
        c(cs == 0 and all((row == row[0]).all() for row in s), 'smooth%s: no checkerboard, every row one colour' % tag, '%d checker rows' % cs)
        c(ns >= 3 * nb, 'smooth%s: many more colours than the steps' % tag, '%d vs %d' % (ns, nb))
        col = s[:, 80, :]
        lum = col[:, 0] * 3 + col[:, 1] * 6 + col[:, 2]
        d = np.diff(lum)
        mono = (d >= 0).all() or (d <= 0).all()
        if not mono:   # a hue change with level luminance: per channel, each channel one way
            mono = all((np.diff(col[:, k]) >= 0).all() or (np.diff(col[:, k]) <= 0).all() for k in range(3))
        c(mono, 'smooth%s: a monotonic gradient down the band' % tag, str(col[::20].tolist()))
        mids = flat_mids(b)
        bad = [(m, b[m, 0].tolist(), s[m, 0].tolist()) for m in mids if np.abs(b[m, 0] - s[m, 0]).max() > 3]
        c(len(mids) >= 4 and not bad, 'banded%s / smooth: the same colour in the middle of each flat band' % tag, '%d bands, off: %s' % (len(mids), bad[:3]))
    g, r_ = res['smooth_amp'][..., 1].mean(), res['smooth_amp'][..., 0].mean()
    c(g > r_ and res['smooth'][..., 0].mean() > res['smooth'][..., 1].mean(), 'smooth follows the palette: light amplification turns the sky green',
      'amp R %.0f G %.0f' % (r_, g))
    # the GRAPHICS page in a DOS mission: Esc, 2 (Graphics), 1 (DOS shading: Dithered -> Smooth), 0 (Accept), Esc (close)
    r = c.run('_menu', shots=[(0.8, 'before'), (4.5, 'after')], keys=[(1, 'esc'), (1.5, '2'), (2, '1'), (2.5, '0'), (3, 'esc')],
              **dict(base, secs=5))
    a0, a1 = sky(r.shots.get('before')), sky(r.shots.get('after'))
    c(a0 is not None and checker_rows(a0) >= 40, 'menu: banded before', str(checker_rows(a0) if a0 is not None else None))
    c(a1 is not None and checker_rows(a1) == 0 and len(np.unique(a1.reshape(-1, 3), axis=0)) > 30, 'menu: smooth once accepted',
      str(len(np.unique(a1.reshape(-1, 3), axis=0)) if a1 is not None else None))
    with open(os.path.join(OUT, c.name + '_menu', 'mw2port.cfg')) as f:
        cfg = f.read()
    c(re.search(r'^dos_shading=smooth$', cfg, re.M) is not None, 'kept in mw2port.cfg (dos_shading=smooth)', cfg.replace('\n', ' | '))



@sequence
def ai_fire_line_units(c):
    """the AI's fire decision (0x100231a0 -> 0x10015090, pilots below level 5) tests the line from the unit to its target
    against the whole collision list (0x100103e0, the unit itself skipped): units and world objects together, and only
    the target struck first lets it fire (object +2 bit 0x100 / controller +0xdc bit 0x200: the piece's owner +0x14 is
    the target) - the port had tested the world only, so an AI fired through a friend in its way. AQUASCN1 before the
    attackers move off (~7 s): the Stormcrow (106500, -20000) stands 35 m west of the Gargoyle (110000, -20000); with
    the player 400 m east on that line the Gargoyle stands in the Stormcrow's line, not off it"""
    r = c.run('_line', secs=4, throttle=0, twist=0, mission='AQUASCN1', edition='dos', dump='/1',
              env={'MW2_START': '150000,-20000', 'MW2_TEST_FIRELINE': '3:0,3.1:1'})
    fl = dict((int(m.group(1)), int(m.group(2))) for m in re.finditer(r'^fireline (\d+) (-?\d+)', r.out, re.M))
    c(fl.get(0) == 1, 'the Gargoyle (in front): its line to the player clear', str(fl))
    c(fl.get(1) == 0, 'the Stormcrow behind it: the Gargoyle struck first - no fire', str(fl))
    r = c.run('_off', secs=4, throttle=0, twist=0, mission='AQUASCN1', edition='dos', dump='/1',
              env={'MW2_START': '150000,-8000', 'MW2_TEST_FIRELINE': '3:0,3.1:1'})
    fl = dict((int(m.group(1)), int(m.group(2))) for m in re.finditer(r'^fireline (\d+) (-?\d+)', r.out, re.M))
    c(fl.get(0) == 1 and fl.get(1) == 1, 'the player 120 m off that line: both clear', str(fl))

@sequence
def dos_shading_dither(c):
    """the DOS renderer's Gouraud polygons dithered as MW2.EXE 0x59e77 fills them (session 16, agent GD): vertices
    rounded to whole pixels (0x3dfb7), each edge from its upper vertex's x / c + 0x8000, the span stepped per pixel pair,
    the even pixel (from the span's left end) at + 0x7fff, the odd at + 0, swapped every row from the polygon's top
    (0x3bd6b / 0x5a1d0..0x5a320): a 1-pixel checkerboard of n / n + 1 where the shade's fraction is under 0.5, flat
    n + 1 above, its phase per polygon. DOSBox RUSTSCN1 1024 x 768 at rest (docs/reference/dos_shading_rust_dosbox.png:
    the cockpit's top bar and strut tops, x 395-630, rows 100-170): the port before drew them undithered and darker -
    22 % of the bar's pixels as DOS, now 98 % (the rest: the struts' edges a pixel off). dos_shading=smooth (the port's
    option, not the original): the same mean, blended - no checkerboard"""
    from PIL import Image
    import numpy as np
    ref = np.array(Image.open(os.path.join(REPO, 'docs', 'reference', 'dos_shading_rust_dosbox.png')).convert('RGB')).astype(np.int32)
    def crop(r, name, box=(100, 170, 395, 630)):
        path = r.shots.get(name)
        if not path or not os.path.exists(path):
            return None
        w, h, px = ppm(path)
        return np.frombuffer(px, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3)[box[0]:box[1], box[2]:box[3]].astype(np.int32)
    def checker_runs(a):   # pixels in a run of 6+ that alternate between two colours pixel by pixel, n / n+1 rows apart too
        n = 0
        for row in a:
            same2 = (row[2:] == row[:-2]).all(1) & (row[1:-1] != row[:-2]).any(1)
            run = 0
            for v in same2:
                run = run + 1 if v else 0
                n += run >= 4
        return n
    base = dict(secs=3, throttle=0, nostart=True, mission='RUSTSCN1', mech=None, size='1024x768', dos_look=True)
    rd = c.run('_dithered', shots=[(2.6, 'cp')], **base)
    rs = c.run('_smooth', shots=[(2.6, 'cp')], env={'MW2_DOS_SHADING': 'smooth'}, **base)
    a, b = crop(rd, 'cp'), crop(rs, 'cp')
    c(a is not None and b is not None, 'pictures')
    if a is None or b is None:
        return
    bar = (slice(12, 66), slice(25, 205))   # the top bar: rows 112-166, x 420-600
    agree_bar = (a[bar] == ref[bar]).all(2).mean()
    agree_all = (a == ref).all(2).mean()
    c(agree_bar >= 0.95, 'the top bar pixel for pixel as DOSBox (dither, phase, rounding)', '%.4f (before: 0.22)' % agree_bar)
    c(agree_all >= 0.90, 'bar and strut tops as DOSBox', '%.4f (before: 0.39)' % agree_all)
    ma, mr = a[bar].reshape(-1, 3).mean(0), ref[bar].reshape(-1, 3).mean(0)
    c(np.abs(ma - mr).max() <= 0.5, 'the bar\'s mean colour as DOS (was a palette step darker)', '%s vs %s' % (ma.round(1).tolist(), mr.round(1).tolist()))
    nck = checker_runs(a)
    c(nck >= 3000, 'checkerboards in the cockpit (1-pixel, two colours)', '%d (DOSBox 4969; before 1100: the sky band only)' % nck)
    nsm = checker_runs(b)
    ms = b[bar].reshape(-1, 3).mean(0)
    c(nsm == 0, 'smooth: no checkerboard', str(nsm))
    c(np.abs(ms - ma).max() <= 2.0, 'smooth: the dither\'s mean', '%s vs %s' % (ms.round(1).tolist(), ma.round(1).tolist()))
    # any size: the checker per output pixel (640 x 480: the bar at half the size, still 1-pixel cells)
    r6 = c.run('_640', shots=[(2.6, 'cp')], **dict(base, size='640x480'))
    a6 = crop(r6, 'cp', (72, 102, 265, 372))   # (the bar: 1224 run pixels; before 0)
    c(a6 is not None and checker_runs(a6) >= 500, '640 x 480: 1-pixel checkerboards in the bar', str(checker_runs(a6) if a6 is not None else None))



@sequence
def tab_in_mission_no_reload(c):
    """user report 2026-10-21: Cmd+Tab restarted the mission - the viewer's Tab (next mech, reload) was live in missions.
    Tab in a mission now does nothing: the player keeps walking, the mission clock runs on (no reload back to 0)"""
    r = c.run(secs=UP + 9, keys=[(UP, '0'), (UP + 3, 'tab'), (UP + 4, 'tab')], dump='/0.5')
    a, b = r.at(UP + 2.5), r.at(UP + 8)
    c(a and b and b['t'] > a['t'] + 4.5, 'the mission runs on after Tab (no reload)', '%s -> %s' % (a and a.get('t'), b and b.get('t')))
    c(a and b and abs(b.get('x', 0) - a.get('x', 0)) + abs(b.get('z', 0) - a.get('z', 0)) > 500, 'the player keeps walking (not reset to the start)',
      '%s,%s -> %s,%s' % (a and a.get('x'), a and a.get('z'), b and b.get('x'), b and b.get('z')))


def main():
    global GLVIEW, VERBOSE
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=max(1, min(4, (os.cpu_count() or 2))))
    ap.add_argument('--glview', default=os.environ.get('GLVIEW', GLVIEW))
    ap.add_argument('--only', default='')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    GLVIEW, VERBOSE = os.path.abspath(a.glview), a.v
    if a.list:
        for f in SEQUENCES:
            print('%-36s %s' % (f.__name__, (f.__doc__ or '').split('\n')[0]))
        return 0
    for p in (GLVIEW, os.path.join(GAME, '3d', 'models.prj'), os.path.join(GTEST, 'USERSTAR.BWD')):
        if not os.path.exists(p):
            print('setup missing: %s (MW2_GAME_DIR / MW2_GTEST / --glview)' % p)
            return 2
    sel = [f for f in SEQUENCES if not a.only or f.__name__ in a.only.split(',')]
    os.makedirs(OUT, exist_ok=True)

    def one(f):
        c = Check(f.__name__)
        t0 = time.time()
        try:
            f(c)
        except Abort:
            pass
        except Exception as x:   # a broken check is a failure, not a crash of the suite
            import traceback
            c.fails.append('exception: %r %s' % (x, traceback.format_exc().splitlines()[-3:]))
        return c, time.time() - t0

    t0, nfail = time.time(), 0
    with cf.ThreadPoolExecutor(max_workers=a.j) as ex:
        for c, dt in ex.map(one, sel):
            ok = not c.fails
            nfail += not ok
            print('%s  %-36s %5.1fs' % ('PASS' if ok else 'FAIL', c.name, dt), flush=True)
            for m in c.fails:
                print('      - ' + m)
            for m in c.notes:
                print('      note: ' + m)
            if VERBOSE:
                for r in c.results:
                    print('      log: %s' % os.path.join(OUT, r.name, 'log.txt'))
    print('%d sequences, %d failed, %.0f s' % (len(sel), nfail, time.time() - t0))
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())

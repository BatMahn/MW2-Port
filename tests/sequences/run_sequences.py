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
        f.write('game=%s\nrenderer=enhanced\ntextures=original\n' % d)
    return d


def run(name, secs, throttle=-9, twist=-999, fire=0, jet=0, keys=(), hold=(), env=None, mech='timbrwlf/tbr00std',
        mission='YELLSCN1', shots=(), dump='/0.25', nostart=False, size=None, edition='enhanced', files=None, dos_look=False):
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
    r = c.run(secs=20, env={'MW2_START': '315212,244208', 'MW2_START_HEADING': 0}, keys=[(UP, 'q'), (UP + 0.3, 'backslash')],
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
    r = c.run(secs=14, env={'MW2_START': '315212,244208', 'MW2_START_HEADING': 0, 'MW2_TEST_BLD_KILL': '11:5'}, keys=[(UP, 'q'), (10, 'f11')])
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
    (T 90.5): 42 kph at 0.73 s"""
    r = c.run(secs=18, keys=[(5, '0'), (10, '1'), (14, '0')], dump='/0.1', nostart=True)
    k = lambda t: r.at(t)['kmh'] * 1.5
    c(abs(k(5.7) - 64) < 6, 'first start: 64 kph at 0.7 s', '%.1f' % k(5.7))
    c(abs(k(6.4) - 88) < 4, 'first start: 88 kph at 1.4 s', '%.1f' % k(6.4))
    full = k(9.9)
    c(abs(k(10.5) / full - 0.70) < 0.08 and abs(k(11.1) / full - 0.23) < 0.08, 'stop: 70 % at 0.5 s, 23 % at 1.1 s',
      '%.2f %.2f' % (k(10.5) / full, k(11.1) / full))
    c(abs(k(14.7) - 42) < 6, 'second start (T 90.5): 42 kph at 0.7 s', '%.1f' % k(14.7))


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
    c(r.at(15)['enemies_dead'] == r.before(11.9)['enemies_dead'] + 1 and r.at(15)['enemies_intact'] == 0, 'one more enemy down, as a wreck')
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
    The original: frame 1 MECTURX2 0xf7 (the player's state 0 -> 1, 0x1001ab71); frame 2 the opener line (yell00bS) and
    every non-enemy, non-resting unit's own MECTURX2 at its position (TNJ1: the instructor - not the resting Timber Wolf);
    the palette fades from black from frame 3 over 0x16a ticks in frame steps (2.0 s at 30 fps); at online (state 2)
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
    f0, f1, f2 = r.at(0.05).get('fade'), r.at(1.0).get('fade'), r.at(2.2).get('fade')
    c(f0 is not None and f0 < 0.05 and 0.4 < f1 < 0.6 and f2 == 1.0, 'palette fade from black over 2 s (frame steps)',
      'fade %s %s %s' % (f0, f1, f2))
    r2 = c.run('_tnj1', secs=3, dump='/0.5', env={'MW2_SFX_LOG': '1'}, mission='TNJ1SCN1')
    snd = sounds(r2)
    tu = [x for x in snd if x[2] == 0xf7]
    c(len(tu) == 2 and tu[1][0] - tu[0][0] < 0.1, 'TNJ1: MECTURX2 for the player and the instructor (not the resting Wolf)', str(tu))
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
    r = c.run('_drop', secs=3.5, throttle=1, nostart=True, dump='/0.25', mission='GOLDSCN1',
              env={'MW2_START': '35000,-89000', 'MW2_START_HEADING': 0})
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
    r2 = c.run('_player', secs=9, keys=[(0.5, '0'), (4.0, '1')], nostart=True, dump='/0.25')
    mid = r2.at(4.0)
    c(mid.get('pkey') not in REST, 'player: mid-stride when the stop is pressed', 'key %s' % mid.get('pkey'))
    tail = r2.span(7.0, 9.0)
    c(tail and all(x.get('pkey') == 12.0 for x in tail), "player: the walk ends at its last key (11) -> the standing pose within 3 s",
      'keys %s' % sorted(set(x.get('pkey') for x in tail)))
    moved = [x['pkey'] for x in r2.span(4.0, 7.0)]
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
    yaw) never add up and the AI's eye ray (0x10044950: the eye node's world yaw) points along legs + lagged twist. The
    port had added the keys to the torso: the Gargoyle's ray swung +-4 deg, missing at range, and the player lived on
    (DOS edition, 9 AI seeds: median 70 s, three > 100 s). DOSBox: first damage ~13.5 s (a PPC at ~650 m: RT front plus
    the PPC impact's area damage 0x10046700 on every location), death 33 / 33 / 44 / 44 / 45.5 / 50 / 52.5 s (7 runs);
    the port now 29-60 s, mean 44 (9 seeds). Fire gate (0x100231a0: |bearing off the legs - lagged twist| < 10 deg,
    rand(max(skill, 1) + 1) == 0 every r x 22 ticks), twist servo (0.6 s AI / 0.2 s player, 0x100190d0) and manoeuvres
    unchanged (they match DOS 0x2e860 / 0x266e0 / 0x2c880)"""
    r = c.run(secs=60, throttle=0, twist=0, mission='AQUASCN1', edition='dos', dump='/1',
              env={'MW2_AI_AIM_TRACE': '1', 'MW2_AI_SEED': '3'})
    hits = [float(l.split()[1]) for l in r.out.splitlines() if l.startswith('plhit ')]
    c(hits and 11.0 <= hits[0] <= 17.0, 'first hit at ~13 s from ~650 m (DOSBox ~13.5 s)', '%s' % hits[:3])
    dead = [s['t'] for s in r.states if s.get('dead')]
    # seed 3 (the AI's throttles in the DOS integers, 0x228d0): killed at 33 s by a critical with armour left (40 s with the
    # 3Dfx floats - the outcome is chaotic in such details; 9 seeds: 28-54 s or alive, as before)
    c(dead and 33.0 <= dead[0] <= 55.0, 'dead within 33-55 s (DOSBox 33-52.5 s, mean 43)', '%s' % (dead[:1] or 'alive at 60 s'))
    pre = r.before((dead[0] if dead else 60.0) - 1.0)
    arm = [float(x) for x in str(pre.get('parmor', '')).split(',') if x]
    full = [float(x) for x in str(r.at(1.0).get('parmor', '')).split(',') if x]
    c(len(arm) == 8 and len(full) == 8 and all(a < f for a, f in zip(arm, full)),
      'every location has lost a little (area damage of the PPC impacts, 0x10046700)', '%s -> %s' % (full, arm))
    by_hit = 'PLAYER DESTROYED' in r.out   # else a critical (ammunition, engine) ended it with armour left
    c(not by_hit or (len(arm) == 8 and arm[1] == 0 and arm[2] < full[2] * 0.2), 'the RT armour gone and the CT nearly (DOS: CT hits to RT / CT)', '%s' % arm)
    c(by_hit or (len(arm) == 8 and arm[2] < full[2]), 'a critical ended it: the CT armour struck', '%s' % arm)
    aim = [l.split() for l in r.out.splitlines() if l.startswith('aiaim ')]
    c(aim and all(abs(float(f[19]) - float(f[21])) < 0.3 for f in aim), 'every AI aim ray along legs + lagged twist (no torso sway; the hips\' carried offsets)')


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
            c(1.0 <= h <= 3.5, '12 m: the head ~579 / 512 (half of 579 ticks / 256; more from the wreck\'s later blasts and its ammunition'
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

def node_task_guard_patrol(c):
    """an enemy star's protect node (kind 4) on a thing (0x10014ba0 -> state 7 with the target, 0x10013f00): four 30 m nav
    points at x - R, z - R, x + R, z + R round it (R 250 m for a thing), the goal the first own point in the nav table; on
    arriving the DEFLT state-7 rule (condition 3, ref 0x2a) re-enters patrol with the target (ref2 0x40): every other
    point deleted (0x10013e90 steps on past each deletion), four more made, the goal again the first left - the -z point
    of the first ring next. SILVSCN1: the Gargoyle (actor 3, table 2) guards silvare2 (3050, -1058) for 30 s - the port
    had left kind 4 unapplied (the leader stood, its wingman followed)"""
    r = c.run(secs=34, throttle=0, twist=0, mission='SILVSCN1', dump='/1', nostart=True,
              env={'MW2_WATCH_ACTOR': '3', 'MW2_DEBUG_NODE_TASKS': '1'})
    tasks = [l for l in r.out.splitlines() if l.startswith('node task') and 'kind 0x4 ' in l and 'silvare2' in l]
    c(len(tasks) == 2 and all('-> 7 ' in l for l in tasks), 'both members of table 2 put in state 7 on silvare2', str(tasks))
    pts = [(s['t'], s['w_x'], s['w_z']) for s in r.states if 'w_x' in s and s['t'] <= 31.0]
    d = [((x + 21950) ** 2 + (z + 1058) ** 2) ** 0.5 for t, x, z in pts]
    k = d.index(min(d)) if d else -1
    c(d and min(d) < 3500, 'reaches the first point, x - 250 m (-21950, -1058)', '%.0f cm' % (min(d) if d else -1))
    later = [z for t, x, z in pts[k + 1:k + 5]] if k >= 0 else []
    c(len(later) >= 2 and later[-1] < pts[k][2] - 2000 and all(b < a for a, b in zip([pts[k][2]] + later, later)),
      'then heads for the second, z - 250 m (3050, -26058)', '%s -> %s' % (pts[k][2] if k >= 0 else '-', later))


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

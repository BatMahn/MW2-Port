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
        f.write('game=%s\nedition=enhanced\n' % d)
    return d


def run(name, secs, throttle=-9, twist=-999, fire=0, jet=0, keys=(), hold=(), env=None, mech='timbrwlf/tbr00std',
        mission='YELLSCN1', shots=(), dump='/0.25', nostart=False, size=None, edition='enhanced', files=None):
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
    destroys the location, the machine gun fed by it loses its rounds; a side torso / arm: not destroyed"""
    for suffix, rng, loc, mask in (('_arm', 12347, 5, 0x10), ('_torso', 12354, 2, 0x12)):
        r = c.run(suffix, secs=12, env={'MW2_TEST_RNG': '9.9:%d' % rng, 'MW2_TEST_PHIT': '10:%d:10:60' % loc})
        t = r.msg_time('ammo explosion')
        c(t is not None, suffix[1:] + ': "Internal ammo explosion detected"')
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
    """standing 60 m from the chem plant's Coolant tank: q targets it (TARGET_AT_RETICLE); group-fire volleys destroy it;
    the target is dropped (engine 0x10046a90 clears DAT_1025a664)"""
    r = c.run(secs=20, env={'MW2_START': '315212,244208', 'MW2_START_HEADING': 0}, keys=[(UP, 'q'), (UP + 0.3, 'backslash')],
              hold=pulses(10, 18, 'space', 1.0, 0.1))
    c(r.at(UP + 0.2)['thing'] == 4, 'q targets the structure ahead', 'thing %d' % r.at(UP + 0.2)['thing'])
    d = next((s for s in r.states if s['bld_destroyed'] > 0), None)
    c(d is not None, 'destroyed by gunfire')
    if d:
        c(r.at(d['t'] + 0.5)['thing'] == -1, 'target dropped', 'thing %d' % r.at(d['t'] + 0.5)['thing'])


@sequence
def target_structure_destroyed_hud_off(c):
    """the same with the HUD hidden (F11) when it goes: the target must still be dropped"""
    r = c.run(secs=14, env={'MW2_START': '315212,244208', 'MW2_START_HEADING': 0, 'MW2_TEST_BLD_KILL': '11:4'}, keys=[(UP, 'q'), (10, 'f11')])
    c(r.at(UP + 0.2)['thing'] == 4, 'q targets the structure ahead')
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
    counts = [int(m.group(1)) for m in re.finditer(r'^debris [\d.]+s: (\d+) pieces', r.out, re.M)]
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
    r = c.run(secs=12, env={'MW2_TEST_PHIT': '4:3:4:40,5:3:4:1,6:3:4:1,7:3:4:1,8:3:4:1,9:3:4:1,10:3:4:1,11:3:4:1'}, nostart=True)   # hits from 4 s: no damage before the first start-up (DAT_1024c570) otherwise
    durs = [int(x) for x in re.findall(r'^FLASH now=\d+ dur=(\d+)', r.out, re.M)]
    c(len(durs) >= 4, 'flashes on the breached torso', '%s' % durs)
    c(durs[:4] == [72, 144, 217, 289], 'durations grow with the count', '%s' % durs[:4])


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
    a mech centred in the reticle at 100 m and at 300 m is hit within 30 cm of the reticle point"""
    for k, d in ((0, 10000), (3, 30000)):
        r = c.run('_%d' % d, secs=10, fire=1, env={'MW2_TEST_AIM': '%d,%d,0' % (k, d), 'MW2_AIM_TRACE': '1',
                                                    'MW2_TEST_IMMOBILE': '0:%d' % k, 'MW2_DEBUG_INVULN': '1'})
        hits = [(float(m.group(1)), float(m.group(2))) for m in re.finditer(r'^AIMHIT now=\d+ unit range=(-?\d+) off_deg=[\d.]+ miss_cm=(\d+)', r.out, re.M)]
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
    """throttle presets, reverse, = / - ramp"""
    r = c.run(secs=24, keys=[(UP, '5'), (12, '0'), (15, 'backquote'), (18, '1')], hold=[(20, 21, 'equal')])
    c(abs(r.at(11.5)['throttle'] - 4 / 9) < 0.01, 'preset 5 = 4/9', '%.2f' % r.at(11.5)['throttle'])
    c(r.at(14.5)['throttle'] == 1, 'preset 0 = full')
    c(r.at(17.5)['throttle'] == -1 and r.at(17.5)['kmh'] < 0, 'reverse', 'throttle %.2f kmh %.1f' % (r.at(17.5)['throttle'], r.at(17.5)['kmh']))
    c(r.at(19)['throttle'] == 0, 'preset 1 = stop')
    c(r.at(21)['throttle'] > 0.3, '= ramps the throttle up', '%.2f' % r.at(21)['throttle'])


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
        f.write('game=%s\nedition=enhanced\n' % gd)
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
    c(-87900 < z1 < -87650 and abs(z2 - z1) < 5, 'GOLD: the grounded dropship stops the walk at its box', 'z %s %s' % (z1, z2))
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

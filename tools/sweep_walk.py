#!/usr/bin/env python3
"""Walking sweep (tests only): walk the player along every mission's nav points with the in-game autopilot
and flag stalls, never-arriving / circling, y jumps, falling under the ground, standing inside objects and
climbing onto object tops.

Uses the test-only glview hooks MW2_WALK_TRACE (walk / walkblk / walknav lines), MW2_TEST_PASSIVE (no damage,
no heat), MW2_TEST_NAVSEEN (navs already visited), MW2_TEST_NAVAUTO (autopilot on), MW2_DEBUG_INVULN.

  tools/sweep_walk.py [-j 2] [-o /tmp/walk] [SCENE ...]      (default: every mission in models.prj)

A leg = drop point (or the last nav) -> the next selectable nav the autopilot takes. When a leg stalls the
run is restarted at the stalled leg's nav (MW2_START) with the earlier navs marked visited, so every leg is
walked. Output: <out>/logs/SCENE.N.log, <out>/report.txt (one line per finding).
"""
import argparse, math, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(os.path.dirname(ROOT), 'MW2-game', '3d')

def env_for(out):
    e = dict(os.environ)
    e.update(MW2_CONFIG=os.path.join(out, 'test.cfg'), MW2_INSTALL_DIR=os.environ.get('MW2_INSTALL_DIR', '/tmp/gtest'),
             SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy', MW2_SIZE='160x120',
             MW2_SKYGND=os.path.join(GAME, 'skygnd.par'), MW2_EDITION='enhanced', MW2_NO_STARTUP='1',
             MW2_DEBUG_INVULN='1', MW2_TEST_PASSIVE='1', MW2_WALK_TRACE='1', MW2_TEST_NAVAUTO='1')
    return e

def run(glview, out, scene, secs, tag, start=None, heading=None, seen=None):
    e = env_for(out)
    e['MW2_AUTOPILOT'] = '%d,1,0,0,%s,1' % (secs, os.path.join(out, 'shots', '%s.%s.ppm' % (scene, tag)))
    if start: e['MW2_START'] = '%.0f,%.0f' % start
    if heading is not None: e['MW2_START_HEADING'] = '%.1f' % heading
    if seen: e['MW2_TEST_NAVSEEN'] = ','.join(str(s) for s in sorted(seen))
    log = os.path.join(out, 'logs', '%s.%s.log' % (scene, tag))
    cmd = [glview, os.path.join(GAME, 'models.prj'), os.path.join(GAME, 'textures.prj'), 'ati', '@', scene]
    navs, rows = {}, []
    with open(log, 'w') as f:
        f.write('# ' + ' '.join('%s=%s' % (k, e[k]) for k in ('MW2_START', 'MW2_START_HEADING', 'MW2_TEST_NAVSEEN', 'MW2_AUTOPILOT') if k in e) + '\n')
        f.flush()
        pr = subprocess.Popen(cmd, cwd=ROOT, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        stop = None
        for ln in pr.stdout:
            f.write(ln)
            if stop is not None:
                stop -= 1
                if stop <= 0: break
                continue
            if ln.startswith('walknav '):
                q = ln.split(); navs[int(q[1])] = int(q[4])
            m = WALK.match(ln)
            if m and secs > 5:   # stop early: every selectable nav reached, or 5 s without moving
                g = m.groups(); seen_now = set(int(v) for v in g[10].split(',') if v)
                rows.append((float(g[0]), float(g[1]), float(g[3])))
                if all(k in seen_now or not sl for k, sl in navs.items()) or g[8] == '0' and len(rows) > 4: stop = 4
                old = [r for r in rows if r[0] <= rows[-1][0] - 5.0]
                if old and math.hypot(rows[-1][1] - old[-1][1], rows[-1][2] - old[-1][2]) < 100: stop = 4
        pr.kill(); pr.wait()
    return log

WALK = re.compile(r'^walk ([\d.]+) (-?\d+) (-?\d+) (-?\d+) g (-?\d+) sp (-?\d+) thr (\S+) hd (\S+) ap (\d) sel (-?\d+) seen(\S*) blk (\d) near (-?\d+) (\d+) out (\d) leg (\d)(.*)$')

def parse(log):
    navs, rows, blks, start = {}, [], [], None
    for ln in open(log, errors='replace'):
        if ln.startswith('walknav '):
            p = ln.split()
            navs[int(p[1])] = dict(name=p[2], sel=int(p[4]), x=float(p[5]), z=float(p[6]), r=float(p[8]))
        elif ln.startswith('walkstart '):
            p = ln.split(); start = (float(p[1]), float(p[3]), float(p[5]))
        elif ln.startswith('walkblk '):
            blks.append(ln.strip())
        else:
            m = WALK.match(ln)
            if m:
                g = m.groups()
                rows.append(dict(t=float(g[0]), x=float(g[1]), y=float(g[2]), z=float(g[3]), g=float(g[4]), ap=int(g[8]),
                                 seen=set(int(v) for v in g[10].split(',') if v), near=int(g[12]), nd=float(g[13]),
                                 out=int(g[14]), leg=int(g[15]), extra=g[16].strip()))
    return navs, rows, blks, start

def target_of(navs, seen):
    for k in sorted(navs):
        if navs[k]['sel'] and k not in seen: return k
    return None

def analyse(scene, tag, navs, rows, blks):
    """findings: (kind, t, x, y, z, nav, detail); returns (findings, stall_nav or None)"""
    F = []
    if not rows: return [('norun', 0, 0, 0, 0, None, 'no walk trace')], None
    leg_t0, leg_nav, stall_from = rows[0]['t'], None, None
    reported = set()
    near_t0 = near_nav = None
    for i, r in enumerate(rows):
        tgt = target_of(navs, r['seen'])
        if tgt is None or (r['ap'] == 0 and i > 2): break
        if tgt != leg_nav: leg_nav, leg_t0, stall_from = tgt, r['t'], None
        n = navs[tgt]
        d = math.hypot(n['x'] - r['x'], n['z'] - r['z'])
        # stall: under 1 m moved over the last 3 s
        j = i
        while j > 0 and rows[j - 1]['t'] >= r['t'] - 3.0: j -= 1
        if r['t'] - rows[j]['t'] >= 2.5 and math.hypot(r['x'] - rows[j]['x'], r['z'] - rows[j]['z']) < 100:
            if stall_from is None:
                stall_from = r
                b = [x for x in blks if abs(float(x.split()[1]) - r['t']) < 3.5]
                why = b[-1].split(':', 1)[1].strip() if b else 'no refused step (not a collision stall)'
                if r['nd'] < 1500: why += ' [mech %d at %.0f m]' % (r['near'], r['nd'] / 100)
                F.append(('STALL', r['t'] - 3, r['x'], r['y'], r['z'], tgt, '%s %.0f m to go; %s' % (n['name'], d / 100, why)))
                return F, tgt
        # never arriving: on this leg far longer than the straight walk needs
        if d < 3 * max(n['r'], 6000):
            near_t0 = near_t0 if near_t0 is not None and near_nav == tgt else r['t']; near_nav = tgt
        leg_len = r['t'] - near_t0 if near_t0 is not None and near_nav == tgt else 0
        if leg_len > 30 and ('circ', tgt) not in reported:
            reported.add(('circ', tgt))
            F.append(('CIRCLING', r['t'], r['x'], r['y'], r['z'], tgt, '%s: %.0f s within 3 radii, %.0f m away, radius %.0f m' % (n['name'], leg_len, d / 100, max(n['r'], 6000) / 100)))
        if i > 0:
            p = rows[i - 1]
            if abs(r['y'] - p['y']) > math.hypot(r['x'] - p['x'], r['z'] - p['z']) + 300 and 'yjump%d' % int(r['t']) not in reported:
                reported.add('yjump%d' % int(r['t']))
                F.append(('YJUMP', r['t'], r['x'], r['y'], r['z'], tgt, 'y %.0f -> %.0f in %.1f s (ground %.0f) %s' % (p['y'], r['y'], r['t'] - p['t'], r['g'], r['extra'])))
        if r['y'] < r['g'] - 200 and ('under', int(r['t'] / 10)) not in reported:
            reported.add(('under', int(r['t'] / 10)))
            F.append(('UNDER', r['t'], r['x'], r['y'], r['z'], tgt, 'feet %.0f under ground %.0f' % (r['g'] - r['y'], r['g'])))
        for m in re.finditer(r"(inside\d|on0) part (\d+) '([^']*)' rec (\S+) coll (\d+)", r['extra']):
            key = (m.group(1), m.group(2))
            if key in reported: continue
            reported.add(key)
            F.append(('INSIDE' if m.group(1).startswith('inside') else 'ONTOP', r['t'], r['x'], r['y'], r['z'], tgt,
                      'part %s %s rec %s coll %s' % (m.group(2), m.group(3), m.group(4), m.group(5))))
    return F, None

def sweep(glview, out, scene):
    res, seen, start, heading, n = [], set(), None, None, 0
    # a first short run for the nav list
    navs, rows, _, st = parse(run(glview, out, scene, 2, 'probe'))
    if not rows: return [(scene, 'probe', ('norun', 0, 0, 0, 0, None, 'glview produced no trace'))]
    seen0 = rows[-1]['seen']
    while n < 12:
        # the straight-line route over the remaining selectable navs: time budget
        px, pz = start if start else (st[0], st[2])
        dist, s2 = 0.0, set(seen0) | seen
        while True:
            k = target_of(navs, s2)
            if k is None: break
            dist += math.hypot(navs[k]['x'] - px, navs[k]['z'] - pz); px, pz = navs[k]['x'], navs[k]['z']; s2.add(k)
        if dist == 0 and n > 0: break
        secs = int(min(1500, dist / 800.0 + 40))
        tag = str(n)
        log = run(glview, out, scene, secs, tag, start, heading, seen)
        navs, rows, blks, _ = parse(log)
        f, stall = analyse(scene, tag, navs, rows, blks)
        res += [(scene, tag, x) for x in f]
        if stall is None: break
        # resume at the stalled leg's nav
        seen |= {k for k in navs if k <= stall and navs[k]['sel']} | (rows[-1]['seen'] if rows else set())
        start = (navs[stall]['x'], navs[stall]['z'])
        nxt = target_of(navs, seen | seen0)
        heading = math.degrees(math.atan2(navs[nxt]['x'] - start[0], navs[nxt]['z'] - start[1])) if nxt is not None else None
        n += 1
    if not res: res.append((scene, '-', ('ok', 0, 0, 0, 0, None, 'all %d selectable navs reached' % sum(1 for k in navs if navs[k]['sel']))))
    return res

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=2)
    ap.add_argument('-o', default='/tmp/walk')
    ap.add_argument('--glview', default=os.path.join(ROOT, 'glview'))
    ap.add_argument('scenes', nargs='*')
    a = ap.parse_args()
    for d in ('logs', 'shots'): os.makedirs(os.path.join(a.o, d), exist_ok=True)
    scenes = a.scenes or [l.split()[0] for l in subprocess.run([os.path.join(ROOT, 'bwdtool'), os.path.join(GAME, 'models.prj'), 'missions'],
                                                              capture_output=True, text=True).stdout.splitlines() if re.match(r'^\w+SCN\d ', l)]
    with ThreadPoolExecutor(a.j) as ex, open(os.path.join(a.o, 'report.txt'), 'w') as rep:
        for res in ex.map(lambda s: sweep(a.glview, a.o, s), scenes):
            for scene, tag, (kind, t, x, y, z, nav, det) in res:
                line = '%-9s run %-5s %-8s t %6.1f at %7.0f,%7.0f y %5.0f nav %-4s %s' % (scene, tag, kind, t, x, z, y, nav, det)
                print(line); rep.write(line + '\n'); rep.flush()

if __name__ == '__main__':
    main()

# How the MW2 port is checked against the original (DOSBox method)

This is the method the MW2 port chat uses to confirm placement, timing and looks against the original DOS MechWarrior 2. It applies to any port of the same game, including a classic Mac OS one. The rule behind it: **the original is the reference.** You either trace a value in the original code or measure it in DOSBox, and you never tune it by eye until it "looks right".

---

## 1. What you need

| Item | Notes |
|---|---|
| `dosbox` (0.74-3 is what we used) | Plus `dosbox-debug` (same version, with the debugger) for memory reads (section 8). |
| `Xvfb`, `xdotool`, `imagemagick` (`import`), `ffmpeg` (optional) | Runs DOSBox headless and drives it. |
| `python3` + Pillow | Crops and pixel diffs. |
| The DOS MW2 CD image (`.cue`/`.bin`) and an installed DOS copy | The user supplies these. **Never distribute or commit them.** |

```sh
apt-get install -y dosbox dosbox-debug xvfb xdotool imagemagick ffmpeg python3-pil
```

**Always work on a copy of the install.** For example, use `/tmp/mw2dos` and never the user's original folder. Some experiments patch `MW2.EXE` or rewrite pilot and career files, so give every experiment its own copy (`/tmp/POSE_mw2dos`, `/tmp/LOCK_mw2dos`, ...) and its own conf.

## 2. The DOSBox conf

Launch the mission directly. The `MW2 <SCENE>` command line skips the shell, the pilot screens and the movies.

```ini
[sdl]
fullscreen=false
output=surface
autolock=false
sensitivity=100
[dosbox]
machine=svga_s3
memsize=32
[cpu]
core=dynamic
cycles=max
[mixer]
nosound=true
[sblaster]
sbtype=none
[gus]
gus=false
[speaker]
pcspeaker=false
[autoexec]
mount c /tmp/mw2dos
imgmount d "/path/to/Mechwarrior2-31stCenturyCombat-MSDOS.cue" -t iso
c:
cd \
MW2 YELLSCN1
```

Notes on these settings:

- **`machine=svga_s3`.** The game's 640x480 and 1024x768 modes need VESA SVGA.
- **`output=surface`.** The capture then holds the game's real pixels, with no scaling or filtering. Don't use `opengl`/`openglnb` output, and don't use a `scaler` other than none or normal.
- **`cycles=max`** keeps it fast, but DOSBox still runs the game **5-16 % slower than real time**. Section 5 covers why this matters.
- **Sound is off.** It isn't needed for visual checks, and leaving it off avoids audio-timing stalls. For sound checks, use a separate conf with SB16/GUS turned on.
- **The mission name is the scene file without `.BWD`.** For example: `YELLSCN1` (Mission 1, the Wolf Dropship one), `TAUPSCN1` (the dropship flying over the hills), `MAROSCN1` and `TNJ1SCN1`.
- **The player mech** is whatever the install's `USERSTAR.BWD` holds. Ours is a **Mad Dog**. Use the same mech in the port, or the eye height and cockpit won't match.
- **The resolution** is the game's own setting, saved in the install copy. Keep one install copy per resolution (one at 640x480, one at 1024x768) instead of switching modes inside a run. Before you trust a capture, check its content bounds (section 4).

## 3. The harness (headless DOSBox with a tiny script language)

Save this as `dosrun.sh`. It starts Xvfb, starts DOSBox at the window's top-left corner, then runs a step file line by line.

```bash
#!/bin/bash
# usage: dosrun.sh script   (lines: sleep N | key K | down K | up K | type TEXT | click X Y | hover X Y | rmove DX DY | lclick | reset | shot NAME)
export DISPLAY=:99
Xvfb :99 -screen 0 1280x1024x24 >/tmp/xvfb.log 2>&1 &
XP=$!
sleep 2
SDL_VIDEO_WINDOW_POS=0,0 SDL_AUDIODRIVER=dummy dosbox -conf ${DOSCONF:-/tmp/dosbox.conf} >/tmp/dosbox.log 2>&1 &
DP=$!
sleep 3
W=$(xdotool search --name DOSBox | head -1)
xset m 1 1 2>/dev/null; xset m 1/1 0 2>/dev/null   # no pointer acceleration
while read -r cmd a b; do
  case "$cmd" in
    sleep) sleep "$a" ;;
    key) xdotool key --window "$W" "$a" ;;
    down) xdotool keydown --window "$W" "$a" ;;
    up) xdotool keyup --window "$W" "$a" ;;
    type) xdotool type --delay 60 --window "$W" "$a" ;;
    click) xdotool mousemove --window "$W" 320 240; for i in 1 2 3 4 5 6; do xdotool mousemove_relative -- -200 -200; sleep 0.05; done
           sleep 0.2; X=$(( a * 246 / 100 )); Y=$(( b * 210 / 100 ))
           while [ $X -gt 0 ] || [ $Y -gt 0 ]; do dx=$(( X > 40 ? 40 : X )); dy=$(( Y > 40 ? 40 : Y )); xdotool mousemove_relative -- $dx $dy; X=$((X-dx)); Y=$((Y-dy)); sleep 0.03; done
           sleep 0.3; xdotool click 1 ;;
    hover) xdotool mousemove --window "$W" 320 240; for i in 1 2 3 4 5 6; do xdotool mousemove_relative -- -200 -200; sleep 0.05; done
           sleep 0.2; X=$(( a * 246 / 100 )); Y=$(( b * 210 / 100 ))
           while [ $X -gt 0 ] || [ $Y -gt 0 ]; do dx=$(( X > 40 ? 40 : X )); dy=$(( Y > 40 ? 40 : Y )); xdotool mousemove_relative -- $dx $dy; X=$((X-dx)); Y=$((Y-dy)); sleep 0.03; done ;;
    shot) import -window root "/tmp/ds_$a.png" ;;
    reset) for i in 1 2 3 4 5 6 7 8; do xdotool mousemove_relative -- -100 -100; sleep 0.03; done ;;
    rmove) X=$a; Y=$b; while [ $X -ne 0 ] || [ $Y -ne 0 ]; do dx=$(( X > 20 ? 20 : (X < -20 ? -20 : X) )); dy=$(( Y > 20 ? 20 : (Y < -20 ? -20 : Y) )); xdotool mousemove_relative -- $dx $dy; X=$((X-dx)); Y=$((Y-dy)); sleep 0.02; done ;;
    lclick) xdotool click 1 ;;
    rec) mkdir -p /tmp/rec/$a; ffmpeg -nostdin -loglevel error -y -f x11grab -framerate ${RECFPS:-20} -video_size 1024x768 -i :99.0+0,0 -c:v png /tmp/rec/$a/f%04d.png & RP=$! ;;
    stoprec) kill -INT $RP; sleep 1 ;;
  esac
done < "$1"
xdotool getwindowgeometry "$W" > /tmp/dosgeom.txt
kill $DP $XP 2>/dev/null
```

Run a step file under a lock, because two runs can't share display `:99`:

```sh
flock /tmp/dosbox.lock ./dosrun.sh yell.ds
DOSCONF=/tmp/POSE_dosbox.conf flock /tmp/dosbox.lock ./dosrun.sh pose.ds
```

Each `shot NAME` saves the whole 1280x1024 root window to `/tmp/ds_NAME.png`.

### Step language

| Step | Effect |
|---|---|
| `sleep N` | Waits N wall seconds (fractions are allowed). |
| `key K` | Taps a key (xdotool names: `e`, `space`, `Escape`, `F12`, `ctrl+s`, `backslash`). |
| `down K` / `up K` | Holds and releases a key, e.g. for a timed throttle or a held trigger. |
| `type TEXT` | Types a string, for shell text fields. |
| `click X Y` / `hover X Y` | Clicks or points at a position in **game 640x480 coordinates**. The pointer is first pushed to 0,0, then moved in small relative steps. The `*246/100` and `*210/100` factors convert game units to the mickeys DOSBox's mouse needs. Calibrate them once for your setup if clicks land off. |
| `rmove DX DY` / `lclick` / `reset` | Raw relative mouse movement, a click, and a push back to the corner. |
| `shot NAME` | Takes a screenshot. |
| `rec NAME` / `stoprec` | Records a frame series for timing work (x11grab). |

### Example: the YELLSCN1 opening (dropship)

```
sleep 22
shot y_a
sleep 1
shot y_b
sleep 1
shot y_c
sleep 2
shot y_d
sleep 3
shot y_e
key e
sleep 1
shot y_f
```

The first ~20 s of wall time are DOSBox boot, DOS4GW and the mission load. Take the first `shot` a little early and check that the HUD is up (section 5).

## 4. Capture geometry and cropping

- DOSBox sits at the top-left of the 1280x1024 Xvfb screen, so **the game image starts at 0,0**. At 1024x768 the content box is 0,0-1023,767. At 640x480 it's 0,0-639,479. In 320x200 modes the window is 640x400, scaled 2x by DOSBox.
- Crop before comparing, and check that the box is what you expect. A wrong box means the wrong mode, or a window that wasn't placed at 0,0.

```python
from PIL import Image
im = Image.open('/tmp/ds_y_a.png').convert('RGB')
print(im.getbbox())              # sanity check: the non-black content box
dos = im.crop((0, 0, 1024, 768)) # or (0, 0, 640, 480)
dos.save('dos_y_a.png')
```

- Keep the reference frames you rely on, under docs/reference/ in your repo, named like `dos_<what>_<mission>_<res>.png`. Then later fixes can be checked against the same picture without rerunning DOSBox.

## 5. Timing: calibrate in game time, not wall time

DOSBox runs the game slower than real time. With `cycles=max` on our container, it ran 5-16 % slow, and the amount varies from run to run. A shot "at 3 s" by the wall clock is not 3 s of game time, so **never match frames by `sleep` totals**. Match them by events in the game itself:

- **The HUD power-up.** The HUD draws in over the first seconds of a mission, so the first frame with the full HUD makes a good t=0 marker. Don't compare HUD pixels before it has finished powering up.
- **The mission clock** on the objectives screen (F12 in DOS, where the build has it), or any message line with a known trigger time.
- **Moving things:** compare a moving object's position along its path, not "the frame at N seconds".
- The engine runs at **182 ticks/s**, and path segments are in ticks. Turn the port's tick counter into the same "where along the path" figure, and compare that.

For a sequence (for example, the dropship flying out), take many shots 1 s apart, or `rec` at 20 fps. Then pick the DOS frame whose state best matches the port frame, using the radar blip, the path position or the message line.

## 6. Capturing the matching frame from the port

The port has test-only environment hooks for producing the same frame headlessly. This is what our sequence tests do (tests/sequences/run_sequences.py). A Mac OS 9 port won't have these, but it should grow equivalents: you need a deterministic "start mission X, wait N frames, dump a screenshot" path. Without one, comparisons turn into guesswork.

| Hook | Use |
|---|---|
| `MW2_EDITION=dos` + glview `<MECH2/MW2.PRJ> <MECH2/MW2.PRJ> dos @ YELLSCN1` | The DOS look: the DOS MW2.PRJ as models and textures, with the DOS HUD. |
| `MW2_SIZE=640x480` or `1024x768` | Must equal the DOSBox mode you're comparing against. |
| `MW2_NO_STARTUP=1` | Skips the startup movies and the shell. |
| `MW2_AUTOPILOT=secs,throttle,twist,fire,final.ppm,1` | Runs a fixed number of 1/30 s frames, then dumps the final frame. |
| `MW2_SHOT=t:path,t:path` | Takes extra screenshots at given game times. |
| `MW2_KEYS=t:key,...` / `MW2_HOLD=t0-t1:key` | Scripted input at game times, so it matches the DOSBox steps. |
| `MW2_START=x,z` / `MW2_START_HEADING=deg` | Puts the player in a chosen spot (cm), to frame the same view as a DOS shot. |
| `MW2_RADAR_LOG=1` | Prints `RADAR t=..` counts and `RADARTHING now=<tick> x y` blip pixels. |
| `MW2_STATE_DUMP=1` | Dumps unit positions and states, to compare with debugger reads. |
| `MW2_TEST_SKY_STILL=1` | Freezes sky motion, for stable diffs. |

Use the **same player mech** as the DOSBox install (a Mad Dog). The eye height comes from the mech's MGEO centre, so a different mech moves the whole view.

## 7. Comparing

1. **Positions first, then pixels.** First check that things are in the right place (radar blip pixel, object screen position, horizon row). Look at colours and shading only after that.
2. **Pixel diff:**

```python
from PIL import Image, ImageChops
a = Image.open('dos.png').convert('RGB'); b = Image.open('port.png').convert('RGB')
d = ImageChops.difference(a, b).convert('L')
h = d.histogram()
mean = sum(i*n for i, n in enumerate(h)) / (a.width*a.height)
big = sum(h[33:])                       # pixels off by more than 32 levels
print('mean %.2f  >32: %d (%.2f%%)' % (mean, big, 100*big/(a.width*a.height)))
d.point(lambda v: 255 if v > 32 else 0).save('diff_mask.png')   # look at WHERE they differ
```

   Always look at the diff mask. A one-row shift shows up as a thin line along every horizontal edge, and that is a placement bug, not a colour bug.

3. **Vertical shift search:** for "is the camera or eye height right", slide one image by -20..+20 rows and find the shift with the lowest mean diff over the world area (exclude the HUD). A best shift of 0 px is the target. Our DOS eye height was fixed this way: it had been off by -14..-20 px.
4. **Small fixed readouts:** for things like the radar, read exact pixel coordinates out of the DOS frame and assert them in an automated test, so the result can't drift later.
5. Write each finding down with its source (DOSBox frame name, or engine address) before moving on. Our ASSUMPTIONS.md table has one row per behaviour, with the source column marked **engine** (traced), **DOSBox** (measured) or ASSUMED.

## 8. Hard states: patched EXE copies and the debugger

Some states are hard to reach by playing: a certain timer, a unit at a set spot, a damage state. In those cases:

- **Patch a copy of MW2.EXE** in a throwaway install copy (for example, change an immediate or a jump to force a state), and give it its own conf. If you edit a .PRJ resource record instead, **fix that record's checksum** too, or the game refuses or misreads it. Never patch the user's original files.
- **dosbox-debug for memory reads.** Run the debug build with the same conf. The debugger window needs a terminal, so run it under `xterm`/`script` or a pty on the Xvfb display. Break in (Alt+Pause), then dump the unit table to read positions (cm, 32-bit) and compare with the port's `MW2_STATE_DUMP`. We used this for the player eye position at the MAROSCN1 and YELLSCN1 starts. In DOS, a unit's root stands at feet + MGEO centre (int[0]), and the hips' OBJ offset is not applied. The Mad Dog's eye at the MARO start was (42388, 931, -32100). This route is fiddly (you have to locate the structure each run under DOS4GW's flat memory), so prefer a radar or screen readout whenever one exists.

## 9. Dropship checklist (YELLSCN1 / TAUPSCN1)

These are the measured facts the MW2 port matches. A Mac port should reproduce them exactly.

- **Drawing height.** Dropships (GP class 8) and aircraft (class 5) are **drawn hanging from feet + the model's MGEO centre height** (int[0]). DROPA is 75 m. Their geometry is modelled at ground level and the flying height lives in int[0]. If the dropship sits on the ground in front of you at mission start, that's the bug: read int[0] up to 200 m for these classes, and don't cap it at a mech-like 50 m. TAUPSCN1 in DOSBox shows the Wolf Dropship overhead from the start. Turned toward it, you see only the top of the canopy, and then it crosses high over the player and flies away above the hills.
- **Physics vs drawing.** Their physics stays on the ground (feet on the terrain), while the collision sphere, aim point, target bracket and target viewer all use the lifted centre.
- **A thing's position** for the radar, target range and guidance is its **object's world bounding-box centre** (the local vertex box's middle placed through the node), **not the model origin**. Engine 0x100257a0 / DOS 0x367f0. With the origin, the blip is 1 px (about 10 m) too high.
- **The radar is the precise position check.** On the YELLSCN1 start, the dropship is a 2x2 friendly-thing blip from the first HUD frame. The waiting blip covers **(87,79)-(88,80) at 640x480** and **(138,127)-(139,128) at 1024x768**. Then it moves out across the ring, through about (150,219), to (137,256) at 1024x768, and disappears past the ring at about 12 s of game time. Compare your blip to these pixels.
- **Paths.** Dropships follow their path task (TSK type 5 / PTBL): points x, y, z, pitch, yaw, roll, segment ticks; loop, repeat or oneshot. Position and angles are smoothed with tau 0.3 x 181 ticks, and the heading is atan2 of the direction. Timing is in **ticks (182/s)**, never frames.
- **Landing.** In YELLSCN1 the cockpit dips about 1.3 s after the first frame, as the mech drops from the dropship. The exact start height is still an open item in our notes, so don't treat our port as the reference for that one.
- **Exhaust colours.** These are a colour-frame task (6342: palette 18-17-16, cycling).

## 10. Pitfalls we hit

- **Matching by wall time.** DOSBox is slow and uneven, so match by game events (section 5).
- **Pointer acceleration.** It makes `click` miss; the harness turns it off with `xset m`. If clicks still land off, recalibrate the 246/210 factors.
- **Two DOSBox runs at once** fight over `:99`. Always use `flock`.
- **A capture taken before the HUD has powered up**, or during the load screen.
- **Comparing different resolutions or different mechs.** Check the crop box and `USERSTAR.BWD` every time.
- **Filtered output** (opengl output, scalers) changes the pixels. Use `output=surface`.
- **Leftover state in a shared install copy:** a patched EXE or a changed pilot file. Use one copy per experiment.
- **Trusting your own port as the reference.** When the original and the port disagree, the original wins. When you can't measure something, mark it ASSUMED in your notes; don't present it as fact.

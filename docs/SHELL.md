# MW2 shell (MW2SHELL.EXE) - data formats and screens

## Files
- `DATABASE.MW2` - shell asset container: u32 count (103), count x u32 offsets (no names).
- `ARCHWO.MW2`, `ARCHJF.MW2` - the in-game lore Archive text per clan (same container).
- `SMK/` - 140 Smacker videos (SMK2) and 107 SHP sprites, loose files. Prefixes: AWO (Clan Wolf),
  AJF (Jade Falcon), AIA (Instant Action), APL (planets), WWO/WJF/WIA (briefings), M* (cinematics).
- `GIDDI/` - cockpit control configurations (CONFIGnn.CPC).
- Handoff to the simulation: `userstar.bwd` (player), `en%02dstar.bwd` (enemies), `instmap1.bwd`.

## DATABASE.MW2 entries
- Compressed entries: u32 uncompressed size, then LZSS:
  flag byte LSB-first, 1 = literal byte; 0 = two bytes a, b: offset = a | (b & 0x0f) << 8,
  length = (b >> 4) + 3; 4096-byte window, zero-filled, write position starts at 0 (verified:
  the PCX run-length data then ends exactly at the palette marker for every image).
- Contents: 21 x 640x480 8-bit PCX (each with its own palette), XMIDI music (FORM/XDIR),
  30 RIFF WAV sounds (uncompressed), "1.10" SHP sprites, "1." fonts, one MZ DLL.

## Screens (DATABASE entry -> screen)
0 main title | 1 Hall of Honor | 2 Combat Variables (options) | 3 Cockpit Controls | 4 The Keshik |
8 Instant Action scenario setup | 10-16 Clan Wolf rooms (hall with plasma orb, holo-room, mech bay,
briefing room x2, framed display, Sibko roster / pilot record) | 17-23 Jade Falcon equivalents.
Each screen = background + small animated SMK hotspots (orb AWOBALL, holo-projector AWOHOLOP,
briefing display AWOBRIEF, planet APLAN01, training grid AWOGRID, mech slots AWOSTR1...).

## Screen state machine (MW2SHELL FUN_00036700)
Current screen id + clan context (0 Wolf, 1 Jade Falcon, 2 neutral). Start: screen 8, context 2.
0 FUN_1c080 | 1 FUN_1dbe0 (clan hall) | 3 FUN_23890 | 5 FUN_26b50 | 7 FUN_29680 (Instant Action, context 2) |
8 FUN_2a410 (title) | 9 FUN_339e0 | 10 launch the simulation (handoff files, exit code 3; the shell
restarts and resumes) | 11 FUN_37a50 | 12 FUN_38310 | 13 FUN_3b420 | 14 FUN_3d980 |
15 landing cinematic (mwoland / mjfland) -> 1 | 16 ending (mend / mend2) -> 1 | -3 exit (exittos).
Per-context music tables at 0x83880 / 0x838c8 / 0x83910 (indexed by screen).

## Title screen (screen 8, FUN_0002a410)
Background slot 1 (DATABASE entry 0); Smacker AMWLOGO1 at (111, 33); ambient WAV entry 74.
Hotspot table 0x7e7fc: 7 ints each {x0, y0, x1, y1, label_x, label_y, text}; "~" prefix on labels.
0 (219,294)-(426,419) TRIALS OF GRIEVANCE -> 7 (context 2) | 1 (427,245)-(634,373) WOLF CLAN HALL -> 15
(context 0) | 2 (10,197)-(200,370) JADE FALCON CLAN HALL -> 15 (context 1) | 3 (0,450)-(639,479) EXIT ->
"Embrace cowardice? Yes/No" -> -3. Labels drawn on hover with DATABASE font entry 28 (assumed behaviour).

## Music (XMIDI) and the Gravis UltraSound
- DATABASE entries 33-72 are 40 LZSS-packed XMIDI songs in five arrangements of eight, one per
  Miles driver family. Getters are 1-based (FUN_00021ad0 / FUN_00021a30 use index - 1).
- Per-screen music numbers (tables 0x83880 neutral, 0x838c8 Wolf, 0x83910 Falcon): 0x23-0x29;
  0x20000000 = keep the current music. Database index = number + driver offset (table 0x83e14):
  ADLIB/SBLASTER/PAS 0, ADLIBG/OPL3/SBPRO/PASPLUS/TANDY 8, SBAWE 16, MT32 24, MPU401/SNDSCAPE/ULTRA 32.
  Entry = index - 1, so the GUS set is entries 65-72 (0x22 -> 65, 0x23 -> 66 ... 0x29 -> 72).
- ULTRA.MDI (Miles GUS driver): ULTRADIR/ULTRASND env, ultrasnd.ini, memory-size column of
  MIDI/ULTRAMID.INI (1024.GUS / 768 / 512 / 256, or CUSTOM.GUS), patches MIDI/<name>.pat.
  src/gusmid.c reproduces this; the envelope frame rate (44100) and the GM volume/velocity
  curves (squared) are ASSUMED. Vibrato / tremolo from the patches NOT YET.

## Instant Action - "Trials of Grievance" (screen 7, FUN_00029680)
Background entry 8. Videos: friendly clan emblem (13,205), enemy (483,329) - clans WIAWOLF, WIAJF,
WIAGHOST, WIASMOKE, WIANOVA, WIASTEEL (never the same on both sides); WIALANCH (209,371);
planet APLANnnC (414,10) per scenario. Scenarios (0x7f2fc): jackscn1 chedscn1 edamscn1 provscn1
goudscn1 colbscn1 goatscn1 whizscn1 ricoscn1 swisscn1. Mechs (0x8367c: code, file, name, tons, ?, ?):
15 normally (Firemoth .. Dire Wolf); pilot names "Calvin" / "Hobbes" / a third string raise the count to
16 / 17 / 18 (Elemental, Tarantula, Battle Master IIC). Formations (0x7d470): Echelon Left, Echelon Right,
Line Abreast, Line Astern, V-Form, Wedge. Hotspots (0x7e8a4, 25): 0 Launch -> screen 10; 1 Exit -> 8;
2 next scenario; 3-5 next mech, 7-9 previous, 6/10 formation, 11 clan, 12 -> screen 9, 13 -> screen 13
(friendly); 14-24 the same for the enemy. Lists: font entry 25, colour 1 -> palette 0x22.
Scenario defaults (FUN_000291b0, done): record <first 4 letters>BRF2. SDSC x 2 = friendly then enemy lance
({+0 ? (3 / 7), +4 tonnage limit, +8 ?, +12 star size}, loadouts at +16 every 16 bytes) fill the star records
0x840dc / 0x8415c (FUN_0003ab50: +0 formation, +8 / +0xc sizes capped at 3, +0x10 tonnage limit; FUN_0003a8a0 per
slot: loadout at +0x18, pilot at +0x28, stride 0x24; a chassis over the limit is refused). PDSC = {planet 1-12,
three text lines} drawn at (239, 69), 12 apart, in font entry 31 (thin) with colour 1 -> 0x22, as the lists. The
planet clip aplanNN (0x7f29c) plays once, then aplanNNc (0x7f2a0) loops. WIALANCH (slot 0x10, flags 0x24) is not
shown until LAUNCH starts it (FUN_00039610). EXIT is drawn grey (colour 6). SUPS (e.g. "liagoat") unused here.
Reference captures: docs/reference/dos_instant_action_jack.png / _ched.png (text, lists, LAUNCH, EXIT: 0 pixels
different). The mech arrows skip chassis over the scenario's
tonnage limit (FUN_0003a8a0).

## Instant Action context (2): Star Configuration and the Mech Lab
Hotspots 12 / 13 per side open the Mech Lab (screen 9) / Star Configuration (screen 13) on that side's star record
(0x840dc friendly / 0x8415c enemy; the port: clan 2 / 3, kept in g_ia); EXIT CONFIG returns to screen 7. Context 2
uses the Wolf hotspot tables (0x7f11c + 2 x 16 -> 0x7dc64), background slot 10 (entry 9), and its own clips
(FUN_0003b420 / FUN_000339e0): aiagrid (108,306), aiastr1 for all three bays, wiabkg2 / wiabkg1 (219,414, flags 4,
SHP stills), wiamp1 / wiamp1es panels, showcases aiasc%s, turntables aiamp%s. The stills with flags 0x24 (wiadsgn,
wiastar, wiacn, wiacp, wiavn, wiavp; also wialanch) are loaded but not shown until triggered. Star Configuration
text (FUN_0003b1f0, all contexts): the formation centred at y 4 (font entry 26), then "Mission: ..." ("Trial of
Grievance" in context 2), "Maximum 'Mechs in current Star: %d", "Keshik Defined Maximum Tonnage (KDMT) per 'Mech:
%d.00 T", "Current Total Mass of the Star: %d.00 T" centred at y 35 / 50 / 65 / 80 (font 31). Per formation
(0x83e84, 3 x {member, showcase x, y, box}) each member's showcase is drawn with its canvas origin at (x - 90, y - 135)
(fitted to the capture) and its box text (pilot cut at 92 px, 'Mech, "%d.00 T", 12 apart) at 0x84210 / 0x8421c
(Wolf and context 2) or 0x84228 / 0x84234 (Falcon). Captures: docs/reference/dos_ia_star_config.png (text, boxes,
buttons 0 pixels different; the animated grid aside) and dos_ia_mech_lab.png (header, panels, labels 0; the
turntable's phase aside). The context-2 Mech Lab header is grey 170 (entry 6) where the clans' is entry 7:
measured, mechanism ASSUMED. In the campaign an unchosen slot shows the mission's own 'Mech, and "Maximum 'Mechs"
is the mission's star size (SDSC; Pyre Light 1). docs/reference/dos_wolf_star_config.png: header, box, EXIT 0 pixels
different (the animated grid aside). Enemy pilot names "Enemy n" ASSUMED.

## Launch handoff (screen 10, FUN_000375a0 / FUN_0003abb0 / FUN_0001d540 / FUN_0001d6c0)
- mw2prm.cfg (536 bytes): return screen, clan context, flag, command line
  "<scenario> -b=xxxxxxxx.xxx -of=<friendly formation> -oe=<enemy formation>" (formations: echelonl
  echelonr lineabreast lineastern vform wedge). The shell exits with code 3; MECH2.EXE runs the sim.
- userstar.bwd: the player's lance; en01..en05star.bwd: the enemy lance, skill 8 - (1 easy / 2 medium /
  3 hard) for en01, one lower per file. GPS record (92 bytes): +8 MEK id (standard loadouts, else 0xfffe),
  +10 chassis BWD id, +12 file number, +13 lance leader, +14 0 player / 2 AI, +16 skill row (skill,
  range, 1, range, pilot level; table 0x7a7a8), +32 6, +34 0x400, +36 chassis, +45 loadout, +54 pilot.
  src/lance.c reproduces the original USERSTAR.BWD byte for byte.
- instmap1.bwd: BMPJ (CEL camo record: l1wolfcl l1jadefn l1gostbr l1smojag l1novact l1steelv) + BMID slot
  (0x114 friendly, 0x115 enemy), then jscamoia -> 0x101.
- Instant Action missions (e.g. JACKSCN1) include UserStar, en01star..en05star and instmap1; a STAR
  chunk lists 5 enemy stars with formation "trialline"; nav points jackST01 (player) .. jackST51.
  The port's simulation does not yet spawn star lances (0 actors) - NOT YET.

## Star lances in the simulation (engine: STAR 0x10042d40, GPS 0x1003f305)
- Star table (16 stars, 0x38 bytes each); STAR chunk entries {u32, u32, formation[16]}; each GPS joins
  the star named by its file byte (+0x0c); +0x0d leader; +0x0e == 0 is the human player.
- Port: star N = state table N (start nav jackSTN1), followers in the STAR formation (trialline).
  Fixed: data roots are registered before missions load, so external includes (UserStar, en01star..,
  instmap1) are read; the player's own GPS is not spawned as an AI; units from the player's lance file
  are friendly (player targeting skips them; AI picks the nearest hostile per side).
- Record ids differ between the DOS and 3D-edition archives (maddog BWD 29 vs 30): lance files must be
  written against the archive the simulation runs on.

## Launch in the port (mw2shell)
Instant Action Launch writes USERSTAR / EN01..05STAR / INSTMAP1 into MW2_SAVE_DIR (default: the install
dir) with record ids from MW2_SIM_PRJ, then runs MW2_SIM ("%s" = scenario, e.g.
"./glview <3D MW2.PRJ> <texture PRJ> ati @ %s") with MW2_INSTALL_DIR = the save dir and MW2_OF / MW2_OE =
the formations (the loader prefers them over the STAR formation). Music stops and the window hides during
the mission; the shell returns to the Instant Action screen afterwards. MW2_PILOT = the player's name.

## Mission end and results (engine 0x1000a9b0 / 0x10009f50 / 0x100374e0 / 0x1000aba0)
The player's star result (all main objectives done / one failed / time limit) or the player's destruction
starts the end sequence: the outcome message; after 264 ticks the radio queue is flushed; the mission closes
after 1820 ticks (Enter / Space skip, Esc aborts). The MISSION OBJECTIVES display is on F12 only. The simulation then writes MW2MSN.CFG into the install dir; the shell
deletes any stale copy before launching, reads it afterwards and shows the result on the Instant Action
screen (the original returns to that screen too).

## Clan halls (screen 1, FUN_0001dbe0)
Per-clan record at 0x7f08c + clan x 16: {hotspot table, count 5, background slot, music}: Wolf 0x7d52c / slot 11
(entry 10), Jade Falcon 0x7de94 / slot 18 (entry 17). Hotspots (x0, y0, x1, y1, label x, label y, text):
Wolf: CADET TRAINING (185,280)-(240,400) -> 14; ARCHIVE HOLOPROJECTOR (320,300)-(470,400) -> 5 (clip awoholop);
READY ROOM (20,245)-(90,411) -> 11, or "This pilot has already won the game" -> 16 once rank >= 16;
REGISTER (95,385)-(180,479) -> clip aworgstr -> 12; EXIT (0,0)-(639,40) -> 8.
Falcon: (66,167)-(152,303), (160,290)-(375,322), (523,154)-(636,332), (397,385)-(492,466), exit as Wolf;
clips ajfholop / ajfrgstr; JF transitions ajf8torl / ajf8torr.
Looping clips: Wolf awoball (91,380), awoarcht (328,332), awolite1 (0,280), awolite2 (158,300), awolite3 (580,275);
Falcon ajfball (400,384), ajfarcht (220,280). First visit with no pilot: the registration clip, then 12.

## Sibko roster / registration (screen 12, FUN_00038310) and MW2REG.CFG
MW2REG.CFG: 20 records x 60 bytes (Wolf 0-9, Falcon 10-19): +0 in use, +4 selected (the active pilot), +8 clan,
+12 missions done (campaign progress; >= 16 "already won the game"), +16 rank title index (table 0x7d394:
Mechwarrior, Star Commander, Nova Commander, Star Captain, Nova Captain, Star Colonel, Nova Colonel, Galaxy
Commander, Khan), +20 honor (install's pilot: 1850), +24.. kills / hits / shots, +40 name. Clans 0x7d3b8: Wolf,
Jade Falcon, Ghost Bear. Roster hotspots 0x7f0bc + clan x 16 -> 0x7d5b8 / 0x7df20 (15): 0 NEW ALLEGIANCE,
1-10 pilot rows (32,83)-(297,116) step 35, names drawn at (42, 92 + 35 i); 11 ACCEPT, 12 DELETE MECHWARRIOR
("Terminate MechWarrior?#Yes|No"), 13 LAUNCH OLD MISSION (missions done > 0), 14 PILOT INFO; background slot 17 / 24.
Empty row: text entry; a new pilot is selected with rank 0. Hall of Honor table (FUN_00028250): Pilot, clan,
rank title, Honor, Kills, Hit %, Last Mission.

Right-hand panels (tables of 11-int records {x, y, w, h, .., object, make callback, click flag, index / text}, shown by
0x2ac90, hidden by 0x2ada0; y < 0 = relative, 0x8000xxyy: previous y + xx x previous height + yy):
- RECORD (0x83970): name (font 29) (468, 92); RANK / HONOR / MISSION labels (font 27) y 209 / 260 / 311, values (font 29)
  below them. Shown on entering with a pilot.
- LAUNCH OLD MISSION list (0x83ad0): name (468, 92); "Select Mission" (font 27) (468, 200); 16 items (x 418, w 100,
  index 0-15) made only while index < missions done: the campaign title (0x7f04c + clan x 4, 9-byte records, +5 title)
  in font 31, white, centred: y 219 + 12 k. Titles as spelled there (Wolf 2 is "Flame Tongue " with a trailing space).
- Buttons 13 / 14 share the spot under the panel (label centre (468, 408)): 13 is shown with the RECORD when missions
  done > 0; clicking it shows the list and PILOT INFO (14), which brings the RECORD back. Selecting, creating or deleting
  a pilot returns to the RECORD. Button labels grey (palette 6), white under the cursor.
- A click inside an item's box (0x305a0: 418 <= x < 518, y .. y + height; checked before the buttons) launches that
  mission at once: the star is the mission's own (0x3ab50(0,0,3,1,100), 0x291b0 BRF2 SDSC, formation 0, the pilot in
  slot 0), state 10 with return state 12. No briefing; the simulation comes back to the roster (RECORD panel) with no
  debriefing - campaign position, honor and kills unchanged (DOSBox: drop screen, Ctrl+Q, roster).
- Sound: DATABASE 0x51 at volume 0x1e on entering the roster (0x384f8).
DOS captures: docs/reference/dos_roster_record.png, dos_roster_select_mission.png; port comparisons
roster_record_dos_vs_port.png, roster_select_mission_dos_vs_port.png, roster_all_missions_dos_vs_port.png (16 done), roster_buttons_hover_dos_vs_port.png (identical
but for the mouse pointer). Test hook: MW2_SHELL_CLICKS="x,y;x,y" (clicks on the roster's first frame).

## Ready Room (screen 11, FUN_00037a50)
Four hotspots normally; the 20-hotspot set (the 16 missions by codename) only when the pilot is named
FREEBIRTHTOAD (strcmp == 0): picking one sets the pilot's campaign position. Codenames (labels "<~", always
shown, (400|520, 25 + 30 k)): Wolf YELLOW ORANGE TEAL TAUPE JENNY SABLE GREY BROWN AMY SILVER AQUA KIM CYAN MAROON
GOLD IRENE; Falcon PINK GREEN RED FUCHSIA CINDY RUST UMBER TAN HEIDI PLUM WHITE JILL PUCE BLONDE BRONZE MARY.
Messages: "Trial Protocol: X0769-Q|Keshik to determine appropriate|'Mech for trial.#Ok" (mech lab on a trial),
"Your 'Mech has been|selected for you.|Prepare for Trial!#Ok" (star config on a trial), "This pilot has|already
won the game.#Finale" (the hall's READY ROOM at 16 missions -> 16, ending mend / mend2). Dialog text: '|' new
line, '#' buttons.
Per-clan record 0x7f14c + clan x 16 {hotspots, count, background, ...}; pilot name "FREEBIRTHTOAD" selects a
4-hotspot set. Training grid clips awogrid1/2 (303,329) / ajfgrid1/2 (277,341). Hotspot 1: the mission's
briefing clip, then screen 9; on a trial ("Trial Protocol X0769 Q: Keshik ...") a message instead. Hotspot 2:
the mech lab (screen 13), or "Your 'Mech has been selected for ..." on a trial.
Trials table 0x7f04c + clan x 4 -> 0x7ef14 / 0x7efb0, records of 9 dwords {scenario, ..., title}; +4 low byte
1 = trial (fixed mech): Wolf yellSCN1 "Temper Edge", jennSCN1 "Scorching Sand", amy_SCN1 "Trial 3", cyanSCN1
"Trial 4"; Falcon pinkSCN1 "Bone Machine", cindSCN1 "Rogue Chariot", heidSCN1 "Trial 3", puceSCN1 "Trial 4";
This is the full campaign: records are 9 BYTES {scenario ptr, trial flag (the code's [m x 9 + 4]), title ptr},
16 per clan, indexed by the pilot's missions done:
Wolf yell Pyre Light, oran Flame Tongue, teal Blade Splint, taup Temper Edge, jenn Trial 1*, sabl Sable Flame, grey
Burning Chrome, brow Scorching Sand, amy_ Trial 2*, silv Silver Staff, aqua Aquiline Fire, kim_ Trial 3*, cyan Cold
Crescent, maro Velvet Hammer, gold Golden Spade, iren Trial 4*;
Falcon pink Silent Thunder, gree Arkham Bridge, red_ Mirror Cage, fuch Bone Machine, cind Trial 1*, rust Bouk Obelisk,
umbe Umber Wall, tan_ Rogue Chariot, heid Trial 2*, plum Plum Wine, whit Rust Heart, jill Trial 3*, puce Armor Veil,
blon Iron Piston, bron Bronze Anvil, mary Trial 4* (* trial: the mech is assigned). Then tnw1-6 / tnj1-6.
Ready Room hotspots 0x7f14c + clan x 16 -> 0x7d890 / 0x7e1f8 (20; background slot 14 / 21): 0 CLAN HALL, 1 MECH LAB
-> 9, 2 STAR CONFIG -> 13, 3 MISSION BRIEFING (clip awobrief (105,100)), 4-19 the missions by colour (YELLOW,
ORANGE, ... / PINK, GREEN, ...) at (400, 25 + 30 k).

## Briefing (screen 0, FUN_0001c080)
Text: BWD record <first 4 letters of the scenario>BRF1, ORDR chunk; markup \c centre, \n line break, \t tab; the
orders (codename, planet, terrain, time, objectives) then SITUATION (the BearNet narrative). Mission 15 with a
pilot title index > 5: KTWOBRF1 / KTJFBRF1. Box Wolf (88,30) 454x400, Falcon (98,51) 403x372. Hotspots
0x7f1dc + clan x 16 -> 0x7dac0 / 0x7e428 (background slot 16 / 23): ABORT (430,450)-(529,474) -> 11, SITUATION
(110,450)-(209,474), LAUNCH (270,450)-(369,474), SKIP (540,450)-(639,474) -> 3 (the debriefing, the mission not flown); SKIP exists only when the pilot is named exactly
FERRARI (otherwise the hotspot count is set to 3).
Other screens: 3 debriefing (FUN_00023890: Mission Completion, objectives, wingman deaths, kills, honor),
9 mech lab (FUN_000339e0), 13 star configuration (FUN_0003b420), 14 cadet training (FUN_0003d980).


## Debriefing (screen 3, FUN_00023890; scoring at 0x00021ee0, a function Ghidra missed)
Inputs: MW2MSN.CFG ("MW2M", entries, start, end, status 2 success / 3 failed / 4 time; 52-byte objective records
{succeeded, type 1 primary / 2 secondary / 4 tertiary / 8 return, 0, time or -1, primary flag, text[32]}; the type
comes from the mission-table node's priority byte (just before the 'M' / 'O': 1 / 2 / 0)), MW2CAR.CFG (the
simulation's u16 counters: +0x07 mechs the player destroyed, +0x13 shots, +0x15 hits, +0x1e mechs destroyed,
+0x34 wingmen lost, +0x44 / +0x4a vehicles direct / total, by side in triples), MW2DIF.CFG (+4 heat tracking,
+5 difficulty). Honor: completion 5000 (0 if any primary failed), secondary 1500, tertiary 500, wingman -4000,
mechs 250 x total, vehicles 125 x total, underweight 25 per ton, hit ratio >= 0.7: 750; x 0.8 / 1.0 / 1.3
(EASY / MEDIUM / HARD); heat tracking off or failure: none. Pilot: kills += mechs total + vehicles direct,
hits / shots accumulate; honor and campaign progress on success. Text: "\g%03d\b%03d%s" = value right-aligned at
x 350. Hotspots 0x7f17c + clan x 16 (background 16 / 23): EXIT (270,450)-(369,474), AFTERMATH (110,450)-(209,474),
REPLAY (430,450)-(529,474: "Are you Sure?#Yes|No" after a success; restores the pilot record).

## Star Configuration (screen 13, FUN_0003b420) and the Mech Lab (screen 9, FUN_000339e0)
Star record 0x841dc (per clan): +0 formation 0-5 (the six -of= formations), +4 the slot being changed, +8 max size 3,
+0xc size, slots from +0x14 every 0x24 bytes {mech index, loadout, ..., pilot name at +0x14} (defaults MechWarrior,
Friend 1, Friend 2). Hotspots 0x7f11c + clan x 16 (9; background slot 15 / 22): 0 EXIT CONFIG (50,445)-(149,469)
-> 11, 1 MECH LAB (404,414)-(474,474) -> 9, 2 / 3 NEXT / PREV FORMATION, 4 ADD / 5 DELETE STARMATE, 6-8 CHANGE MECH
(the bays) -> 9 for that slot. Formation table 0x83e84 (Wolf) / 0x83fa4 (Falcon): per formation 3 x {slot, x, y,
bay}; wingman names at (81,166) (313,133) (508,189). Clips: grid, bays awostr1-3 / ajfstr1-3, buttons wwobkg
wwodsgn wwocn wwocp wwovn wwovp (Falcon wjf*, Instant Action context aia* / wia*); per-mech showcase awosc%s.
The Mech Lab saves designs as mek\%s%02dusr.mek ("Invalid 'Mech specification" checks: criticals, mass).
Port: the star is saved per clan as STAR<clan>.CFG (the original's storage not confirmed); the Mech Lab picks
chassis and standard loadout only (weapon editing not yet); campaign launches use the star and its formation.

## Mission setup and aftermath records
<mission 4 letters>BRF2: SDSC {?, star tonnage limit, ?, star size} + 16-byte default loadouts (trials: the assigned
mechs; e.g. YELL 60 t mdg00std, SABL 85 t 2 x tbr00std, IREN 100 t tbr02std); SUPS: the camo / insignia argument
(-b=, e.g. lwocolm). <mission>DBFS / DBFF: the AFTERMATH text (ORDR) after success / failure. (<mission>END1/2 are
GPS unit records, not text.)

## Global menu, Hall of Honor, Cadet Training, Archive
Global menu FUN_000258e0 (Esc / right button; Esc on the title too): panel = shell image 7 (DATABASE entry 6, SHP
244x248) at (198,116); items grey in shell font 0x1f (entry 30), the selected white in 0x1e (entry 29), y 157 + 33 k minus
half the font height (15); DOS capture docs/reference/dos_global_menu.png: the panel 0 pixels different.
Font header: "1." , glyph count, height (+8), transparent index. Shell font handles: 0x1a.. = DATABASE entry - 1.
The Keshik (FUN_00021400): background shell image 5 (entry 4, "THE KESHIK"), the amwlogo1 clip at (120,4), credits from
MW2SHELL.EXE's table 0x7ca80 (388 lines, 20 px apart, 1 px a step from y 460; '<' font 0x1b, '>' 0x1c, '~' 0x1b colour
1 -> 0x10, else 0x20; shown 105 < y <= 459); any key / click returns. Cockpit Controls (FUN_00021020): see tools/mw2shell.c
cockpit_controls and src/cockpitcfg.h (pages 0x7c6c0 / 0x7a9b4, GIDDI drivers and CPC files, the INPUT.MAP writer);
captures docs/reference/dos_cockpit_devices.png / dos_cockpit_editor.png. FLEE TO DOS: "Embrace cowardice?#Yes|No"
in the shell dialog (FUN_000263d0), No by default; every shell message now uses that dialog.
Older note: items 0x7eefc NEW ALLEGIANCE, COMBAT
VARIABLES, COCKPIT CONTROLS, HALL OF HONOR (FUN_00028250), THE KESHIK, FLEE TO DOS; centred x 320, y 157 + 33 k.
Hall of Honor: background slot 2 (entry 1), logo amwlogo1 (120,4); columns Pilot 0, Clan 125, Rank 200, Honor 325,
Kills 400, Hit % 460, Last Mission 520 (the title of mission "missions done" - 1), headers y 150, 8 rows from 182 step 16,
sorted by honor then missions (FUN_000281e0).
Cadet Training (screen 14, FUN_0003d980): 0x7f23c (7; background slot 13 / 20): CLAN HALL (5,149)-(57,440); NAV
COMPUTER, MECH HANDLING, WEAPONS USAGE, HUNTING, INSPECTION, TRIAL at (450, 13 + 25 k) -> tnw1-6 / tnj1-6; clips
awotrnwn (453,0) once, instructor awotrnwa / awotrnwb alternating at (72,224).
Archive (screen 5, FUN_00026b50): ARCHWO.MW2 / ARCHJF.MW2: u32 count (70), u32 offsets[count + 1], pages of tagged
fields 00 01 title, 00 04 <target> 00 <name> (links), 00 02 text (\aNN: the next word links to link NN), 00 ff end.
Note: the registry record is 60 bytes (one runtime field after the name).

## Combat Variables (global menu entry 2) and the port's display settings
Background slot 3 (entry 2) contains every option word; the value column (375,124) 258x352 is blanked and the chosen
word copied back (widgets 0x7d09c, x 393): DIFFICULTY y 219 (MW2DIF +5 EASY / MEDIUM / HARD), HEAT TRACKING 239 (+4),
OBJECT / TERRAIN TEXTURES 277 / 297, DISPLAY DETAIL 317, OBJECT DENSITY 337 (HIGH / LOW), CHUNKY EXPLOSIONS 357
(MW2SND.CFG int32 +0x14 .. +0x24, 1 = ON / HIGH), RESOLUTION 377 (MW2SND +0x2c: VESA driver name vesa480.dll /
vesa768.dll), INVULNERABILITY 416 (MW2DIF +1), UNLIMITED AMMO 436 (+0), NO COLLISION DAMAGE 456 (+3); sliders MUSIC /
EFFECTS / VOICE (335, 128 / 152 / 176) 285 wide = MW2SND int32 16.16 at +0xc / +4 / +8.
Port: RESOLUTION cycles DESKTOP (native fullscreen, the default) / WINDOWED / the display's modes; ANTI-ALIASING (y 397)
OFF / 2X / 4X / 8X; both in MW2PORT.CFG (resolution=desktop|window|WxH, aa=N), read by the shell and the simulation;
the original's VESA field keeps the nearest of its modes.

## Mech Lab (screen 9, FUN_000339e0, 44 KB) - decoding in progress
Hotspots 0x7f26c + clan x 16 (11; background slot 15 / 22 / IA 10): 0 EXIT LAB (50,445)-(149,469), 1 STAR CONFIG
(404,414)-(474,474), 2 / 3 NEXT / PREV CHASSIS (303,425)-(330,469) / (200,425)-(236,469), 4 / 5 NEXT / PREV VARIANT
(263,425)-(302,469) / (237,425)-(262,469), 6 CUSTOMIZE (50,420)-(149,444), 7 ACCEPT MECH (50,395)-(149,419), 8 SAVE,
9 ABORT (customise mode), 10 DELETE (490,445)-(589,469). Clips (Wolf): awogrid (140,310); panels wwomp1 (12,52) 192x240,
wwomp4ar armour (216,52) 208x428 / wwomp5wa weapons (same place), wwomp7cr criticals (436,52) 192x428 / wwomp1es engine
(same place), wwomp4mp (75,171) SHP; buttons wwobkg (219,429), wwostar (407,410), wwocn / wwocp / wwovn / wwovp.
Mech picture: SHP awomp<code> (30 shapes: shape 0 the whole mech, the rest per-location overlays), around (326,383).
Mech table 0x83684 (18 x 0x18): {picture code, archive code, file, name, tonnage, index}: Firemoth ds/frm 20, Kit Fox kf/ktf 30,
Jenner IIC jn/jnr 35, Nova bh/nva 50, Stormcrow sc/stm 55, Mad Dog md/mdg 60, Hellbringer lo/hlb 65, Rifleman IIC rf/rfl 65,
Summoner su/smn 70, Timber Wolf mc/tbr 75, Gargoyle mw/grg 80, Warhammer IIC wh/whm 80, Marauder IIC mr/mrd 85, Warhawk
ms/whk 85, Dire Wolf da/drw 100, Elemental el/ele, Tarantula ta/tar, Battle Master IIC bm/btm.
Strings: "~CALLSIGN: %s (%d.00 T MAX)", "User Variant #%d", designs saved as mek\<code>NNusr.mek; checks "Unassigned
criticals detected", "Chassis can not support current mass", "'Mech exceeds Keshik Defined Maximum Tonnage (KDMT) for
mission", "Error: Too many mechs of this variant to save", "Delete this 'Mech? Are you sure?".

### Mech Lab main page: matched to a DOS capture (pixel-identical text and panels)
Captured in DOSBox (title -> Wolf hall -> roster: select + ACCEPT -> Ready Room -> MECH LAB; pointer acceleration off,
1:1 relative moves). Header: DATABASE font 30, centred, y 4 and 28. Panels: font 31, ink remapped 1 -> palette 6
(170,170,170): "Variant:" at (20,64) and the configuration name at (65,64); COMPONENT / MASS / NOTES at y 86, x 20 / 98 /
147; rows from y 108 every 11; Used / Max Mass at y 229 / 240; WEAPONS AND AMMO at (444,64), lines from y 86. Buttons:
font 27, same ink. Turntable: SHP AWOMP<code> (30 frames), origin (175,127) + each frame's offset, 13 frames / s,
advancing (template-matched against the captures). Component masses from the MEK data by the BattleTech rules (XL
from engine criticals in the side torsos, Ferro / Endo from their criticals, double heat sinks = dissipation / 2,
10 free) reproduce the original's table. Starting chassis: the star slot's, else the mission's (BRF2 SDSC).
The roster's RECORD panel (DOS): centred RANK / <title>, HONOR / <n>, MISSION / <title> (now matched; see the roster section).

### CUSTOMIZE (DOS captures in docs/reference/dos_customize_*.png)
Header "CUSTOMIZING" / chassis (no turntable); the left panel's component rows select the right-hand page; buttons SAVE /
ABORT. Pages: ENGINE (Rating 300XL, Type XL, Manufactur(er) Vlar, Mass, Walking Speed 54.0 kph, Running Speed 86.4 kph =
running MP ceil(1.5 x walk) x 10.8 - the Mech Lab's figure, not the simulation's top speed; FASTER / SLOWER), HEAT SINKS
(Count 12 (24), Type Double, Mass; ADD / DELETE), JUMP JETS (Count, Mass; ADD / DELETE), INTERNAL STRUCTURE (Type, Mass,
points per location), ARMOR (Factor, Allocated, Mass, Type; ADD / DELETE; per-location up / down; the tall ARMOR
ALLOCATION panel: "Head (9) 9", "Right Torso (26) 16/7" ... beside the clickable armour diagram = the AWOMP SHP's
location shapes), WEAPONS / AMMO (the fitted list; ADD / DELETE WEAPON, ADD / DELETE AMMO; WEAPON INFO: Type, Heat,
Damage, Range, Mass, Crit, Ammo; the WEAPONS TABLE), EQUIPMENT (Yes / No: CASE, MASC, lower arm and hand actuators),
ASSIGN CRITICALS (a location's slots beside the diagram; UNASSIGNED CRITICALS).
User designs: MEK\<code>NNusr.MEK (mek_write = the exact inverse of mek_parse: all 96 DOS records round-trip byte for
byte); the lance files carry them as MEK id 0xfffe + the name, and the simulation loads them (mek_load: the archive, else
the file through the datapath roots). Two 3D-edition records (GRG03STD, STM04STD) are one entry short of their counts:
read with the trailing ammo link dropped.

### CUSTOMIZE: built (panels pixel-identical to the DOS captures)
Pages via the left panel's white rows (Engine, Heat Sinks, Jump Jets, Internal, Armor, Weapons, Ammo, Equipment) and
"Assign Criticals" (y 262). Panels: short right wwomp1es for Engine / Heat Sinks / Jump Jets / Internal / Armor / Equipment;
tall right wwomp7cr for the WEAPONS TABLE and UNASSIGNED CRITICALS; tall middle wwomp4ar (armour allocation, criticals:
location name white at (224,64), slots from y 79) or wwomp5wa (weapons). Values at x 544; the Armor list centred at x 344.5;
up / down arrows = font 31 codes 1 / 2 at (551 | 581, 168 | 202). Internal page rows in the Armor page's order. Weapons
table: 27 rows (energy 7, ballistic 10, missile 10; no flamer, Narc, AMS) at y 86 .. 394 with gaps at 163 and 284.
Diagram (WWOMP4MP): head at (291,223); the pieces' art carries the cross-hatching: the selected location drawn as it is,
the others filled with palette (247,89,0) (full armour; other levels ASSUMED the same); ~100 outline pixels differ.
SAVE: "Unassigned criticals detected" / "Chassis can not support current mass" / "Too many mechs of this variant"
(01-99) / "Error saving 'Mech"; writes <install>/MEK/<code>NNusr.mek as "User Variant #N". DELETE (main page, user
designs): "Delete this 'Mech? Are you sure?" Yes / No.

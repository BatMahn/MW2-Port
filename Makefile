# MechWarrior 2 port - foundation modules (Linux / macOS)
CC      ?= cc
CFLAGS  ?= -std=c99 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wconversion
LDLIBS  = -lpthread -lm
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDLIBS += -ldl
  GLLIB = -lGL
  CXXLIB = -lstdc++
endif
ifeq ($(UNAME_S),Darwin)
  GLLIB = -framework OpenGL
  CXXLIB = -lc++
  LDLIBS += -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
endif

LIB_SRC = src/prj.c src/datapath.c src/cdaudio.c src/mek.c
BIN     = prjtool cdtool mektool bwdtool mechview mechview3d cdrip test_port shelltool xmiplay
GLBIN   = glshot glview mw2shell
ifeq ($(UNAME_S),Darwin)
  GLBIN = glview mw2shell          # glshot renders headless through EGL: Linux only
endif

all: $(BIN)

build/miniaudio.o: third_party/miniaudio.c third_party/miniaudio.h
	@mkdir -p build
	$(CC) -O2 -w -c $< -o $@

xmiplay: tools/xmiplay.c src/gusmid.c src/shelldb.c src/gusmid.h src/shelldb.h
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@ -lm

shelltool: tools/shelltool.c src/shelldb.c src/smk.c src/shelldb.h src/smk.h
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

prjtool: tools/prjtool.c src/prj.c
	$(CC) $(CFLAGS) -Isrc $^ -o $@

cdrip: tools/cdrip.c
	$(CC) $(CFLAGS) $^ -o $@

HDRS = $(wildcard src/*.h tools/*.h)
GL3D_SRC = src/hud.c src/shp.c src/glr.c src/skygnd.c src/anim.c src/mek.c src/mtbl.c src/msim.c src/fx.c src/combat.c src/ai.c src/mech3d.c src/render.c src/tex.c src/wtb.c src/bwd.c src/prj.c src/datapath.c

missionsim: tools/missionsim.c src/msim.c src/fx.c src/ai.c src/combat.c src/mtbl.c src/anim.c src/mek.c src/mech3d.c src/wtb.c src/bwd.c src/prj.c src/datapath.c $(HDRS)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc $(filter %.c,$^) -o $@ -lm

glshot: tools/glshot.c $(GL3D_SRC) $(HDRS)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc $(filter %.c,$^) -o $@ -lEGL -lGL -lm

# the MT-32 emulator (Munt mt32emu, LGPL 2.1+): C++, built into a static library
CXX ?= c++
MT32_SRC = $(wildcard third_party/mt32emu/*.cpp) $(wildcard third_party/mt32emu/sha1/*.cpp) third_party/mt32emu/c_interface/c_interface.cpp third_party/mt32emu/srchelper/InternalResampler.cpp $(wildcard third_party/mt32emu/srchelper/srctools/src/*.cpp)
MT32_OBJ = $(patsubst third_party/mt32emu/%.cpp,build/mt32emu/%.o,$(MT32_SRC))
build/mt32emu/%.o: third_party/mt32emu/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w -DMT32EMU_WITH_INTERNAL_RESAMPLER=1 -Ithird_party/mt32emu -c $< -o $@
build/libmt32emu.a: $(MT32_OBJ)
	ar rcs $@ $^

# mw2: one executable - the shell, and with --sim the simulation (glview built in, its main renamed)
mw2: build/libmt32emu.a src/mt32mid.c tools/mw2shell.c tools/glview.c src/gusmid.c src/shelldb.c src/smk.c src/lance.c src/ttext.c src/cockpitcfg.c src/sfx.c src/portcfg.c src/cdaudio.c build/miniaudio.o $(GL3D_SRC) $(HDRS)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party $(shell sdl2-config --cflags 2>/dev/null) -c tools/glview.c -Dmain=glview_main -o build/glview_sim.o
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party -Ithird_party/mt32emu -DMW2_ONE_BINARY -c tools/mw2shell.c $(shell sdl2-config --cflags 2>/dev/null) -o build/mw2shell_one.o
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party/mt32emu -c src/mt32mid.c -o build/mt32mid.o
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party build/mw2shell_one.o build/glview_sim.o build/mt32mid.o $(sort $(filter-out src/mt32mid.c,$(filter src/%.c,$^))) build/miniaudio.o build/libmt32emu.a -o $@ $(shell sdl2-config --cflags --libs 2>/dev/null) $(GLLIB) -lm -lpthread -ldl $(CXXLIB)

mw2shell: build/libmt32emu.a src/mt32mid.c tools/mw2shell.c src/sfx.c src/gusmid.c src/shelldb.c src/smk.c src/lance.c src/mek.c src/prj.c src/portcfg.c src/ttext.c src/datapath.c src/shp.c src/cockpitcfg.c src/bwd.c
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party/mt32emu $(filter %.c,$^) build/libmt32emu.a -o $@ $(shell sdl2-config --cflags --libs 2>/dev/null) -lm $(CXXLIB)

glview: tools/glview.c src/sfx.c src/portcfg.c src/cdaudio.c build/miniaudio.o $(GL3D_SRC) $(HDRS)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party $(filter %.c,$^) $(filter %.o,$^) -o $@ $(shell sdl2-config --cflags --libs 2>/dev/null) $(GLLIB) -lm -lpthread -ldl

gl: $(GLBIN)

mechview3d: tools/mechview3d.c src/anim.c src/mek.c src/mech3d.c src/render.c src/tex.c src/wtb.c src/bwd.c src/prj.c src/datapath.c
	$(CC) $(CFLAGS) -Isrc $^ -o $@ -lm

mechview: tools/mechview.c src/anim.c src/mek.c src/mech3d.c src/render.c src/tex.c src/wtb.c src/bwd.c src/prj.c src/datapath.c
	$(CC) $(CFLAGS) -Isrc $^ -o $@ -lm

bwdtool: tools/bwdtool.c src/mtbl.c src/bwd.c src/prj.c src/datapath.c
	$(CC) $(CFLAGS) -Isrc $^ -o $@

mektool: tools/mektool.c src/mek.c src/datapath.c src/prj.c
	$(CC) $(CFLAGS) -Isrc $^ -o $@

cdtool: tools/cdtool.c src/cdaudio.c build/miniaudio.o
	$(CC) $(CFLAGS) -Isrc $^ -o $@ $(LDLIBS)

test_port: tests/test_port.c src/cdaudio.c src/datapath.c build/miniaudio.o
	$(CC) $(CFLAGS) -Wno-conversion -Isrc $^ -o $@ $(LDLIBS)

test_cockpitcfg: tests/test_cockpitcfg.c src/cockpitcfg.c src/cockpitcfg.h
	$(CC) $(CFLAGS) -Isrc tests/test_cockpitcfg.c src/cockpitcfg.c -o $@

test_inputmap: tests/test_inputmap.c tools/inputmap.h tools/inputmap_defaults.h
	$(CC) -std=c99 -D_DEFAULT_SOURCE -Itools $$(sdl2-config --cflags) tests/test_inputmap.c $$(sdl2-config --libs) -o $@

# make check MUSIC=/path/to/rip MW2=/path/to/install [ISO=/path/to/mw2cd.iso]
check: all
	./prjtool verify $(MW2)/MW2.PRJ
	./mektool $(MW2)/MW2.PRJ list | tail -1
	./bwdtool $(MW2)/MW2.PRJ verify
	./mechview $(MW2)/MW2.PRJ verify
	MW2_INSTALL_DIR=$(MW2) ./bwdtool $(MW2)/MW2.PRJ missions | tail -1
	./test_port $(MUSIC) $(MW2) $(ISO)
	$(MAKE) test_inputmap && ./test_inputmap $(MW2)
	$(MAKE) test_cockpitcfg && ./test_cockpitcfg $(MW2)
	MW2_LAB_MASSES=1 MW2_CONFIG=/tmp/mw2check.cfg ./mw2 $(MW2) > /tmp/mw2lab.txt; r=$$?; tail -1 /tmp/mw2lab.txt; exit $$r

clean:
	rm -rf build $(BIN) $(GLBIN)

.PHONY: all check clean

# A self-contained folder to run from anywhere: the launcher, the simulation, the font (found next to the
# executables), the documentation. Needs SDL2 and OpenGL from the system.
DIST = dist/mw2port
dist: mw2
	rm -rf $(DIST)
	mkdir -p $(DIST)/assets/fonts
	cp mw2 $(DIST)/
	cp assets/fonts/LiberationSans-Bold.ttf assets/fonts/README.md $(DIST)/assets/fonts/
	cp README.md THIRD_PARTY.md $(DIST)/
	cd dist && tar czf mw2port-linux-x86_64.tar.gz mw2port
	@echo "dist/mw2port-linux-x86_64.tar.gz"

# The objective sweep: every mission run headless by a perfect player (tools/sweep_objectives.c).
# make sweep MW2=/path/to/install   (SWEEP_ARGS="-v CYANSCN1" for one scene, verbose)
SWEEP_SRC = tools/sweep_objectives.c src/lance.c src/msim.c src/fx.c src/ai.c src/combat.c src/mtbl.c src/anim.c src/mek.c src/mech3d.c src/wtb.c src/bwd.c src/prj.c src/datapath.c
sweep_objectives: $(SWEEP_SRC) $(HDRS)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc $(SWEEP_SRC) -o $@ -lm
sweep_objectives_asan: $(SWEEP_SRC) $(HDRS)
	$(CC) -std=c99 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc $(SWEEP_SRC) -o $@ -lm
sweep: sweep_objectives
	MW2_INSTALL_DIR=$(MW2) ./sweep_objectives $(MW2)/MW2.PRJ $(SWEEP_ARGS)
.PHONY: sweep

# walking sweep (tests only): tools/sweep_walk.py drives glview (MW2_WALK_TRACE hooks); sweep_ai reports stuck AI mechs
sweep_ai: tools/sweep_ai.c src/msim.c src/fx.c src/ai.c src/combat.c src/mtbl.c src/anim.c src/mek.c src/mech3d.c src/wtb.c src/bwd.c src/prj.c src/datapath.c $(HDRS)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc $(filter %.c,$^) -o $@ -lm
sweep-walk: glview sweep_ai
	python3 tools/sweep_walk.py -o /tmp/walk $(WALK_ARGS)
	for m in $$(./bwdtool ../MW2-game/3d/models.prj missions | awk '/SCN/{print $$1}'); do MW2_INSTALL_DIR=$${MW2_INSTALL_DIR:-/tmp/gtest} ./sweep_ai ../MW2-game/3d/models.prj $$m 300 attack; done | grep aistuck
.PHONY: sweep-walk

# scripted in-mission action sequences, headless (tests/sequences/run_sequences.py; SEQ_ARGS="--only NAME -j 2")
# MW2_GAME_DIR (default ../MW2-game) and MW2_GTEST (an installed game directory, default /tmp/gtest)
test_sequences: glview
	python3 tests/sequences/run_sequences.py --glview ./glview $(SEQ_ARGS)

# the same under AddressSanitizer / UBSan (a separate binary, build/glview_asan)
test_sequences_asan: tools/glview.c src/sfx.c src/portcfg.c src/cdaudio.c build/miniaudio.o $(GL3D_SRC) $(HDRS)
	$(CC) -std=c99 -D_DEFAULT_SOURCE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Isrc -Ithird_party $(filter %.c,$^) $(filter %.o,$^) -o build/glview_asan $(shell sdl2-config --cflags --libs 2>/dev/null) $(GLLIB) -lm -lpthread -ldl
	ASAN_OPTIONS=detect_leaks=0 SEQ_TIME_SCALE=6 python3 tests/sequences/run_sequences.py --glview build/glview_asan $(SEQ_ARGS)
.PHONY: test_sequences test_sequences_asan

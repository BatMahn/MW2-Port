# MechWarrior 2 port - foundation modules (Linux / macOS / Windows via MinGW-w64)
CC      ?= cc
CFLAGS  ?= -std=c99 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wconversion
LDLIBS  = -lpthread -lm
UNAME_S := $(shell uname -s)
# SDL3 (3.2.0 or newer) through pkg-config's sdl3 module. SDL3_DIR=<prefix> uses only <prefix>/lib/pkgconfig (an SDL3
# built into its own prefix, or the MinGW development package); SDL3_CFLAGS / SDL3_LIBS override the flags outright.
PKG_CONFIG ?= pkg-config
SDL3_MIN_VERSION = 3.2.0
ifdef SDL3_DIR
  SDL3_PKG = PKG_CONFIG_LIBDIR="$(SDL3_DIR)/lib/pkgconfig" PKG_CONFIG_PATH= $(PKG_CONFIG)
else
  SDL3_PKG = $(PKG_CONFIG)
endif
SDL3_CFLAGS ?= $(shell $(SDL3_PKG) --cflags sdl3 2>/dev/null)
SDL3_LIBS   ?= $(shell $(SDL3_PKG) --libs sdl3 2>/dev/null)
SDL3_CHECK   = @$(SDL3_PKG) --exists 'sdl3 >= $(SDL3_MIN_VERSION)' 2>/dev/null || { echo "SDL3 >= $(SDL3_MIN_VERSION) not found by $(PKG_CONFIG) (sdl3.pc): install SDL3's development files, or set SDL3_DIR=<prefix> / PKG_CONFIG_PATH (README, Build)" >&2; exit 1; }
# The legacy build (Windows XP: SDL3 does not run there) - SDL=2 builds against SDL2 (2.0.18 or newer; src/sdlcompat.h maps
# the SDL3 calls the port makes), through pkg-config's sdl2 module or SDL2_DIR=<prefix> (the SDL2 MinGW development
# package's i686-w64-mingw32 folder). The SDL3 build is unaffected (sdlcompat.h is then <SDL3/SDL.h>).
ifeq ($(SDL),2)
  SDL2_MIN_VERSION = 2.0.18
  ifdef SDL2_DIR
    SDL2_PKG = PKG_CONFIG_LIBDIR="$(SDL2_DIR)/lib/pkgconfig" PKG_CONFIG_PATH= $(PKG_CONFIG) --define-variable=prefix=$(SDL2_DIR)
  else
    SDL2_PKG = $(PKG_CONFIG)
  endif
  SDL3_CFLAGS := $(filter-out -Dmain=SDL_main,$(shell $(SDL2_PKG) --cflags sdl2 2>/dev/null)) -DMW2_SDL2
  SDL3_LIBS   := $(shell $(SDL2_PKG) --libs sdl2 2>/dev/null)
  SDL3_CHECK   = @$(SDL2_PKG) --exists 'sdl2 >= $(SDL2_MIN_VERSION)' 2>/dev/null || { echo "SDL2 >= $(SDL2_MIN_VERSION) not found by $(PKG_CONFIG) (sdl2.pc): set SDL2_DIR=<prefix> (README, Build: the legacy build)" >&2; exit 1; }
  # no Roland SC-55 emulation in the legacy build (Nuked SC-55 needs most of a modern CPU core; keeps that build lean)
  CFLAGS += -DMW2_NO_SC55
  SC55_LIB =
  SC55_SRC =
else
  SC55_LIB = build/libnsc55.a
  SC55_SRC = src/sc55mid.c
endif
# The render backend (src/rb.h): SDL_GPU (default; Direct3D 12 / Vulkan / Metal), or RB=gl12 - OpenGL 1.2's fixed-function
# pipeline (src/rb_gl12.c, the legacy build for older PCs; docs/RENDERER_GL12.md), linked to the system's OpenGL
ifeq ($(RB),gl12)
  RB_SRC = src/rb_gl12.c src/rb_gl12_world.c
  CFLAGS += -DMW2_RB_GL12
else
  RB_SRC = src/rb_gpu.c src/rb_shaders.c
endif
# Windows: MSYS2's MINGW64 shell (uname MINGW64_NT-...), or cross-compiling from Linux with the SDL3 MinGW development
# package (SDL3-devel-<version>-mingw.tar.gz):
#   make mw2 WINDOWS=1 CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ SDL3_DIR=/path/to/SDL3-<version>/x86_64-w64-mingw32
ifneq (,$(findstring MINGW,$(UNAME_S)))
  WINDOWS = 1
  ifeq ($(origin CC),default)
    CC = gcc
  endif
endif
ifeq ($(WINDOWS),1)
  UNAME_S = Windows
  CFLAGS += -include src/wincompat.h
  ifeq ($(SDL),2)
    # the legacy build (32-bit, Windows XP SP3 and newer): XP's API level; SSE2 arithmetic as the 64-bit builds' (the
    # simulation's floats the same - README; a Pentium 4 / Athlon 64 or newer), the stack realigned for it in callbacks
    # from 4-byte-aligned Windows code
    LEGACY_FLAGS = -msse2 -mfpmath=sse -mstackrealign
    CFLAGS += $(LEGACY_FLAGS) -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DMW2_LEGACY_XP
  endif
  LDLIBS = -lm
  CXXLIB = -static-libgcc -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic   # no libstdc++ / libwinpthread DLLs to ship
  DLLIB =
  WINSRC = src/wincompat.c
  WINLIBS = -lole32 -lshell32   # system DLLs: the folder picker (wincompat.c mw2_pick_folder)
  WINRES = build/mw2res.o
  WINDRES ?= $(patsubst %gcc,%windres,$(CC))
else
  DLLIB = -ldl
  WINSRC =
  WINRES =
  WINLIBS =
endif
ifeq ($(UNAME_S),Linux)
  LDLIBS += -ldl
  CXXLIB = -lstdc++
endif
ifeq ($(UNAME_S),Darwin)
  CXXLIB = -lc++
  LDLIBS += -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
endif
ifeq ($(RB),gl12)
  ifeq ($(UNAME_S),Windows)
    RB_LIBS = -lopengl32
  else ifeq ($(UNAME_S),Darwin)
    RB_LIBS = -framework OpenGL
  else
    RB_LIBS = -lGL
  endif
endif

LIB_SRC = src/prj.c src/datapath.c src/cdaudio.c src/mek.c
BIN     = prjtool cdtool mektool bwdtool mechview mechview3d cdrip test_port shelltool xmiplay test_titanium test_gamefolder
GLBIN   = glshot glview mw2shell

all: $(BIN)

# Windows: the exe's icon and version information
build/mw2res.o: packaging/windows/mw2port.rc packaging/windows/mw2port.ico
	@mkdir -p build
	$(WINDRES) -I packaging/windows $< -O coff -o $@

build/miniaudio.o: third_party/miniaudio.c third_party/miniaudio.h
	@mkdir -p build
	$(CC) -O2 -w $(LEGACY_FLAGS) -c $< -o $@

xmiplay: tools/xmiplay.c src/gusmid.c src/shelldb.c src/gusmid.h src/shelldb.h
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@ -lm

shelltool: tools/shelltool.c src/shelldb.c src/smk.c src/shelldb.h src/smk.h
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -o $@

prjtool: tools/prjtool.c src/prj.c
	$(CC) $(CFLAGS) -Isrc $^ -o $@

cdrip: tools/cdrip.c
	$(CC) $(CFLAGS) $^ -o $@

HDRS = $(wildcard src/*.h tools/*.h)
# the renderer: the world (glr.c), the HUD (hud.c) and the render backend - on SDL_GPU (rb_gpu.c; the shaders compiled
# for Vulkan, Direct3D 12 and Metal in rb_shaders.c - generated, checked in: "make shaders" remakes it), or RB=gl12
GL3D_SRC = $(RB_SRC) src/hud.c src/shp.c src/glr.c src/skygnd.c src/anim.c src/mek.c src/mtbl.c src/msim.c src/fx.c src/combat.c src/ai.c src/mech3d.c src/render.c src/tex.c src/wtb.c src/bwd.c src/prj.c src/datapath.c

missionsim: tools/missionsim.c src/msim.c src/fx.c src/ai.c src/combat.c src/mtbl.c src/anim.c src/mek.c src/mech3d.c src/wtb.c src/bwd.c src/prj.c src/datapath.c $(HDRS)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc $(filter %.c,$^) -o $@ -lm

glshot: tools/glshot.c $(GL3D_SRC) $(HDRS)
	$(SDL3_CHECK)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc $(SDL3_CFLAGS) -DSDL_MAIN_HANDLED $(filter %.c,$^) -o $@ $(SDL3_LIBS) $(RB_LIBS) -lm

# the MT-32 emulator (Munt mt32emu, LGPL 2.1+): C++, built into a static library
CXX ?= c++
MT32_SRC = $(wildcard third_party/mt32emu/*.cpp) $(wildcard third_party/mt32emu/sha1/*.cpp) third_party/mt32emu/c_interface/c_interface.cpp third_party/mt32emu/srchelper/InternalResampler.cpp $(wildcard third_party/mt32emu/srchelper/srctools/src/*.cpp)
MT32_OBJ = $(patsubst third_party/mt32emu/%.cpp,build/mt32emu/%.o,$(MT32_SRC))
build/mt32emu/%.o: third_party/mt32emu/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w $(LEGACY_FLAGS) -DMT32EMU_WITH_INTERNAL_RESAMPLER=1 -Ithird_party/mt32emu -c $< -o $@
build/libmt32emu.a: $(MT32_OBJ)
	$(AR) rcs $@ $^

# the SC-55 emulator (Nuked SC-55, GPL 2.0+; the emulator core only, third_party/nuked-sc55): C++, a static library
NSC55_SRC = $(wildcard third_party/nuked-sc55/*.cpp)
NSC55_OBJ = $(patsubst third_party/nuked-sc55/%.cpp,build/nsc55/%.o,$(NSC55_SRC))
build/nsc55/%.o: third_party/nuked-sc55/%.cpp $(wildcard third_party/nuked-sc55/*.h)
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w $(LEGACY_FLAGS) -c $< -o $@
build/libnsc55.a: $(NSC55_OBJ)
	$(AR) rcs $@ $^

# mw2: one executable - the shell, and with --sim the simulation (glview built in, its main renamed)
mw2: $(WINRES) build/libmt32emu.a $(SC55_LIB) $(SC55_SRC) src/mt32mid.c tools/mw2shell.c tools/glview.c src/gusmid.c src/shelldb.c src/smk.c src/vidfx.c src/lance.c src/ttext.c src/cockpitcfg.c src/sfx.c src/portcfg.c src/cdaudio.c build/miniaudio.o $(GL3D_SRC) $(WINSRC) $(HDRS)
	$(SDL3_CHECK)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party $(SDL3_CFLAGS) -DSDL_MAIN_HANDLED -c tools/glview.c -Dmain=glview_main -o build/glview_sim.o
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party -Ithird_party/mt32emu -DMW2_ONE_BINARY -c tools/mw2shell.c $(SDL3_CFLAGS) -o build/mw2shell_one.o
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party/mt32emu -c src/mt32mid.c -o build/mt32mid.o
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party $(SDL3_CFLAGS) build/mw2shell_one.o build/glview_sim.o build/mt32mid.o $(sort $(filter-out src/mt32mid.c,$(filter src/%.c,$^))) build/miniaudio.o build/libmt32emu.a $(SC55_LIB) $(WINRES) -o $@ $(SDL3_LIBS) $(RB_LIBS) -lm -lpthread $(DLLIB) $(CXXLIB) $(WINLIBS)

mw2shell: build/libmt32emu.a $(SC55_LIB) $(SC55_SRC) src/mt32mid.c tools/mw2shell.c src/sfx.c src/gusmid.c src/shelldb.c src/smk.c src/vidfx.c src/lance.c src/mek.c src/prj.c src/portcfg.c src/ttext.c src/datapath.c src/shp.c src/cockpitcfg.c src/bwd.c
	$(SDL3_CHECK)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party/mt32emu $(SDL3_CFLAGS) -Ithird_party $(filter %.c,$^) build/libmt32emu.a $(SC55_LIB) -o $@ $(SDL3_LIBS) -lm -lpthread $(CXXLIB)

glview: tools/glview.c src/sfx.c src/portcfg.c src/cdaudio.c build/miniaudio.o $(GL3D_SRC) $(HDRS)
	$(SDL3_CHECK)
	$(CC) $(CFLAGS) -Wno-conversion -Isrc -Ithird_party $(SDL3_CFLAGS) $(filter %.c,$^) $(filter %.o,$^) -o $@ $(SDL3_LIBS) $(RB_LIBS) -lm -lpthread $(DLLIB)

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

test_titanium: tests/test_titanium.c src/tex.c src/skygnd.c src/portcfg.c src/prj.c src/datapath.c
	$(CC) $(CFLAGS) -Isrc $^ -o $@ -lm

test_gamefolder: tests/test_gamefolder.c src/portcfg.c src/portcfg.h
	$(CC) $(CFLAGS) -Isrc tests/test_gamefolder.c src/portcfg.c -o $@

# the SC-55 menu music without firmware: ROM set detection, the device choice, a stub ROM set run (tests/test_sc55.c)
test_sc55: tests/test_sc55.c src/sc55mid.c src/sc55mid.h src/shelldb.c src/portcfg.c build/libnsc55.a
	$(SDL3_CHECK)
	$(CC) $(CFLAGS) -Isrc -Ithird_party $(SDL3_CFLAGS) tests/test_sc55.c src/sc55mid.c src/shelldb.c src/portcfg.c build/libnsc55.a -o $@ $(SDL3_LIBS) -lm -lpthread $(CXXLIB)

test_cockpitcfg: tests/test_cockpitcfg.c src/cockpitcfg.c src/cockpitcfg.h
	$(CC) $(CFLAGS) -Isrc tests/test_cockpitcfg.c src/cockpitcfg.c -o $@

test_inputmap: tests/test_inputmap.c tools/inputmap.h tools/inputmap_defaults.h src/datapath.c src/datapath.h
	$(SDL3_CHECK)
	$(CC) -std=c99 -D_DEFAULT_SOURCE -Itools -Isrc $(SDL3_CFLAGS) tests/test_inputmap.c src/datapath.c $(SDL3_LIBS) -o $@

# make check MUSIC=/path/to/rip MW2=/path/to/install [ISO=/path/to/mw2cd.iso] [TITANIUM=/path/to/MW2-game/titanium]
TITANIUM ?= $(MW2)/../MW2-game/titanium
check: all
	./prjtool verify $(MW2)/MW2.PRJ
	./mektool $(MW2)/MW2.PRJ list | tail -1
	./bwdtool $(MW2)/MW2.PRJ verify
	./mechview $(MW2)/MW2.PRJ verify
	MW2_INSTALL_DIR=$(MW2) ./bwdtool $(MW2)/MW2.PRJ missions | tail -1
	./test_port $(MUSIC) $(MW2) $(ISO)
	./test_titanium $(TITANIUM)
	./test_gamefolder
	$(MAKE) test_inputmap && ./test_inputmap $(MW2)
	$(MAKE) test_cockpitcfg && ./test_cockpitcfg $(MW2)
	$(MAKE) test_sc55 && ./test_sc55 $(MW2)
	MW2_LAB_MASSES=1 MW2_CONFIG=/tmp/mw2check.cfg ./mw2 $(MW2) > /tmp/mw2lab.txt; r=$$?; tail -1 /tmp/mw2lab.txt; exit $$r

clean:
	rm -rf build $(BIN) $(GLBIN) mw2 mw2.exe test_sc55

.PHONY: all check clean

# the shaders (shaders/*.vert, *.frag) for every backend into src/rb_shaders.c / .h (checked in; needs glslangValidator,
# spirv-cross and dxc - on PATH or in SHADER_TOOLS=<folder>/bin; see scripts/gen_shaders.py)
shaders:
	python3 scripts/gen_shaders.py $(if $(SHADER_TOOLS),--tools $(SHADER_TOOLS))
.PHONY: shaders

# A self-contained folder to run from anywhere: the launcher, the simulation, the font (found next to the
# executables), the documentation. Needs SDL3 from the system (and a Vulkan driver: the GPU's).
DIST = dist/mw2port
dist: mw2
	rm -rf $(DIST)
	mkdir -p $(DIST)/assets/fonts
	cp mw2 $(DIST)/
	cp assets/fonts/LiberationSans-Bold.ttf assets/fonts/README.md $(DIST)/assets/fonts/
	cp README.md THIRD_PARTY.md LICENSE $(DIST)/
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

# the sequences against the OpenGL 1.2 renderer (RB=gl12): a glview built with it as build/glview_gl12 (the system's
# OpenGL - Mesa's llvmpipe headless); SEQ_ARGS="--only NAME,NAME" for the picture / look sequences
GL12_SRC = $(filter-out src/rb_gpu.c src/rb_shaders.c,$(GL3D_SRC)) src/rb_gl12.c src/rb_gl12_world.c
build/glview_gl12: tools/glview.c src/sfx.c src/portcfg.c src/cdaudio.c build/miniaudio.o $(sort $(GL12_SRC)) $(HDRS)
	$(SDL3_CHECK)
	@mkdir -p build
	$(CC) $(CFLAGS) -DMW2_RB_GL12 -Wno-conversion -Isrc -Ithird_party $(SDL3_CFLAGS) $(filter %.c,$^) $(filter %.o,$^) -o $@ $(SDL3_LIBS) $(if $(filter Darwin,$(UNAME_S)),-framework OpenGL,-lGL) -lm -lpthread $(DLLIB)
test_sequences_gl12: build/glview_gl12
	python3 tests/sequences/run_sequences.py --glview build/glview_gl12 $(SEQ_ARGS)
.PHONY: test_sequences_gl12

# the same under AddressSanitizer / UBSan (a separate binary, build/glview_asan)
test_sequences_asan: tools/glview.c src/sfx.c src/portcfg.c src/cdaudio.c build/miniaudio.o $(GL3D_SRC) $(HDRS)
	$(CC) -std=c99 -D_DEFAULT_SOURCE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Isrc -Ithird_party $(filter %.c,$^) $(filter %.o,$^) -o build/glview_asan $(SDL3_CFLAGS) $(SDL3_LIBS) $(RB_LIBS) -lm -lpthread $(DLLIB)
	ASAN_OPTIONS=detect_leaks=0 SEQ_TIME_SCALE=6 python3 tests/sequences/run_sequences.py --glview build/glview_asan $(SEQ_ARGS)
.PHONY: test_sequences test_sequences_asan

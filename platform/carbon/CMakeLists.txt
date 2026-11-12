# MechWarrior 2 - the classic Mac OS / Carbon build (PowerPC: Mac OS 9 with CarbonLib 1.6+, Mac OS X 10.1-10.4).
# Retro68 with Apple's Universal Interfaces 3.4 (see docs/CARBON.md):
#   mkdir build-carbon && cd build-carbon
#   cmake ../platform/carbon -DCMAKE_TOOLCHAIN_FILE=<Retro68>/toolchain/powerpc-apple-macos/cmake/retrocarbon.toolchain.cmake
#   make
cmake_minimum_required(VERSION 3.13)
project(MW2Carbon C)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(GAME_SRC
    ${ROOT}/tools/mw2shell.c ${ROOT}/tools/glview.c
    ${ROOT}/src/rb_gl12.c ${ROOT}/src/rb_gl12_world.c ${ROOT}/src/hud.c ${ROOT}/src/shp.c ${ROOT}/src/glr.c
    ${ROOT}/src/skygnd.c ${ROOT}/src/anim.c ${ROOT}/src/mek.c ${ROOT}/src/mtbl.c ${ROOT}/src/msim.c ${ROOT}/src/fx.c
    ${ROOT}/src/combat.c ${ROOT}/src/ai.c ${ROOT}/src/mech3d.c ${ROOT}/src/render.c ${ROOT}/src/tex.c ${ROOT}/src/wtb.c
    ${ROOT}/src/bwd.c ${ROOT}/src/prj.c ${ROOT}/src/datapath.c ${ROOT}/src/sfx.c ${ROOT}/src/portcfg.c ${ROOT}/src/cdaudio.c
    ${ROOT}/src/gusmid.c ${ROOT}/src/shelldb.c ${ROOT}/src/smk.c ${ROOT}/src/vidfx.c ${ROOT}/src/lance.c ${ROOT}/src/ttext.c
    ${ROOT}/src/cockpitcfg.c)
# (no MT-32 emulator on this build: menu music is Gravis patches or QuickTime's synth - mt32_none.c, qtmidi.c)
set(PLAT_SRC sdl_core.c sdl_plat_carbon.c gl_dyn.c posix_mac.c posix_mac_path.c atomic8.c qtmidi.c mt32_none.c ${ROOT}/third_party/miniaudio.c)

# the application icon (tools/mac_icon.py from the Windows icon) when there is one
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/icon.r)
    set(ICON_R icon.r)
endif()
add_application(MechWarrior2 TYPE APPL CREATOR MW2P ${GAME_SRC} ${PLAT_SRC} mw2.r ${ICON_R})

# Retro68 leaves the Finder flags 0: set "has bundle" in the MacBinary file so the Finder shows the icon
find_package(Python3 COMPONENTS Interpreter)
if(ICON_R AND Python3_FOUND)
    add_custom_command(TARGET MechWarrior2_APPL POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/macbin_bundle.py ${CMAKE_CURRENT_BINARY_DIR}/MechWarrior2.bin)
endif()

# newlib's maths library (sqrtf, floorf, lrintf ... and cexpf, which GCC makes of a sinf/cosf pair)
target_link_libraries(MechWarrior2 m)

target_include_directories(MechWarrior2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ROOT}/src ${ROOT}/third_party)
target_compile_definitions(MechWarrior2 PRIVATE _DEFAULT_SOURCE MW2_CLASSIC_MAC MW2_NO_SC55 MW2_ONE_BINARY SDL_MAIN_HANDLED
    MA_NO_DEVICE_IO MA_NO_THREADING MA_NO_ENGINE MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH)
target_compile_options(MechWarrior2 PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu99> -O2 -mcpu=750 -mtune=750 -ffp-contract=off -w)   # (G3 scheduling, runs on G4s; no fused multiply-add: the simulation's floats must round as on the other builds)
# the game's sources see the POSIX file layer; the platform files are its implementation
set_source_files_properties(${GAME_SRC} PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/include/posix_mac.h")
set_source_files_properties(${ROOT}/tools/glview.c PROPERTIES COMPILE_DEFINITIONS "main=glview_main")
# miniaudio: decoder only, no dynamic loading (no dlopen here), and the one POSIX call Retro68's libc lacks
set_source_files_properties(${ROOT}/third_party/miniaudio.c PROPERTIES COMPILE_DEFINITIONS "MA_NO_RUNTIME_LINKING"
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/include/mac_libc_extra.h")

#!/bin/sh
# builds the port shims for simdiff.py: tools/audit/build.sh [outdir]  (default: /tmp/simdiff_build)
set -e
D=$(cd "$(dirname "$0")/../.." && pwd)
O=${1:-/tmp/simdiff_build}
mkdir -p "$O"
CF="-std=c99 -D_DEFAULT_SOURCE -O1 -g -fPIC -w -I$D/src"
S="$D/src"
cc $CF -shared -o "$O/libshim_combat.so" "$D/tools/audit/shim_combat.c" $S/mek.c $S/prj.c $S/datapath.c -lm
cc $CF -shared -o "$O/libshim_ai.so" "$D/tools/audit/shim_ai.c" $S/mek.c $S/prj.c $S/datapath.c -lm
cc $CF -shared -o "$O/libshim_msim.so" "$D/tools/audit/shim_msim.c" $S/fx.c $S/ai.c $S/combat.c $S/mtbl.c $S/anim.c $S/mek.c $S/mech3d.c $S/wtb.c $S/bwd.c $S/prj.c $S/datapath.c -lm
echo "$O"

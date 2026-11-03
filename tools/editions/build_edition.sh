#!/bin/bash
# build_edition.ps1 for Linux: builds the NRO of another edition (app_<edition>, made by crear_arbol.py) with the same
# paths as the default tree, so the PGO profile of that edition matches (see build_edition.ps1).
#
#   bash tools/editions/build_edition.sh usa /path/to/mesa-switch/mesa-linux-install/opt/devkitpro/portlibs/switch
#
# While the build runs, app_<edition> is renamed to app (and app to app_es_aparte); whatever happens, both are renamed
# back at the end. The first run configures app_<edition>/out/sw8; later ones only build. A watchdog stops the build
# below 2.5 GB of free memory instead of letting the system swap to a halt: the link time optimization is the peak.
# JOBS sets the parallel jobs (4 by default, as build_edition.ps1). The result is app_<edition>/out/sw8/nfsmw.nro.
set -u
EDICION=${1:?usage: tools/editions/build_edition.sh <edition> <Mesa SDK portlibs/switch folder>}
SDK=${2:?usage: tools/editions/build_edition.sh <edition> <Mesa SDK portlibs/switch folder>}
JOBS=${JOBS:-4}
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
cd "$RAIZ" || exit 1
[ -e app_es_aparte ] && { echo "A previous swap did not finish: app_es_aparte exists" >&2; exit 1; }
[ -d "app_$EDICION" ] || { echo "app_$EDICION does not exist (tools/editions/crear_arbol.py creates it)" >&2; exit 1; }
[ -d "pgo/$EDICION" ] || { echo "There is no profile in pgo/$EDICION" >&2; exit 1; }
[ -f "$SDK/lib/libvulkan.a" ] || { echo "No lib/libvulkan.a in $SDK" >&2; exit 1; }
LOG=$RAIZ/build_$EDICION.log

restore() {
    [ -d app ] && [ -d app_es_aparte ] && mv app "app_$EDICION"
    [ -d app_es_aparte ] && mv app_es_aparte app
    [ -n "${VIGIA:-}" ] && kill "$VIGIA" 2>/dev/null
}
trap restore EXIT

(
    while sleep 5; do
        libre=$(awk '/MemAvailable/ {print int($2 / 1024)}' /proc/meminfo)
        if [ "$libre" -lt 2500 ]; then
            pkill -f 'cc1plus|lto1|lto-wrapper'
            pkill ninja
            echo "stopped for memory: $libre MB free at $(date)" >> "$RAIZ/build_memory.log"
        fi
    done
) &
VIGIA=$!

mv app app_es_aparte && mv "app_$EDICION" app || exit 1
inicio=$(date +%s)
if [ ! -f app/out/sw8/CMakeCache.txt ]; then
    cmake -S app -B app/out/sw8 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$RAIZ/tools/switch/cmake/switch-devkitA64.cmake" \
        -DREXSDK_DIR="$RAIZ/sdk" -DREXGLUE_SWITCH_NVK_SDK="$SDK" \
        -DNFSMW_PGO=usar -DNFSMW_BUILD_LAUNCHER=OFF > "${LOG%.log}_configure.log" 2>&1 ||
        { echo "cmake (configure) failed, see ${LOG%.log}_configure.log" >&2; exit 1; }
fi
cmake --build app/out/sw8 -j "$JOBS" > "$LOG" 2>&1
rc=$?
echo "END $(date) ($(( ($(date +%s) - inicio) / 60 )) min)" >> "$LOG"
[ "$rc" = 0 ] && echo "Built app_$EDICION/out/sw8/nfsmw.nro" || echo "cmake (build) failed with $rc, see $LOG" >&2
exit "$rc"

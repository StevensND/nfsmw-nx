#!/usr/bin/env bash
# Builds the deko3d NRO of the default tree (app/, PAL Spanish) in the devkitpro/devkita64 container (podman or
# docker): out/sw_dk/nfsmw.nro. No Mesa: the SDK is built without Vulkan (docs/HowToDeko3d.md).
#
#   tools/switch/build_deko3d.sh                 release deko3d library, no LTO (faster to iterate)
#   LTO=ON tools/switch/build_deko3d.sh          with LTO, as the released NVK NROs
#   DEPURAR=ON tools/switch/build_deko3d.sh      deko3d's debug library (every call checked, readable errors):
#                                                out/sw_dk_depurar/nfsmw.nro
#   JOBS=4 tools/switch/build_deko3d.sh          parallel compiles (default 8; each generated file needs ~1-2 GB)
#
# Needs the generated code (tools/codegen.sh app) and sdk/thirdparty with the SDK's sources, SPIRV-Cross and uam
# (python tools/fetch_thirdparty.py). On the SD card the NRO goes to sdmc:/switch/nfsmw-nx/ next to game_root/,
# nfsmw.toml and nfsmw_shaders_dk.nfsp (shaders/nfsmw_regenerar_biblioteca_pcf.sh with DEKO3D=1).
set -euo pipefail
cd "$(dirname "$0")/../.."
if [[ ! -f app/generated/default/sources.cmake ]]; then
    echo "app/generated is missing: run tools/codegen.sh app first (docs/building.md, step 5)" >&2
    exit 1
fi
if [[ ! -f sdk/thirdparty/uam/uam_api.h || ! -f sdk/thirdparty/SPIRV-Cross/spirv_glsl.cpp || ! -d sdk/thirdparty/fmt ]]; then
    echo "sdk/thirdparty is incomplete: run python3 tools/fetch_thirdparty.py" >&2
    exit 1
fi
engine=$(command -v podman || command -v docker || true)
[[ -n "$engine" ]] || { echo "podman or docker is required" >&2; exit 1; }
dir=out/sw_dk
[[ ${DEPURAR:-OFF} == ON ]] && dir=out/sw_dk_depurar
"$engine" run --rm -v "$PWD":/src:Z -w /src docker.io/devkitpro/devkita64:latest bash -c "
    set -e
    if [[ ! -f $dir/CMakeCache.txt ]]; then
        cmake -S app -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_TOOLCHAIN_FILE=/src/tools/switch/cmake/switch-devkitA64.cmake -DREXSDK_DIR=/src/sdk \
              -DREXGLUE_SWITCH_DEKO3D=ON -DREXGLUE_SWITCH_DEKO3D_DEBUG=${DEPURAR:-OFF} \
              -DNFSMW_LTO=${LTO:-OFF} -DNFSMW_BUILD_LAUNCHER=OFF > $dir.configure.log
    fi
    ninja -C $dir -j ${JOBS:-8}"
echo "built $dir/nfsmw.nro"

#!/usr/bin/env bash
# Builds the deko3d test NRO in the devkitpro/devkita64 container (podman or docker).
#   tools/switch/dk_prueba/build.sh            release deko3d library: out/dk_prueba/dk_prueba.nro
#   DEPURAR=ON tools/switch/dk_prueba/build.sh deko3d's debug library (every call checked, readable errors):
#                                              out/dk_prueba_depurar/dk_prueba.nro
# Copy the NRO to sdmc:/switch/nfsmw-nx/ with nfsmw_shaders_dk.nfsp and shadercache_dksh.bin next to it
# (shaders/nfsmw_regenerar_biblioteca_pcf.sh with DEKO3D=1 DKSH=...), start it from the Homebrew Menu and read
# logs/dk_prueba.log there. What to look for: the comment at the top of dk_prueba.cpp.
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [[ ! -f sdk/thirdparty/uam/uam_api.h || ! -f sdk/thirdparty/SPIRV-Cross/spirv_glsl.cpp ]]; then
    echo "Missing sdk/thirdparty/uam or SPIRV-Cross: run python3 tools/fetch_thirdparty.py --deko3d" >&2
    exit 1
fi
engine=$(command -v podman || command -v docker || true)
[[ -n "$engine" ]] || { echo "podman or docker is required" >&2; exit 1; }
dir=out/dk_prueba
[[ ${DEPURAR:-OFF} == ON ]] && dir=out/dk_prueba_depurar
"$engine" run --rm -v "$PWD":/src:Z -w /src docker.io/devkitpro/devkita64:latest bash -c "
    cmake -S tools/switch/dk_prueba -B $dir -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
          -DCMAKE_BUILD_TYPE=Release -DNFSMW_DEKO3D_DEPURAR=${DEPURAR:-OFF} &&
    ninja -C $dir"
echo "built $dir/dk_prueba.nro"

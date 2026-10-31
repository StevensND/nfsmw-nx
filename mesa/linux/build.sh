#!/bin/bash
# Builds the driver (mesa-switch with this port's patch) on Linux, with the options of mesa-switch's
# build-unified.sh, which only runs in MSYS2. The result is the SDK folder that the app needs:
#   <mesa-switch>/mesa-linux-install/opt/devkitpro/portlibs/switch
#
#   bash mesa/linux/build.sh /path/to/mesa-switch
#
# The mesa-switch tree must be at commit 1a8c1a66d6f with mesa/mesa-switch-nfsmw.patch applied (mesa/README.md).
# Needs Meson 1.4 or newer (MESON=... to use another one), devkitPro in /opt/devkitpro, LLVM 18 with libclc and the
# SPIR-V translator for the host tools (LLVM_CONFIG=... to use another one), and a Rust 1.85+ toolchain with the
# aarch64-unknown-linux-gnu target, bindgen-cli and cbindgen. docs/building-linux.md lists the packages.
#
# Two builds, as in build-unified.sh: first the host tools that compile the driver's internal shaders (mesa_clc,
# vtn_bindgen2) in builddir-native-host, only once; then the driver for the Switch in builddir-switch-linux.
set -euo pipefail
MESA=$(cd "${1:?usage: mesa/linux/build.sh <mesa-switch tree>}" && pwd)
AQUI=$(cd "$(dirname "$0")" && pwd)
MESON=${MESON:-meson}
LLVM_CONFIG=${LLVM_CONFIG:-$(command -v llvm-config-18 || command -v llvm-config || true)}
INSTALL=$MESA/mesa-linux-install
SDK=$INSTALL/opt/devkitpro/portlibs/switch

version=$("$MESON" --version)
if [ "$(printf '%s\n1.4.0\n' "$version" | sort -V | head -1)" != 1.4.0 ]; then
    echo "Meson $version is too old: Mesa needs 1.4.0 or newer (pip install meson, then MESON=...)" >&2
    exit 2
fi
[ -n "$LLVM_CONFIG" ] || { echo "llvm-config not found: set LLVM_CONFIG" >&2; exit 2; }

# What build-unified.sh exports, for Linux.
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
export MESA_SWITCH_RUSTC=${MESA_SWITCH_RUSTC:-$(command -v rustc)}
export MESA_SWITCH_BINDGEN=${MESA_SWITCH_BINDGEN:-$(command -v bindgen)}
export SOURCE_DATE_EPOCH=$(git -C "$MESA" show -s --format=%ct HEAD)
export ZERO_AR_DATE=1 LC_ALL=C TZ=UTC
cd "$MESA"

if [ ! -x builddir-native-host/src/compiler/clc/mesa_clc ] || [ ! -x builddir-native-host/src/compiler/spirv/vtn_bindgen2 ]; then
    echo "== host tools (LLVM $("$LLVM_CONFIG" --version))"
    printf "[binaries]\nllvm-config = '%s'\n" "$LLVM_CONFIG" > linux-host-native.ini
    "$MESON" setup builddir-native-host --wipe --native-file linux-host-native.ini \
        --buildtype=release \
        -Dvulkan-drivers= \
        -Dgallium-drivers= \
        -Dshader-cache=disabled \
        -Dplatforms= \
        -Dglx=disabled \
        -Degl=disabled \
        -Dopengl=false \
        -Dgles1=disabled \
        -Dgles2=disabled \
        -Dtools=[] \
        -Dllvm=enabled \
        -Dshared-llvm=enabled \
        -Dmesa-clc=enabled \
        -Dprecomp-compiler=enabled \
        -Dinstall-mesa-clc=true
    ninja -C builddir-native-host src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
fi

echo "== driver for the Switch"
setup_mode=()
[ -f builddir-switch-linux/meson-private/coredata.dat ] && setup_mode+=(--wipe)
"$MESON" setup builddir-switch-linux "${setup_mode[@]}" \
    --cross-file "$AQUI/switch_cross_file.txt" \
    --native-file "$AQUI/switch_native_tools.txt" \
    --default-library=static \
    --prefix=/opt/devkitpro/portlibs/switch \
    --libdir=lib \
    --buildtype=release \
    -Doptimization=2 \
    -Db_lto=false \
    -Db_ndebug=true \
    -Dvulkan-drivers=nouveau \
    -Dgallium-drivers=nouveau,zink \
    -Dgallium-rusticl=false \
    -Dplatforms=switch \
    -Degl-native-platform=switch \
    -Dglx=disabled \
    -Degl=enabled \
    -Dopengl=true \
    -Dgles1=enabled \
    -Dgles2=enabled \
    -Dvideo-codecs= \
    -Dshader-cache=enabled \
    -Dxmlconfig=enabled \
    -Dexpat=enabled \
    -Dtools=[] \
    -Dllvm=disabled \
    -Dshared-glapi=disabled \
    -Dshared-llvm=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dcpp_rtti=false \
    -Dbuild-tests=false
# "rustfmt is not installed" while building is harmless: bindgen only uses it to indent what it writes.
ninja -C builddir-switch-linux

echo "== install"
rm -rf -- "$INSTALL"
"$MESON" install -C builddir-switch-linux --destdir "$INSTALL" > /dev/null
for pc in "$SDK"/lib/pkgconfig/*.pc; do
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
done
[ -f "$SDK/lib/libvulkan.a" ] || { echo "no lib/libvulkan.a in $SDK" >&2; exit 1; }
echo "SDK for -DREXGLUE_SWITCH_NVK_SDK:"
echo "  $SDK"

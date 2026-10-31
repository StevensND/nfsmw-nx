#!/bin/bash
# devkitPro's Switch pkg-config metadata, as mesa-switch's switch-pkg-config-wrapper.sh does for MSYS2, without
# rewriting /opt/devkitpro into a Windows path.
export PKG_CONFIG_DIR=
export PKG_CONFIG_PATH=
export PKG_CONFIG_SYSROOT_DIR=
export PKG_CONFIG_LIBDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig
exec pkg-config --static "$@"

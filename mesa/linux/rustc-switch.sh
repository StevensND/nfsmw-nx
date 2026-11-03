#!/bin/bash
# rustc for the Switch build of Mesa on Linux: mesa-switch's rustc-switch-wrapper.sh, plus Meson's own probe.
#
# The wrapper builds for the Switch (MESA_SWITCH_RUST_TARGET), except Meson's sanity probe, which has to run on the
# host. It recognizes the probe by its file name, and Meson 1.5 and later call it meson-private/sanity.rs, a name the
# wrapper does not know: the probe was built for the Switch and linked as a host program, and Meson stopped with
# "Rust compiler ... cannot compile programs". Here that probe goes to the host rustc without the cross linker, and
# everything else to the wrapper.
#
# First argument: the mesa-switch tree (@GLOBAL_SOURCE_ROOT@ in switch_cross_file.txt).
MESA=$1
shift
for arg in "$@"; do
    case "$arg" in
        */meson-private/sanity.rs)
            args=()
            skip=0
            for a in "$@"; do
                if [ "$skip" = 1 ]; then
                    skip=0
                    case "$a" in linker=*) continue ;; esac
                    args+=(-C "$a")
                    continue
                fi
                case "$a" in
                    -Clinker=*) continue ;;
                    -C) skip=1 ;;
                    *) args+=("$a") ;;
                esac
            done
            exec "${MESA_SWITCH_RUSTC:-rustc}" "${args[@]}"
            ;;
    esac
done
exec /bin/bash "$MESA/rustc-switch-wrapper.sh" "$@"

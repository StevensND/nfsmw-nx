# Building on Linux (and WSL)

[building.md](building.md) describes the build as it was done on Windows. Everything except the graphics driver works
on Linux too, with a few differences, and the driver has its own Linux build in [`mesa/linux/`](../mesa/linux). This
page is the whole route on Linux. It was done on Ubuntu 24.04 inside WSL 2, building the USA edition; steps that are
the same as in building.md link there.

## What you need

Ubuntu 24.04 packages:

```sh
sudo apt install cmake ninja-build git python3 python3-pip python3-numpy pkg-config \
  clang-20 lld-20 libx11-xcb-dev libxtst-dev libwayland-dev \
  python3-mako python3-yaml bison flex zlib1g-dev libzstd-dev libexpat1-dev \
  llvm-18-dev libclang-18-dev libclc-18-dev libllvmspirvlib-18-dev spirv-tools glslang-tools
```

- **clang-20**: the code generator needs Clang 19 or newer. Clang 18, Ubuntu's default, fails on `std::expected`
  from GCC 13's library. The X11 and Wayland headers are only for the SDK's display module, which CMake configures
  even when only the code generator is built.
- **LLVM 18, libclc and the SPIR-V translator**: for the driver's host tools, which compile its internal shaders.
- **python3-numpy**: for the edition tools in `tools/editions/`.

Also:

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) in `/opt/devkitpro`, with `switch-dev` and the portlibs
  `switch-libexpat`, `switch-libzstd` and `switch-zlib` (`sudo dkp-pacman -S switch-dev switch-libexpat
  switch-libzstd switch-zlib`).
- **Meson 1.4 or newer.** Ubuntu 24.04 ships 1.3, so install a newer one with pip into a folder of its own, with a
  small script that runs it:

  ```sh
  python3 -m pip install --target ~/opt/pymeson meson
  printf '#!/bin/sh\nPYTHONPATH=%s exec python3 -m mesonbuild.mesonmain "$@"\n' ~/opt/pymeson > ~/opt/meson
  chmod +x ~/opt/meson
  ```
- **Rust** through [rustup](https://rustup.rs), 1.85 or newer, with the target the driver's Rust code is built for,
  and bindgen and cbindgen:

  ```sh
  rustup target add aarch64-unknown-linux-gnu
  cargo install --locked bindgen-cli cbindgen
  ```

  It was done with a nightly toolchain (1.98). Mesa refuses bindgen 0.72.0 and anything older than 0.71.1.
- **Memory**: about 16 GB, as on Windows. WSL 2 gets half of the PC's memory by default; raise it with `memory=` in
  `%UserProfile%\.wslconfig` if needed. The edition build script stops the build below 2.5 GB free instead of letting
  the system swap to a halt.

## 1. Get the missing libraries

The same as in building.md: `python3 tools/fetch_thirdparty.py`.

## 2. The game

`python3 tools/fase1_extraer.py path/to/NFSMW.iso -o assets/game_root` for the PAL Spanish edition, as in
building.md. For another edition, put its `default.xex` in a folder of its own under `assets/` (for example
`assets/usa/default.xex`): the code generator needs the executable to be inside its `game_root`.

## 3. Build the code generator

```sh
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3
cmake --build out/host --target rexglue
```

- `-march=x86-64-v3`: the SDK uses SSSE3 instructions, which Clang on Linux does not enable by default. The code
  generator only runs on the PC that builds it, so it can be built for that processor (v3 is any x86-64 processor with
  AVX2).
- The executable ends up in `sdk/out/linux-amd64/rexglue`, not in `out/host`.

## 4. Translate the game to C++

The scripts are run with `bash` and Python is `python3`:

```sh
REXGLUE=sdk/out/linux-amd64/rexglue PYTHON=python3 bash tools/codegen.sh app
```

For another edition, follow [editions.md](editions.md). The USA edition does not need the Spanish executable: see
[Building the USA edition with only a USA executable](editions.md#building-the-usa-edition-with-only-a-usa-executable).
Matching a new edition (`emparejar.py`) needs the decompressed images of both executables. The code generator writes
the image of the executable in a manifest and stops, without translating anything, when `REXGLUE_VOLCAR_IMAGEN` names
a file:

```sh
REXGLUE_VOLCAR_IMAGEN=$PWD/assets/pal_es.bin sdk/out/linux-amd64/rexglue codegen app/nfsmw_manifest.toml
```

It is the image as loaded, with the import thunks resolved, so its SHA-256 differs from the `image_sha256` of the
installer's manifest; the code and the sections are the same.

## 5. Build the graphics driver

```sh
git clone https://github.com/danfromtico/mesa-switch.git
cd mesa-switch
git checkout 1a8c1a66d6f
git apply ../nfsmw-nx/mesa/mesa-switch-nfsmw.patch
cd ../nfsmw-nx
MESON=~/opt/meson bash mesa/linux/build.sh ../mesa-switch
```

`MESON` is only needed when the `meson` in `PATH` is older than 1.4. `mesa/linux/build.sh` does what mesa-switch's
`build-unified.sh` does in MSYS2, with the same options:

1. It builds the host tools (`mesa_clc`, `vtn_bindgen2`) with LLVM 18 in `builddir-native-host`, only the first time.
2. It builds the driver for the Switch in `builddir-switch-linux`, with the cross file in `mesa/linux/`.
3. It installs it into `mesa-linux-install/` and prints the SDK folder for the next step:
   `<mesa-switch>/mesa-linux-install/opt/devkitpro/portlibs/switch`.

While it builds, "rustfmt is not installed" errors are harmless: bindgen only uses rustfmt to indent the code it
writes, and carries on without it. `mesa/linux/rustc-switch.sh` fixes the only problem found with mesa-switch's own
scripts on Linux: Meson 1.5 and later gave its Rust sanity check a name mesa-switch's wrapper does not recognize.

## 6. Build the NRO

The default tree (PAL Spanish), as in building.md:

```sh
cmake -S app -B app/out/sw8 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/tools/switch/cmake/switch-devkitA64.cmake \
  -DREXSDK_DIR=$PWD/sdk -DREXGLUE_SWITCH_NVK_SDK=<SDK folder from step 5> \
  -DNFSMW_PGO=usar -DNFSMW_BUILD_LAUNCHER=OFF
cmake --build app/out/sw8 -j 4
```

Another edition, with the Linux version of `build_edition.ps1`, which builds it from the paths of `app/` so the PGO
profile matches:

```sh
bash tools/editions/build_edition.sh usa <SDK folder from step 5>
```

The result is `app_usa/out/sw8/nfsmw.nro`. The PGO profiles were recorded on Windows: the game's code finds its profile
(its file names are relative to the build folder), but the SDK's carry the Windows paths of the PC that recorded them
and are built without one. That costs a little speed and is reported as warnings (`-Wcoverage-mismatch`,
`-Wmissing-profile`), not errors. A first build took about 8 minutes, and about 5 after a change in the SDK.

Then run the checks of [toolchain.md](toolchain.md) (`_start` at address 0 with
`/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm`, and `python3 tools/comprobar_clave_textura.py`).

## 7. Shaders and the SD card

The shader library is the same for a build made on Linux: use the one the installer page made from your disc, or
build it as in building.md. Copy the NRO to the SD card as in [step 8 of building.md](building.md#8-copy-it-to-the-sd-card).

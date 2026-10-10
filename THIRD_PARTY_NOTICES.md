# Third-party notices

NFSMW-NX is built on the work of other projects. Their licenses apply to their code and to the parts of this
repository derived from it.

## Projects this port is derived from

| Project | Used for | License |
|---|---|---|
| [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip | The recompilation project this port started from: `app/` (hooks, configuration, codegen setup) and tools | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | `sdk/`: code generator, Xbox 360 kernel and runtime, GPU command processing | BSD-3-Clause (`sdk/LICENSE`) |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev and contributors | `shaders/XenosRecomp`: Xenos microcode to HLSL translator | MIT (`shaders/LICENSE.md`) |
| [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico, on top of [Mesa](https://mesa3d.org) | The Vulkan driver (NVK) and shader compiler (NAK) for Horizon, linked into the NRO; `mesa/` holds the changes | MIT (Mesa's licenses per file) |

The Horizon (Switch) layer added to `sdk/` is under the SDK's BSD-3-Clause license, and the changes in `shaders/`
and `mesa/` under the licenses of those projects (MIT), so they can be reused by other ports. The rest of this
repository is GPL-3.0 (`LICENSE`).

## Libraries

Libraries used by the SDK, the NRO and the code generator, fetched from the submodules of ReXGlue SDK v0.10.0
(`tools/fetch_thirdparty.py`):

| Library | License |
|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: the WMV3 decoder for the cutscenes), from `wmarti/FFmpeg` at `0604b464c7cb` | LGPL-2.1-or-later (configured without GPL parts) |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression) | LGPL-2.1 |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others (see its LICENSE.txt) |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause |
| AES-128 by LuoPeng (`thirdparty/aes_128`) | MIT |
| [libnx](https://github.com/switchbrew/libnx) and the devkitA64 runtime (newlib, libstdc++) | ISC; newlib and GCC runtime licenses |

The deko3d renderer (in progress, `docs/HowToDeko3d.md`; linked into the deko3d test NRO and the PC shader tool
`shaders/dksh`, not into the released NROs yet), fetched with `tools/fetch_thirdparty.py --deko3d`:

| Library | License |
|---|---|
| [deko3d](https://github.com/devkitPro/deko3d) (devkitPro's package) | zlib |
| [uam](https://github.com/devkitPro/uam), as vendored by [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD) (`runtime/third_party/uam`, with the patches its `PATCHES.md` lists) | zlib for uam's own files, MIT for the Mesa parts |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | Apache-2.0 |

### FFmpeg and libmspack (LGPL)

This port modifies these files of the SDK's third-party tree; the modified copies are in `sdk/thirdparty`:

- `FFmpeg/libavcodec/fft_template.c` and `FFmpeg/libavcodec/mdct_template.c`
- `FFmpeg/config.h`, plus `FFmpeg/config_switch_aarch64.h` and `FFmpeg/config_nfsmw_wmv3.h` (new): the configuration
  for AArch64 Horizon and for a build limited to the WMV3 decoder
- `ffmpeg-overlay/codec_list.c`: registers the WMV3 decoder
- `CMakeLists.txt`: builds the libraries for the Switch

The rest of FFmpeg and libmspack is the unmodified upstream source at the commits pinned by ReXGlue SDK v0.10.0.
Everything needed to rebuild and relink the NRO with a modified version of either library is in this repository and
in [docs/building.md](docs/building.md).

## Installer page

The [installer page](https://github.com/StevensND/nfsmw-nx-installer) runs WebAssembly builds of the shader translator
and packer from `shaders/`, libmspack (LZX), and [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
v2025.1 (University of Illinois/NCSA Open Source License, with LLVM's license terms). Its own notices are in that
repository.

## Trademarks

Need for Speed and Need for Speed: Most Wanted are trademarks of Electronic Arts Inc. Nintendo Switch is a trademark of
Nintendo. Xbox 360 is a trademark of Microsoft. This project is not affiliated with or endorsed by any of them.

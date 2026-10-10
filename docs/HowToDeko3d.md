# Moving the renderer from NVK (Mesa) to deko3d

Written 2026-10-08, before any deko3d code exists in this repository. It is a starting guide: what to build, in which
order, and what the port of *The Legend of Zelda: The Wind Waker HD* to the Switch (SwitchWakerHD, repository
`ZeldaWWHDRecomp`) learned when it made the same move. That port went from OpenGL on Mesa nouveau to deko3d in
October 2026 and now ships deko3d only. Its notes are `docs/deko3d-plan.md` (in Spanish) and `docs/switch-port.md`
(rounds 39-47 and the retrospective). Paths written as `WWHD:path` are files in that repository.

## In short

- [deko3d](https://github.com/devkitPro/deko3d) is devkitPro's low-level graphics API for the Switch's GPU. It looks
  like Vulkan (memory blocks, command buffers, queues, fences, barriers, images, descriptors) but has **no pipelines,
  no render passes and no descriptor sets**, and it writes GPU commands directly, with no driver in between.
- Its shaders are **DKSH** binaries made by **uam**, a compiler that only accepts **GLSL**. This port's shaders are
  SPIR-V made from HLSL. So the shader path becomes HLSL → DXC → SPIR-V → SPIRV-Cross → GLSL → uam → DKSH. This,
  and the changes it needs in XenosRecomp's HLSL, is the largest and riskiest piece of work. Do it first (P0).
- What deko3d removes: pipeline creation (about 59 ms each on NVK), the pipeline cache and the background thread
  that creates pipelines ahead of time, NVK's CPU cost per draw, the 35-file Mesa patch, NAK's compiler revision
  number, and the MSYS2/Rust driver build. deko3d and uam come from devkitPro's packages.
- What deko3d does **not** fix: the GPU cost of the shaders (uam's code generator is not NAK, and this port's NAK
  tuning does not carry over), the game's main thread, the limit on the number of NvMap handles, and the GPU
  timestamp factor (1.627 on both drivers).
- In WWHD, deko3d fixed most of what Mesa had caused, starting with its first release. The frame rate went up (29.9
  against 28.1-29.4 fps on the same route), the shader freezes went away (18.9 s at Windfall before, none after), and
  Mesa's GL thread disappeared. Its total CPU per draw fell from ~9 µs (6.6 µs on the render thread plus ~2.3 µs on
  Mesa's GL thread, on another core) to ~7 µs on one thread. Its own performance work then took that to 4.3 µs. That
  later work matters as much as the move itself: plan P4 from the start.
- deko3d does not run on a PC. Keep the Vulkan renderer: it remains the PC build and the reference for every
  comparison, and the NVK NRO stays as the fallback until deko3d is at parity on the console.

## 0. Where it stands

Updated 2026-10-10. P0 and P1 are done and checked on the console. P2 and the race features of P3 run on the
console: the deko3d build draws the videos, the menus, the title screen and races with the image of the NVK build,
and run 5 was faster than the released NVK build at stock clocks (26-32 fps against 21-29). P4 (performance) is
under way: the deko3d build ran races at 28.8-32.3 fps at 1280x720 (Mesa on the same races: 26.2 and 29.2, with 4.5
frames of 100 ms or more per minute against 1.0 in the first pair, see
[performance-history.md](performance-history.md), step 9), and at 35.2 fps with the scene drawn at 960x540
(`nfsmw_resolucion_interna = escena_960x540`, run 12). What was done, each
console run and each bug found are in [deko3d-devlog.md](deko3d-devlog.md). The NVK build is unchanged.

| Piece | Files | State |
|---|---|---|
| deko3d mode of the translator and the HLSL helpers (3.1) | `shaders/XenosRecomp/shader_recompiler.cpp` (`printDeko3dConstants`), `shaders/shader_common.h` (`NFSMW_DEKO3D`) | Done. The same HLSL gives both libraries: DXC with `-DNFSMW_DEKO3D` and without `-fvk-invert-y` for deko3d. Without the define the Vulkan library is **byte-identical** to the released one (SHA-256 `a27aea23…`, checked with the installer's own WASM DXC and packer) |
| deko3d library | `shaders/nfsmw_regenerar_biblioteca_pcf.sh` with `DEKO3D=1` → `nfsmw_shaders_dk.nfsp` | Done: 207 containers, 152 shaders, all compile. Only the `Shader` and `ImageQuery` capabilities are left (no Int64, no buffer addresses, no descriptor indexing) |
| SPIR-V → GLSL for uam | `app/src/nfsmw_dk_glsl.*` | Done, for the PC and the Switch |
| DKSH cache file | `app/src/nfsmw_dk_cache_dksh.*` | Done (format version 2) |
| PC tool | `shaders/dksh/` (`nfsmw_dksh`) | Done: library (+ an optional pipeline list from the console) → DKSH cache and report |
| uam and SPIRV-Cross | `python tools/fetch_thirdparty.py --deko3d` → `sdk/thirdparty/uam` (SwitchWakerHD's copy at `2d147aa`, the 7 patches), `sdk/thirdparty/SPIRV-Cross` (`vulkan-sdk-1.4.363.0`) | Fetched, not committed, like the rest of `sdk/thirdparty` |
| Core: device, queue, work slots, upload ring, command memory, image heap with the tile fix, DKSH loading, descriptors, swapchain | `app/src/nfsmw_dk_nucleo.*` | Runs on the console (test NRO and the game) |
| Test NRO | `tools/switch/dk_prueba/` (`build.sh`, `DEPURAR=ON` for libdeko3dd) | Runs on the console (results below) |
| SDK and toolchain without Vulkan or Mesa | `-DREXGLUE_SWITCH_DEKO3D=ON`: `sdk/CMakeLists.txt`, `sdk/cmake/rexglue_switch.cmake` (`rex::switch_gpu` = deko3d), `tools/switch/cmake/switch-devkitA64.cmake` | Done |
| The game on deko3d, P1: provider, presenter (guest output mailbox, swapchain on the window's NWindow, letterboxed bilinear copy), ImGui drawer (Debug Menu, F3), compile worker (uam, 8 MB stack, 0x3B) | `app/src/nfsmw_dk_presentador.*`, `nfsmw_dk_compilador.*`; `nfsmw_nativo_sistema.cpp` under `NFSMW_DEKO3D`; `tools/switch/build_deko3d.sh` | Runs on the console: the game boots, the Debug Menu and sound work. The NVK configuration still compiles every app file |
| The renderer backend (`DestinosNativos`) on deko3d, P2 | `app/src/nfsmw_dk_render.h`, `nfsmw_dk_destinos.cpp`, `nfsmw_dk_dibujos.cpp`, `nfsmw_dk_texturas.cpp`; shared Xenos helpers in `nfsmw_nativo_xenos.h` | Draws videos, menus, title and races (0 rejected draws), with the NVK path's race features: cubemaps from resolved faces (car reflections, rear-view mirror), the shadow minimum, the deferred sky, early depth, resolves without copies, exposure read-back. Not done: occlusion queries, `nfsmw_tratamiento_visual` other than `original` |
| P4 (performance) | `nfsmw_dk_geometria.*` and the files above | Under way, one batch per console run (devlog): copies on the 3D engine, depth compression, useful-area and deferred clears, constants by differences, vertices and indices kept across frames, sampled texture checks, a per-draw GPU profile |

P0 results on the PC (`nfsmw_dksh`, default variants: all 8 alpha-test functions, cheap PCF, shadow minimum, no blur
and without 1/size for every pixel shader; every vertex shader with all its interpolators and with the position only):

- 1,283 requested variants → 702 distinct DKSH, **702 compiled, 0 failed**, no uam warning of any kind (no "integer
  division … decays to floating point", no fp64), no dynamic component index into a UBO vector.
- Host time per variant (SPIR-V → GLSL → DKSH): p50 4.2 ms, p90 11.3, p99 22.8, max 32.6. The DKSH cache is
  byte-identical between two runs (uam patch 6).

Decisions taken, and what they mean for the renderer:

- **Textures:** each sampler register R is one texture and one sampler at binding R (separate in HLSL, combined by
  SPIRV-Cross, the combination takes the texture's binding). The pair of `tfetch2DSombraMin` is a second 2D texture at
  binding 16 + R. The renderer binds `dkMakeTextureHandle(image, sampler)` at R (and 16 + R).
- **y:** no `-fvk-invert-y`. deko3d's clip-space y points up, as Direct3D's does, so the shaders' own convention is
  the right one. The test pattern checks it; front faces must be checked when culling is wired.
- **Specialization:** one DKSH per (SPIR-V, value masked to the bits that shader reads, variant).
  `MascaraEspecializacion` finds the bits in the SPIR-V. No vertex shader reads any; a pixel shader reads at most
  bits 1, 9, 15, 16-18, 19 and 23. The PC tool checks every mask against the GLSL.
- **Vertex shader outputs:** every library vertex shader writes all 22 interpolators (TEXCOORD0-15 at locations 0-15,
  COLOR0-5 at 16-21). NVK drops the unread ones when it links a pipeline; uam has no linking. So the vertex shader's
  DKSH variant is the set its pixel shader reads (`SpirvAGlsl`'s `salidas`). Position-only shadow-map vertex shaders
  shrink from 3-8 KB to about 2 KB of code.
- **The shared constants block** is 31 float4 (496 bytes) in deko3d mode. The 1/size pairs end at byte 488, past the
  23 float4 the Vulkan block declares. NVK reads past the declared array (strictly undefined in SPIR-V); uam's GLSL
  front end rejects it at compile time. The renderer must bind 496 bytes.

First console run of the test NRO (2026-10-09, handheld, GPU clock not set by the test):

- uam on the console: the 702 variants compile (0 failures) and are **byte-identical** to the PC's; the whole cache
  file is identical. Compile time per variant on the console: mean 58 ms, p50 42, p90 109, p99 235, max 363; 40.8 s
  for all 702.
- The core ran 2,400+ frames without a deko3d error or a command memory overflow.
- GPU timestamps ×1.627 add up to 812.6 ms against 816.0 ms of wall clock: the factor holds on deko3d too.
- Full-screen 1280x720 passes with 64x64 dummy textures: `p_000177` 6.37 ms, `p_000071` 4.63, `p_000023` 4.34,
  `p_000139` (the race composition) 2.43 (2.8-3.2 ms on NVK in a real frame, docs: not the same clock or textures),
  `p_000101` 1.90, `p_000131` 0.80. Not yet a fair comparison: the test must request the game's clocks, and the same
  passes must be timed on NVK.

What to run on the console now: build `tools/switch/dk_prueba` and copy `dk_prueba.nro`, `nfsmw_shaders_dk.nfsp` and
`shadercache_dksh.bin` to `sdmc:/switch/nfsmw-nx/`. Run it from the Homebrew Menu and read `logs/dk_prueba.log` there (the game writes `logs/nfsmw_NNN.log`, one per session; stderr and abort reports go to `logs/rex/`). It
covers uam on the console, the test pattern (orientation, depth, texture upload), all PC variants compiled again and
compared byte for byte, and the GPU time of every pixel shader on full-screen passes. The comment at the top of
`dk_prueba.cpp` says what the screen must look like.

## 1. Why leave NVK, and what deko3d changes

| On NVK / Mesa today | Evidence | With deko3d |
|---|---|---|
| Creating a pipeline costs ~59 ms (82 ms in a race), so the port keeps a `VkPipelineCache` and a list of pipelines that it creates again on a background thread at start | `docs/platform-notes.md`, `docs/native-renderer.md` | There are no pipelines. Shaders are bound on their own and each piece of state is set with its own call. The pipeline cache, the pipeline list and the pre-creation thread can go. **But** a new shader *variant* now costs a uam compile (about 95 ms on the console) unless it was compiled before: see section 3 |
| CPU cost per draw inside NVK: binding a pipeline 2.95 µs, plus descriptor sets, constant buffers and state. Several patches to `nvk_cmd_draw.c` and `vk_pipeline.c` exist to reduce it | `docs/mesa.md` | `dkCmdBuf*` calls write GPU methods into command memory. What a draw costs is what your own code emits, so skipping unchanged state is your job (section 5) |
| Set 4 by differences (a Mesa patch) | `docs/mesa.md` | `dkCmdBufPushConstants` updates a bound uniform buffer inside the command stream: earlier draws keep the old values and later draws see the new ones. It is the same idea, built in |
| ZCULL compiled out in mesa-switch, and three separate fixes to turn it on | `docs/mesa.md` | Built in, with the queue flag `DkQueueFlags_EnableZcull`. You must invalidate it yourself in some cases (section 6, item 6) |
| Memory the CPU only writes to must skip the CPU cache (`NVK_SWITCH_CPU_WRITE_MEM_UNCACHED`) | `docs/mesa.md` | You choose it per memory block: `DkMemBlockFlags_CpuUncached \| DkMemBlockFlags_GpuCached` |
| Uniform buffers read through the GPU's constant memory needed a patch (upstream hung the GPU) | `mesa/README.md` | Native: every deko3d uniform buffer is a hardware constant buffer |
| NAK bugs and tuning: `FADD32I` lost its clamp (the black sea), memory latency set to 200 cycles, `peephole_select` limit raised to 8 | `docs/mesa.md` | A different compiler (uam: Mesa 19's GLSL front end and nouveau's nv50_ir code generator), with its own bugs (section 6). The tuning is gone and the black sea must be checked again |
| Driver build: MSYS2, Rust, bindgen, seven build problems, a patch against `1a8c1a66d6f` | `docs/mesa.md` | `dkp-pacman -S deko3d uam`, plus uam vendored as a library (section 3.2) |
| Every `vkQueueSubmit` is an IPC call, and the WSI adds one more per present | `docs/platform-notes.md` | `dkQueueSubmitCommands` only queues the work. `dkQueueFlush` is the call into the system. You decide how many there are |
| `VK_EXT_memory_budget` gives the GPU memory in use | `docs/platform-notes.md` | Count your own memory blocks |

What WWHD measured after the move (on the console, same route, CPU 1020 MHz):

| | OpenGL (Mesa nouveau) | deko3d |
|---|---|---|
| Frame rate, Outset route (GPU 460.8 MHz) | median 28.1-29.4 fps | median 29.9 fps (first deko3d release) |
| Total CPU per draw | ~9 µs: render thread ~6.6 µs plus Mesa's GL thread ~2.3 µs on another core (stock clocks: 4,359 draws at 26 fps, 751 + 266 ms/s) | ~7 µs in the first deko3d build, all on the render thread, with no driver thread; 4.3 µs after its performance phase and three more rounds |
| CPU cores | The render thread plus Mesa's GL thread at 266-470 ms/s | The render thread only: about a quarter to half a core freed |
| Shader stutters | 18.9 s frozen on the first visit to Windfall | none with the offline cache: 7,680 DKSH loaded in 0.29 s at start |
| Memory | ~323 MiB of Mesa shader programs plus ~140 MiB of EGL/Mesa | ~400 MiB more room (the plan's estimate; its notes have no separate measurement) |

Here NVK has no GL thread, and the pipeline cache already removes most stutters, so not every WWHD number carries over
one to one. The gains to expect here: NVK's CPU per draw and the pipeline binds gone, no pipeline creation, no driver
to maintain, and more free memory. The one thing to measure early is GPU time with uam's shaders (section 4, step
P0.4).

## 2. Vulkan concepts and their deko3d replacements

The Vulkan calls live in `app/src/nfsmw_nativo_dibujos.cpp` (133 call sites), `app/src/nfsmw_nativo_destinos.cpp`
(118), `app/src/nfsmw_nativo_sistema.cpp` (19: device and presenter), `app/src/nfsmw_nativo_texturas_pool.cpp`,
`app/src/nfsmw_shader_vulkan.cpp`, `app/src/nfsmw_video_vulkan.cpp`/`nfsmw_video_present.cpp`, and the SDK's
`sdk/src/ui/vulkan/` (provider, presenter, immediate drawer, which draws the Debug Menu).

| Vulkan (as used here) | deko3d |
|---|---|
| `VkInstance`, `VkDevice` | `DkDevice` with `DkDeviceFlags_OriginUpperLeft \| DkDeviceFlags_DepthZeroToOne`, and `cbDebug` set (section 6, item 1) |
| One graphics `VkQueue` | One `DkQueue` (`DkQueueFlags_Graphics`). Set the ZCULL flag here |
| `VkDeviceMemory`, `vkMapMemory`, `vkFlushMappedMemoryRanges` | `DkMemBlock`: uncached for upload rings and command memory; `CpuCached \| GpuUncached` plus `dkMemBlockFlushCpuCache` for read-back |
| `VkBuffer` | A GPU address (`dkMemBlockGetGpuAddr` + offset). Buffers have no objects |
| `VkImage`, `VkImageView` | `DkImageLayoutMaker` → `dkImageLayoutInitialize` → `dkImageInitialize` on your own sub-allocated memory; `DkImageView` for format, swizzle, mips and layers |
| `VkSampler` | `DkSampler` written into a sampler descriptor |
| Descriptor sets, pools, layouts; the bindless heaps (`g_Texture2DDescriptorHeap[]` …) | Two descriptor tables (`dkCmdBufBindImageDescriptorSet`, `dkCmdBufBindSamplerDescriptorSet`), filled with `dkCmdBufPushData`. Per draw, `dkCmdBufBindTextures(stage, slot, dkMakeTextureHandle(image, sampler))` into up to 32 slots per stage |
| Set 4 dynamic UBOs (`g_UboVertex`, `g_UboPixel`, `g_UboCompartidas`) | `dkCmdBufBindUniformBuffer(stage, slot, addr, size)` with addresses from the upload ring; changed pieces through `dkCmdBufPushConstants` |
| `vkCmdPushConstants` with 64-bit addresses (`RawBufferLoad`) | Removed: uam's GLSL front end has no buffer references. Everything goes through UBOs (section 3.1) |
| `VkPipeline`, `VkPipelineLayout`, `VkPipelineCache`, spec constants | `dkCmdBufBindShaders` (one `DkShader` per stage) plus state structs: `DkRasterizerState`, `DkColorState`, `DkColorWriteState`, `DkBlendState`, `DkDepthStencilState`, `DkVtxAttribState`/`DkVtxBufferState`, viewports and scissors. Spec constants become compile-time variants |
| `VkRenderPass`, `VkFramebuffer`, `vkCmdBeginRenderPass` | `dkCmdBufBindRenderTargets` plus explicit barriers. There are no load or store operations; it is an immediate-mode GPU anyway (`docs/platform-notes.md`) |
| `vkCmdClearColorImage`, `vkCmdClearDepthStencilImage` | `dkCmdBufClearColor`/`dkCmdBufClearDepthStencil` on the **bound** targets. They respect the scissor (section 6, item 9) |
| `vkCmdCopyImage`, `vkCmdBlitImage`, `vkCmdCopyBufferToImage` | `dkCmdBufCopyImage`, `dkCmdBufBlitImage` (the 2D engine; WWHD always copies depth this way), `dkCmdBufCopyBufferToImage` (copy engine, linear to block-linear) |
| `vkCmdPipelineBarrier` | `dkCmdBufBarrier(DkBarrier_Fragments / Full, DkInvalidateFlags_*)` (section 5.6) |
| `vkCmdBindVertexBuffers`, `vkCmdBindIndexBuffer`, `vkCmdDraw(Indexed)` | `dkCmdBufBindVtxBuffers`, `dkCmdBufBindIdxBuffer`, `dkCmdBufDraw(Indexed)`. Quads, fans and line loops are native (`DkPrimitive_Quads` …) |
| Query pools (timestamps, statistics) | `dkCmdBufReportCounter` writes a 16-byte report to a GPU address. There are no pools, so nothing to reset |
| `VkFence`, `vkQueueSubmit` | `DkFence` (`dkCmdBufSignalFence`, `dkFenceWait`), `dkQueueSubmitCommands` + `dkQueueFlush` |
| Swapchain and ReXGlue's `VulkanPresenter` (IMMEDIATE) | `DkSwapchain` on `nwindowGetDefault()` (3 images), `dkQueueAcquireImage`/`dkQueuePresentImage`, `dkSwapchainSetSwapInterval(…, 0)` |

## 3. Shaders: the first thing to build

### 3.1 From SPIR-V to DKSH

uam (devkitPro, version 1.1.0) takes GLSL 4.60 only. It has no SPIR-V input. The path:

```
XenosRecomp HLSL (built with a deko3d define, e.g. NFSMW_DEKO3D)
  → DXC -spirv            (the same tool as today)
  → SPIRV-Cross           (GLSL 460, not ES, without Vulkan semantics)
  → uam                   (GLSL → DKSH, one binary per stage and variant)
```

The HLSL needs these changes, behind the define, so that the Vulkan library stays as it is:

1. **Remove the bindless heaps.** `shaders/shader_common.h:136-139` declares `g_Texture2DDescriptorHeap[]`,
   `g_Texture3DDescriptorHeap[]`, `g_TextureCubeDescriptorHeap[]` and `g_SamplerDescriptorHeap[]`, and indexes them
   with `*_DescriptorIndex` values read from the shared constants (`shaders/XenosRecomp/shader_recompiler.cpp:1237-1241`,
   and the fetch helpers around lines 306 and 366). uam would need bindless textures for that, which deko3d does not
   offer here. In the deko3d version, each Xenos fetch slot that a shader uses becomes one texture at a fixed binding
   (the slot number, or a compact numbering below 32), with one type per slot (2D, 3D or cube). In GLSL that is a
   combined `sampler2D`/`sampler3D`/`samplerCube`. Use SPIRV-Cross's combined image samplers (check
   `--combined-samplers-inherit-bindings`), or DXC's `[[vk::combinedImageSampler]]`. The renderer then binds one
   handle per slot: the image of the fetch constant and its sampler, made with `dkMakeTextureHandle`.
2. **Remove the `vk::RawBufferLoad` path and the push constants** (`shader_common.h:84` and the `NFSMW_UBO ? … :
   RawBufferLoad` macros on lines 99-113). They are not used at run time when the UBO bit is set, but DXC still emits
   them, and SPIRV-Cross would then write `GL_EXT_buffer_reference`, which uam's front end cannot parse.
3. **Make the specialization constant a compile-time value.** `[[vk::constant_id(0)]] const uint g_SpecConstants`
   (`shader_common.h:115`). When SPIRV-Cross writes GLSL without Vulkan semantics, it turns a spec constant into a
   `SPIRV_CROSS_CONSTANT_ID_0` macro with a default value. Define that macro before compiling each variant. Check the
   real output on one shader first. A DKSH is identified by (shader fingerprint, spec value).
4. **Give every resource an explicit binding.** uam rejects resources without one, and its numbers are deko3d's slot
   numbers: 16 UBOs, 32 samplers and 8 images per stage. Default uniforms outside a block are an error.
5. **Decide how to flip y**, once (section 6, item 2). Either build the deko3d vertex shaders without
   `-fvk-invert-y` (`shaders/XenosRecomp/dxc_compiler.cpp:47`), or keep it and use a `NegativeY` viewport swizzle.
   Check it with a test pattern in P1.
6. **Use UBO layouts that std140 accepts.** `-fvk-use-dx-layout` with `float4` arrays already fits. The video
   shaders ask for `scalarBlockLayout` (`app/src/nfsmw_video_present.cpp:26`): check what they read.

### 3.2 uam: reuse WWHD's vendored copy

`WWHD:runtime/third_party/uam` is uam 1.1.0 (zlib for uam's own files, MIT for Mesa's) built with CMake as a static
library, `uamlib`, behind `uam_api.h`. Its `PATCHES.md` lists seven patches; you want all of them:

| Patch | Why it matters here |
|---|---|
| 1. Real mutexes | uam is still **not reentrant** (a static `gl_context`): compile on one thread only |
| 2. DKSH to memory | The stock tool only writes files |
| 3. Log callback | Compile warnings and errors go to your log |
| 4. No `exit()`/`abort()` | A bad shader fails the compile instead of killing the game |
| 5. Resident front end | About 20 % faster compiles |
| 6. Deterministic scheduling data | Stock uam read uninitialised memory, so the same GLSL could give a different DKSH (89 of 1,561 shaders did) |
| 7. **No dual issue** | uam's dual-issue scheduling made 4x8-pixel blocks (one 32-thread warp) flicker with wrong lighting. Found by bisecting on the console. Same frame rate without it |

`WWHD:tools/switch/dksh_cache` (host tool) and `uamtest2` (a Switch NRO that compiled 1,561 shaders and compared the
bytes with the host) are the models for P0.

### 3.3 Where the compile runs

The library is made from the player's disc, so its DKSH cannot be shipped either.

| Option | Cost | Use |
|---|---|---|
| **Host tool** (DXC + SPIRV-Cross + uam on the PC) | Host p50 9 ms per shader | From day one: compile the **whole** library and every variant, count failures and warnings. It is the P0 gate |
| **On the console at first start**, from the library's SPIR-V with SPIRV-Cross and uam linked into the NRO | uam on the console: mean 70-95 ms, p90 204, p99 403, max 1,077 ms per shader. The ~113 pipelines of a race are ≤226 compiles, about 20 s once (an estimate) | The first release. It is what WWHD runs for the shaders its offline cache lacks. Show a progress bar, save to the SD card |
| **In the installer page** (uam built with Emscripten next to DXC) | Unknown: nobody has built uam for WebAssembly | Later, if the first start is too slow. No compile on the console at all |

Rules that WWHD's compile worker follows:

- one thread, with an 8 MB stack;
- priority **0x3B**. 0x3C is invalid for an application: thread creation failed with 0xE001 and ended two builds at
  start;
- it sleeps on a condition variable when it has no work, and never spins (section 7);
- a self-test compile at start, with a log line `uam ready` or `SELF-TEST FAILED`;
- while a variant is still compiling, its draws are skipped and counted, never stalled.

**Cache key:** GLSL hash + uam version + the patch set + your converter's revision. This is the lesson of
`nvk_shader.c`'s revision number again: when any of them changes, old DKSH must not be loaded. WWHD rejects a cache
with another id and logs a line saying so.

**Loading:** copy the DKSH into a code memory block (`DkMemBlockFlags_Code`). Each shader starts at a multiple of
`DK_SHADER_CODE_ALIGNMENT`, and the last `DK_SHADER_CODE_UNUSABLE_SIZE` bytes of the block are unusable. Then call
`dkShaderInitialize`. Code loaded in the middle of a frame needs the shader caches invalidated before it runs
(`DkInvalidateFlags_Shader`). deko3d only does that at `dkQueueFlush`.

**The renderer's own shaders** (present, Debug Menu, video conversion if it is not from the library, FPS text): write
them in GLSL, compile them at build time with devkitPro's `uam`, and embed the DKSH in the NRO.

## 4. Order of work

Each phase ends with a check on the console. WWHD's plan estimated 6-9 weeks. With the work split into parallel
lanes (P2 in three, P4 in four), P0 to P5 took a few days, and the console test cycles set the pace.

### P0: shader feasibility (no renderer)

1. Build uam as a host tool and as a Switch library (from WWHD's vendored copy).
2. Add the deko3d define to XenosRecomp and `shader_common.h` (section 3.1), and produce a deko3d library with the
   same tools (`nfsmw_regenerar_biblioteca_pcf.sh`).
3. Write the host tool: library + list of (shader, spec value) pairs → DKSH files and a report. Take the variants
   from the pipeline list in `cache/nfsmw_nativo_pipelines.bin`.
4. **Measure GPU speed early.** A small Switch NRO that draws a full-screen quad with the heaviest pixel shaders of a
   race (the scene, the water, the five-layer smoke effect), with dummy constants and textures, timed with
   `dkCmdBufReportCounter`. Compare with the same draw on NVK. uam's code is not NAK's, and this port is often limited
   by the GPU. If uam is much slower, you need to know now.
5. On the console: compile the whole set with the Switch library and compare the bytes with the host (WWHD:
   1,561/1,561 identical).

**Exit:** 100 % of the shaders and variants compile. Every uam warning is read, in particular "integer division …
decays to floating point" (section 6, item 13). The bytes are identical on host and console, and the GPU time of the
heavy shaders is known.

### P1: device, memory, presentation, Debug Menu

- Device, queue, memory blocks (upload ring and command memory per work slot, code, images, descriptors, queries),
  swapchain, a test pattern that shows orientation and depth, the FPS counter, and the Debug Menu's ImGui drawn with
  deko3d.
- The game runs with the renderer doing nothing. The PM4 reader must keep writing `SCRATCH_REG` and answering waits,
  or the game freezes within a second (`docs/native-renderer.md`).
- ReXGlue's `rex::ui::Presenter` and immediate drawer are Vulkan on the Switch (`sdk/src/ui/vulkan`). You need a deko3d
  version of whatever the Debug Menu and the SaltyNX block use. `WWHD:runtime/src/gfx/deko/overlay_dk.cpp` is a
  working ImGui renderer for deko3d. The SaltyNX block says `api = kApiVulkan` (`sdk/src/ui/switch_saltynx.cpp:208`):
  check what the overlays do with that field.

**Exit:** the pattern is right, the menu works, and the game runs for 10 minutes without a crash.

### P2: the first real frames (videos, menus, title)

Surfaces and uploads (reuse the fingerprints and NEON untiling as they are), descriptors, state, draws, clears,
resolves and copies, the present pass. Videos and menus first: they have no 3D and few shaders.

**Exit:** the menus look the same as captures from the NVK build, no draws skipped with a warm cache, and CPU µs per
draw measured by stage. **Go/no-go**, with criteria fixed before you start. WWHD's were: at least 25 % less render
thread time per draw than the old path, no visual regression, an offline cache that misses at most ~20 %, and stable
uam memory. Add one of your own: GPU time per frame no worse than NVK in a paused race.

### P3: races at parity

Shadows, reflections, the mirror, the exposure read-back (section 5.7), water (check the black sea area again: the
NAK bug is gone, but uam has its own), the effect passes, videos. Build the capture tool (section 7) at the start of
this phase, not after the first wrong texture.

**Exit:** the same images as NVK in paused-race comparisons, and 30 minutes without a crash.

### P4: performance

In this order, because it is the order of what WWHD gained: skip unchanged state (5.2), UBO pieces through push
constants (5.3), cached texture handles (5.4), barriers only where there is a real hazard (5.6), ZCULL (section 6,
item 6), native quads and fans (5.5), how often to submit (5.8), and docked 1080p with a swapchain made again when
the console changes mode (5.9). Each one behind a switch in the Debug Menu.

### P5: deko3d by default

The deko3d NRO becomes the default, and the NVK NRO stays as a fallback for one or two releases. The installer page
builds the deko3d library variant (and the DKSH, if section 3.3's WebAssembly option is done). Update `README.md`,
`docs/mesa.md` and `docs/building.md`.

## 5. Notes on each part of the renderer

### 5.1 Structure

Keep everything that does not depend on the API: PM4 reading, the register shadow, shader identification, texture
fingerprints and untiling, resolve-without-copy logic, guards and native game functions. The only part that changes
is what records the GPU work. WWHD copied its OpenGL backend file by file and replaced the calls, without a shared
layer, so that GL stayed untouched as the fallback. Here the Vulkan renderer must also stay the PC reference, and the
draw logic is in large files (`nfsmw_nativo_dibujos.cpp` has 13,000 lines). A copy would mean fixing every frontend
bug twice. Recommendation: first move the Vulkan calls behind a thin backend boundary inside the existing renderer,
with no change in behaviour, tested on PC and console. Then write the deko3d backend behind the same boundary. Do not
force pipelines into deko3d, or state structs into Vulkan: the boundary is "record this draw with this state", not a
generic graphics API.

### 5.2 State

- The pipeline key splits into a shader key (VS, PS, spec value) and state blocks. Keep
  `static_assert(std::has_unique_object_representations_v<T>)` on every block that is hashed or compared.
- Remember what was emitted last for each block and emit only what changed. WWHD's "register generations" (a
  counter per category of registers, advanced when one of them changes value) saved ~0.2 µs per draw, about 1.5 fps
  at 6,900 draws.
- Pipeline barriers, render passes and pipelines no longer cut the command stream, so long runs of draws with the same
  targets cost almost nothing more than their own state.

### 5.3 Constants

- The three blocks (VS 256 vec4, PS 224 vec4, shared 23 vec4) come from the upload ring, bound with
  `dkCmdBufBindUniformBuffer` per stage.
- WWHD's mode that worked: one slice per shader and frame, written whole the first time, then only the changed 16-byte
  pieces through `dkCmdBufPushConstants`, with no rebind. A version that marked unchanged vec4 gained nothing in
  WWHD, because that game changes its constants on almost every draw. Here "set 4 by differences" measured 1 write
  instead of 4, so it should pay off. Measure it.

### 5.4 Textures and descriptors

- Two descriptor tables. WWHD: images 64..8191 and samplers 16..1023 for the game, the low slots reserved for the
  renderer (overlay, present).
- **Write descriptors only with `dkCmdBufPushData`**, ordered with the commands. Never write with the CPU a descriptor
  the GPU may still be reading. A freed slot is reused only after the fence of its frame.
- Cache per draw: (fetch constant words, sampler words) → handle, valid while the surface has not changed (WWHD's
  epoch counters). Its table caught 85 % of the misses of the per-unit cache in WWHD's logs. Surfaces that share an address (a render target sampled
  later) need their own rule: WWHD cut its misses from 1,420 to 44 per frame with one write counter per address.
- The `1/size` constants stay as they are: the size is still the Switch-side image's size.

### 5.5 Geometry

- Upload ring memory: `CpuUncached | GpuCached`. The vertex copy thread and its byte swap stay as they are.
- `DkVtxAttribState` per input location (SNORM, UNORM or float as now) and `DkVtxBufferState` per stream. WWHD built
  the attribute layout once per (fetch layout, vertex shader) and kept it on the shader.
- Xenos type 13 (quad list) is converted to triangle lists today (`nfsmw_nativo_dibujos.cpp:2270`). deko3d draws
  quads, fans and line loops natively, without index lists. Watch flat varyings: a native quad uses its last vertex
  for both of its triangles. Keep the provoking vertex that Vulkan uses today (the first vertex:
  `DkProvokingVertex_First` in the rasterizer state).

### 5.6 Barriers and caches

What each one costs in deko3d 0.5.0 (from its source):

- `DkBarrier_Fragments` waits for the pixel work in flight and invalidates the texture caches, with a wait for idle.
- `DkBarrier_Full` makes the host wait for the 3D, 2D and copy engines and stop prefetching commands.
- `dkQueueFlush` already invalidates image, shader and descriptor caches and L2 at the start of the next submission.

Start with a barrier after every change of render targets and every upload. Once the image is right, track hazards
per surface: a draw that samples what was written since the last barrier, or writes what was sampled since then.
Writes after writes need nothing. WWHD went from 47 to 20 `Fragments` barriers per frame this way.

### 5.7 Read-back to guest memory (the 64x64 exposure images)

`dkCmdBufCopyImageToBuffer` into a `CpuCached | GpuUncached` block, `dkMemBlockFlushCpuCache` on it before the GPU
writes, a fence, then `dkMemBlockFlushCpuCache` again before the CPU reads. WWHD's Picto Box fix
(`guest_writeback`) and its PNG captures use exactly this.

### 5.8 Submits and command memory

- Command memory per work slot. `dkCmdBufClear` (0.5.0) rewinds to the start of the last chunk added and does not ask
  for new memory: add the slot's chunk again after its fence each frame (the `CCmdMemRing` pattern from
  deko_examples), and set an out-of-memory callback.
- How often to submit is a trade-off. In WWHD, submitting every 1,024 draws instead of every 256 gained 0.6-0.8 fps. Here
  splitting a frame into two submissions left the GPU idle for 3.6 ms on NVK. Put the number behind a setting and
  measure.

### 5.9 Presentation and resolution

- A swapchain of 3 RGBA8 images. Swap interval 0 is today's IMMEDIATE.
- WWHD makes the swapchain again when `appletGetOperationMode` changes (`dkQueueWaitIdle`, destroy, create; a few ms),
  so docked 1080p and handheld 720p follow the console without a restart. Today this port keeps the start-up
  resolution until the game restarts. With deko3d that limit can go.

### 5.10 Memory

- Create few, large memory blocks at start and sub-allocate (as the texture pool does now: 16 MB blocks). Every memory
  block is still an NvMap handle, so `__nx_nv_transfermem_size` (`nfsmw_switch_nvmap_mb`) still matters.
- Color render targets with `DkImageFlags_HwCompression`: fine in WWHD. Depth compression: off. It is untested on
  deko3d, and on nouveau copies through the copy engine broke it.

## 6. Pitfalls WWHD hit (check each one)

1. **The release library aborts without saying why.** On any error, `libdeko3d` ends the process with a 2359-xxxx
   result and never calls the debug callback. Creations never return null. During bring-up, link the debug library
   (`libdeko3dd`): its callback can log the error and end the process with a readable message
   (`WWHD:runtime/src/gfx/deko/backend.cpp:145`). Flush the log before every creation, and check
   `dkQueueIsInErrorState` before acquire, submit, present and flush (a GPU fault puts the queue in that state, and
   any of those calls then aborts).
2. **Clip-space y points up in deko3d 0.5.0**, whatever the origin flag. `DkDeviceFlags_YAxisPointsDown` came after
   0.5.0 and is not in devkitPro's package (checked 2026-10-08 in `devkitpro/devkita64`: deko3d 0.5.0-1, uam 1.1.0-1,
   libnx 4.12.0-1). **Never use a negative viewport height**: deko3d computes the height as an unsigned integer and it
   overflows. Either negate y in the vertex shader, or use `dkCmdBufSetViewportSwizzles` with `DkSwizzle_NegativeY`
   (`WWHD:runtime/src/gfx/deko/draw.cpp:940-965`). Check which face is the front face after the flip.
3. **Tile height bug in deko3d 0.5.0.** For an image whose level 0 has 6 to 8 rows of 8-row blocks (for example BC
   textures 24-32 pixels tall), the layout and the descriptor disagree about the tile height. The second column of
   blocks of level 0 lands on mip 1, and the mip 1 upload overwrites it. Symptom: small squares, or a flat sea without
   its normal map. Fix: before every `dkImageLayoutInitialize`, apply WWHD's `image_tile_size_fix`
   (`WWHD:runtime/src/gfx/deko/memory.cpp:243`), which sets `DkImageFlags_CustomTileSize` with the shrunk tile:

   ```cpp
   void image_tile_size_fix(DkImageLayoutMaker& m, uint32_t rows) {  // rows: level-0 height in blocks
       if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) return;
       if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D ||
           m.type == DkImageType_Buffer) return;
       if ((m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) &&
           (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x)) rows *= 2;
       const uint32_t gobs = (rows + rows / 2 + 7) / 8;  // deko3d's pickTileSize
       uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
       uint32_t shrunk = tile;                           // its level-0 shrink (calcLevelOffset)
       while (shrunk && (8u << (shrunk - 1)) >= rows) shrunk--;
       if (shrunk == tile) return;
       m.flags |= DkImageFlags_CustomTileSize;
       m.tileSize = DkTileSize(shrunk);
   }
   ```
4. **Depth textures** return (D, D, D, 1) in deko3d, against (D, 0, 0, 1) in WWHD's GL path. Check what the shadow
   and depth reads of this game's shaders expect.
5. **sRGB render targets** (not confirmed): deko3d 0.5.0 never writes Maxwell register 0x56E (framebuffer sRGB). If
   something drawn into an sRGB target and sampled later comes out too dark, look here first.
6. **ZCULL.** deko3d drops a depth buffer's ZCULL data only when the bound depth address changes. After an upload, a
   copy or a blit into a depth image, or when the image at an address is replaced, invalidate it yourself
   (`DkInvalidateFlags_Zcull`). WWHD turned ZCULL off by default (`DkQueueFlags_DisableZcull`) because of flickering
   4x8 blocks. Those turned out to be uam's dual issue (item 14), and ZCULL was not tested again after that. Here
   ZCULL was worth about 2 ms of GPU per frame: once you have uam patch 7, put it behind a Debug Menu switch and
   compare.
7. **Tiled cache:** deko3d 0.5.0 never enables Maxwell's tiled cache. WWHD now disables it explicitly every frame.
   Turning it on is untested.
8. **Settings WWHD kept after its bisection:** polygon offset without a clamp (as Mesa does; deko3d wrote
   `PA_SU_POLY_OFFSET_CLAMP`), wait for idle before invalidating descriptors, invalidate the shader caches after
   loading code in the middle of a frame, tiled cache disabled explicitly.
9. **Clears respect the scissor.** Set the scissor to the whole target before a clear. Everything outside the draw
   path that binds targets, shaders or state (clears, present, overlay) must tell the draw state cache to forget
   what it emitted.
10. **Depth-only draws:** WWHD binds an empty embedded pixel shader rather than none. Most shadow casters of that game
    kept their pixel shader because of the alpha test. This port already builds pipelines without a pixel shader
    when it can: check what deko3d accepts.
11. **Code memory:** shaders aligned to `DK_SHADER_CODE_ALIGNMENT`, and the last `DK_SHADER_CODE_UNUSABLE_SIZE` bytes of
    the block left unused.
12. **uam loses a dynamic component index into a UBO vector:** `glyphs[i >> 2][i & 3]` read `.x` every time. A
    dynamic array index is fine; a dynamic index into the components of a `vec4` is not. XenosRecomp's
    `v[(B) / 16][((B) % 16) / 4]` is safe when `B` is a constant. Search the GLSL for any other case and rewrite it
    with selects.
13. **Integer division and modulo by a non-constant value** become floating-point divisions in uam, with a warning.
    Treat that warning as a bug to look at.
14. **uam's dual issue** (patch 7) and **uninitialised scheduling data** (patch 6): without them, warp-sized blocks of
    pixels flicker, and the same GLSL can give different binaries.
15. **GPU timestamps:** the GPU timer runs at 19.2 MHz, and deko3d converts it as if it ran at 31.25 MHz. The same
    1.627 factor as on NVK. Keep calibrating against the wall clock as `docs/measuring.md` says.
16. **Threads:** the compile worker at 0x3B, as above. The render thread's priority did not need changing in WWHD (it
    was already 0x2C). Here the ring thread is at 0x2D because of the presenter, so check again once the presenter
    is deko3d.

Found by this port (details in [deko3d-devlog.md](deko3d-devlog.md)):

17. **`dkCmdBufBindVtxBuffers` writes more than it reserves** (deko3d 0.5.0, `maxwell/gpu_3d_vbo.cpp`): it reserves 5
    words per buffer and writes 6. A command buffer made with `dkCmdBufCreate` has no slack words, so near the end of
    a command memory piece up to `numBuffers - 1` words go past it. If the next piece starts right there, the next
    command overwrites them and the GPU reports a method error (`[b197:7c4] = 0x200205f2` here: a method header read
    as the limit of vertex stream 2). Bind one buffer per call, and leave a gap at the end of every piece you give
    `dkCmdBufAddMemory`.
18. **`std::thread::detach()` ends the process on the Switch.** libnx has no `pthread_detach`, so it throws
    `std::system_error` ("Function not implemented"). Join every thread.

## 7. How to work and test

- **deko3d does not run on a PC.** Test shared code on the PC Vulkan build. Test deko3d only on the console, and make
  every console run count: start-up self-tests (uam compile, layouts of every format on the first frame, the FPS
  counter's glyphs) and one log line per active option.
- **A capture button from P3 on.** WWHD: both sticks pressed → the next frame's render targets, every sampled texture
  as the GPU has it, and the same texture decoded by the CPU from the upload data, as PNG files, with a log line per
  file naming the draws that used it. Right in the upload and wrong on the GPU means layout, view or sampling; wrong
  in both means format or decoding. That is how the tile bug (item 3) was found in one round.
- **Every option and A/B switch in the Debug Menu**, switchable while the game runs and logged with the frame it
  changed at. This port already works this way. Change one thing at a time.
- **Stage timers per draw** in a report line every 5 s (lookup, indices, textures, constants, state, submit), sampled
  the way `docs/measuring.md` says.
- **No busy helper threads.** WWHD moved its guest copies to a helper thread at a high priority. The helper never
  slept, the game's sound threads starved (18,398 "kill DSP channel" lines), and the frame rate dropped. Reduce work
  instead. The existing vertex copy thread is fine; add nothing that spins.
- **Know which limit you are working on.** WWHD's renderer rounds raised dense views from 22 to 29 fps and did nothing
  at sea, where the game's main thread was the limit. This port's ring thread and GPU have the same problem: look at
  each core's load before choosing a target.
- **Keep the NVK NRO buildable** until P5, with the same settings file, for A/B on the console.

## 8. Licenses

deko3d: zlib. uam: zlib for its own files, MIT for the Mesa parts. SPIRV-Cross: Apache-2.0. All of them can be used in
a GPL-3.0 program. Add them to `THIRD_PARTY_NOTICES.md` when they are linked into the NRO.

## 9. Files to read in WWHD

| File | What it has |
|---|---|
| `docs/deko3d-plan.md` | The plan, its phases and each phase's results (Spanish) |
| `docs/switch-port.md`, rounds 39-47 and the retrospective | Performance work on deko3d, what gained and what did not |
| `runtime/third_party/uam/PATCHES.md`, `uam_api.h` | uam as a library, the patches |
| `runtime/src/gfx/deko/memory.cpp` | Memory blocks, rings, the code block, `image_tile_size_fix` |
| `runtime/src/gfx/deko/backend.cpp` | Device, error handling, frames, present, swapchain recreation, GPU pass timing |
| `runtime/src/gfx/deko/draw.cpp` | The draw path: state cache, viewport flip, native primitives |
| `runtime/src/gfx/deko/gpu_sync.cpp`, `dk_sync.h` | Hazard-tracked barriers, ZCULL, tiled cache |
| `runtime/src/gfx/deko/descriptors.cpp` | Descriptor slots, `dkCmdBufPushData`, recycling |
| `runtime/src/gfx/deko/shaders_dk.cpp`, `shader_files.cpp` | The compile worker, the DKSH cache format and its key |
| `runtime/src/gfx/deko/capture.cpp` | The PNG capture tool and read-back |
| `runtime/src/gfx/deko/overlay_dk.cpp` | ImGui on deko3d |

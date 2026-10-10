# deko3d port: development log

What was done to move the Switch renderer from NVK (Mesa) to deko3d, in order, with what each console run showed and
why each decision was taken. The plan and the background are in [HowToDeko3d.md](HowToDeko3d.md); its phase names
(P0-P5) are used here. Newest entries are at the bottom.

All of this is on the branch `deko3d-implementation`, uncommitted unless an entry says otherwise. The console logs,
crash reports and captures each entry cites are kept on the PC under `out/consola/<run>/` (not committed: they are
game output). On the console they come from `sdmc:/switch/nfsmw-nx/logs/`, `capturas/` and
`/atmosphere/crash_reports/`.

## Summary

| Date | Phase | What | Result |
|---|---|---|---|
| 2026-10-08 | P0 | Shader path HLSL → SPIR-V → GLSL → DKSH, deko3d library, PC tool, DKSH cache | 702/702 variants compile, 0 uam warnings |
| 2026-10-08 | P0.4, P0.5, P1 core | Test NRO `dk_prueba` on the console | Console DKSH byte-identical to the PC's; core ran 2,400+ frames; test pattern right |
| 2026-10-09 | P1 | The game on deko3d with an empty renderer (presenter, ImGui, compile worker) | Boots, Debug Menu and sound work |
| 2026-10-09 | P2a | `RenderDk`: render targets, resolves, clears, draws, textures, output pass | Intro video and loading screen right; crash after 20 s (`std::thread::detach`), fixed |
| 2026-10-09 | P2a run 2 | 36 min: menus, title, career, a story race at stock clocks | Everything draws. Two problems: GPU method error in a race, overexposed 3D. Both fixed, waiting for the next run |
| 2026-10-09 | Run 3 | The same race on deko3d and on the released NVK build, stock clocks | No crash, exposure right. deko3d smoother (no pipeline stalls) but GPU-bound at 35-43 ms per frame against NVK's 19-24 ms: the NVK path's GPU savings were missing. First batch ported |
| 2026-10-09 | Run 4 | A race with the first batch, GPU time by category | 22-27.5 fps. Copies 10.5 ms per frame against NVK's 1.6-2.7: the biggest gap. Second batch: swaps, shadow minimum, fewer barriers, cubemaps, deferred sky, deferred depth clears, vertex dedupe |
| 2026-10-09 | Run 5 | A race with the second batch | 26-32 fps, ahead of NVK's 21-29 in the same race; image right (car reflections, mirror, shadows, sky). Still GPU-bound (30-37 ms). Third batch: output ramp as a texture, 3D-engine copies, depth compression, useful-area clears, deferred color clears, front buffer from its target, constants by differences, cross-frame texture cache |
| 2026-10-09 | Run 6 | The third batch, a race of about 8 minutes (log `out/consola/p2b3/nfsmw_010.log`) | 28-35 fps at 1,400-2,500 draws per frame; GPU 17-30 ms per frame. Not yet compared line by line with run 5. Log 011 (after it) is a Mesa run |
| 2026-10-10 | Music, fourth batch | No music in races. An inverted XMP notification was fixed in the SDK, but it was not the cause (run 7). Fourth batch: vertices and indices kept across frames, indices converted in one pass, sampled texture rechecks, a per-draw GPU profile | Run 7 |
| 2026-10-10 | Run 7 | The fourth batch in a race (log `out/consola/p2c1/nfsmw_013.log`); one crash at start-up (012) | 25-32 fps at 2,000-2,650 draws per frame. Upload 10 MB → 0.1 MB per frame, but the ring thread's time per draw barely moved (hashing cost as much as copying). Now CPU-bound: the GPU waits 3-8 ms per frame for the ring thread, the 3 cores are 80-90 % busy. Still no music. Fifth batch: sampled geometry checks, profile details, music diagnostics |
| 2026-10-10 | Run 8 | Sampled geometry checks; native audio validated (not the cause); the gate function never runs | 23-33 fps. Geometry checks 7.2 → 1-1.9 MB per frame but the vertex stage barely moved. The 3.6 ms draw is a 320x180 16-tap downsample of the compressed scene target. Sixth batch: decoder hook and mix dump for the music, compression test, texture reuse, CPU stack profile |
| 2026-10-10 | Run 9 | Heritage quick race, stack-sampled CPU profile, compression off | Music plays in quick races (2-channel segments); career races use 4-channel streams: the bug is there. 20-35 fps, GPU-bound in Heritage. Compression not the downsample's cause. Ring thread hot spots: Dibujar, constant differences, store lookups, XXH3 |
| 2026-10-10 | Run 10 | A race (2-channel music, like run 9), the repeat test in the GPU profile | 27-37 fps, GPU-bound (26-36 ms per frame). The downsample n77 costs 3.6 ms the second time too: its own cost, ~20x slower per texture read than the full-screen n19. The mix dump ended before the race; the hooked decoder never ran. The texture-check sampling switched itself off again (one texture) |
| 2026-10-10 | Run 11 | `nfsmw_resolucion_interna = escena_960x540` (the game draws at 960x540 and the frame is scaled to the screen), per-draw profile with variants | 33-55 fps at the 960x540 scene, GPU 20-29 ms per frame (27-37 fps and 26-36 ms at 1280x720). Bloom, shadows and reflections came out shifted to the left: resolved textures were created at the copy destination's pitch (256 for a 240-wide image). The bloom downscale (3.6 ms at 1280x720) is 1.74 ms; its cost is in the reads of the resolved texture, not in the arithmetic or the filter |
| 2026-10-10 | Run 12 | The width fix, 11 minutes of racing at 960x540 (log `out/consola/p2c6/nfsmw_019.log`) | Fixed: the bloom textures are 240x134 and 120x67, and the user reports the image right. **35.2 fps on average over 59 race windows (28.0-47.7)**, 2,187 draws per frame, GPU 24.6 ms per frame, no crash. The GPU now waits 5.3 ms per frame for the ring thread: the frame is balanced between the two |

## The goal

A playable game **at the Switch's stock clocks** in handheld mode: CPU 1020 MHz, GPU 307.2 MHz, memory 1331.2 MHz,
with no performance profile and no overclock. The deko3d build must use less CPU and less GPU than the NVK build, not
just match it, so the comparison that counts is both builds on the same race at those clocks. The console's test
settings file therefore has `nfsmw_switch_gpu_mhz = 0` (the game asks the system for nothing); a higher clock, when
wanted for a comparison, is set by hand with the console's clock overlay.

## How to read the numbers

- **Two builds of the same code.** `nfsmw-dk.nro` links deko3d's release library. `nfsmw-dk-depurar.nro` links the
  debug library (`libdeko3dd`), which checks every call and turns errors into readable log lines; it is slower. Every
  run below says which one it was.
- **The renderer's report** is the `[deko3d] ultimos 10 s` line, one every 10 s. Until 2026-10-09 evening it mixed
  per-interval values (frames per second, draws per frame) with running totals since start (copies, clears,
  textures, uploads, barriers). Divide the difference between two lines by the frames in between to get per-frame
  values. From that date on every value is per frame or per interval, except those marked "en total".
- **The ring thread** (the thread that reads the game's GPU commands and records the deko3d work) reports
  `[nativo] tiempos: anillo N ms/s; dibujos X us/dibujo` every 20 s: how busy it was, and its time per draw.
- **The clocks** are in `logs/rex/rex_perfil.log`, one block per 10 s (`relojes: CPU ..., GPU ..., memoria ...`). The
  game asks for GPU 460.8 MHz in handheld mode (configuration `0x92220008`); the console's default is 307.2.

## 2026-10-08: P0, the shader path

Done on the PC with the real shader library of the PAL Spanish disc. Details and figures are in
[HowToDeko3d.md](HowToDeko3d.md), section 0.

- XenosRecomp gets a deko3d mode (`printDeko3dConstants`, `NFSMW_DEKO3D` in `shader_common.h`): fixed texture slots,
  no descriptor heaps, no push constants, no 64-bit integers, and no `-fvk-invert-y` (deko3d's clip-space y points
  up, as Direct3D's does). Without the define the Vulkan library is byte-identical to the released one.
- `nfsmw_regenerar_biblioteca_pcf.sh` with `DEKO3D=1` makes `nfsmw_shaders_dk.nfsp`: 207 containers, 152 shaders.
- `app/src/nfsmw_dk_glsl.*` turns SPIR-V into GLSL for uam; `nfsmw_dk_cache_dksh.*` is the DKSH cache file (format 2);
  `shaders/dksh/` is the PC tool that fills it.
- Found on the way:
  - The shared constants block is declared as 23 float4, but the 1/size pairs reach byte 488. NVK reads past the end;
    uam refuses. The deko3d mode declares 31 float4, so the renderer binds 496 bytes.
  - Every vertex shader writes all 22 interpolators. NVK drops the unread ones when it links a pipeline; uam cannot.
    The vertex shader's DKSH variant is therefore the set of interpolators its pixel shader reads.
  - A shader's specialization value is masked to the bits it reads (`MascaraEspecializacion`). Vertex shaders read
    none: one compile per vertex shader, not one per pipeline.
  - `tools/fetch_thirdparty.py` crashed on Linux on a broken symlink in moltenvk (fixed). `rom/` added to `.gitignore`.
- Result: 1,283 requested variants, 702 distinct DKSH, 702 compiled, 0 failed, 0 uam warnings; 4.2 ms median per
  variant on the PC; the cache file is identical between two runs.

## 2026-10-08: the test NRO on the console (P0.4, P0.5, P1 core)

`tools/switch/dk_prueba/` exercises the core (`app/src/nfsmw_dk_nucleo.*`) without the game. Log: `out/consola/p1/dk_prueba.log`.

- uam on the console: all 702 variants compile and are byte-identical to the PC's; mean 58 ms per variant, 40.8 s for
  all of them.
- The core ran 2,400+ frames without a deko3d error or a command memory overflow.
- GPU timestamps ×1.627 add up to the wall clock (812.6 against 816.0 ms): the factor from the NVK days holds.
- Test pattern, checked on a screenshot: quadrant colors in the right corners (y is right without `-fvk-invert-y`),
  depth order right, texture orientation right.
- GPU time of full-screen passes with the game's pixel shaders: the race composition `p_000139` took 2.43 ms (2.8-3.2
  on NVK in a real frame). Not a fair comparison: the test did not ask for the game's GPU clock and used 64x64 dummy
  textures.

## 2026-10-08 and 09: P1, the game on deko3d with an empty renderer

- `-DREXGLUE_SWITCH_DEKO3D=ON` builds the SDK without Vulkan, Mesa or the Xenos plugin
  (`sdk/CMakeLists.txt`, `sdk/cmake/rexglue_switch.cmake`, `tools/switch/cmake/switch-devkitA64.cmake`).
  `tools/switch/build_deko3d.sh` builds the NRO in the devkitPro container.
- New: the provider, presenter (guest output mailbox, swapchain on the window, letterboxed copy) and ImGui drawer in
  `nfsmw_dk_presentador.*`; the uam compile worker in `nfsmw_dk_compilador.*` (8 MB stack, priority 0x3B).
  `nfsmw_nativo_sistema.cpp` keeps its ring and PM4 reading, with `NFSMW_DEKO3D` guards around the Vulkan parts.
- The renderer backend only cleared the guest output to dark blue on every Swap.
- Fixed on the way, also affecting the NVK build: `sdk/thirdparty/CMakeLists.txt` looked for `.git` folders and
  refused fetched sources; the SDK took the port's `v1.0.0` tag as its own version and stopped configuring.
- The deko3d core's own log lines (device and memory creation, the uam self-test, fatal errors) went nowhere in the
  game build. They now go to `logs/nfsmw_NNN.log`, and the log is flushed before anything that can end the process.
- Console (logs in `out/consola/p1/`, two sessions of 18 s and 2 min; the folder also holds an Atmosphère report from
  `hbloader` that was not looked into): the user confirmed the game boots, the Debug Menu works and sound plays.

## 2026-10-09: the P2 design decision

The question was what the NVK build teaches. Its ring thread set the pace (about 2,000 draws per race frame, 32-35
fps), and about 9 µs of CPU per draw went there, of which about 3.4 µs was inside NVK: draw 1.5, pipeline bind 3-3.7
on one draw in three, descriptor set 4 1.3, plus 0.5-0.8 for the pipeline lookup. Five Mesa patches, the extended
dynamic state experiments and the pipeline cache and prewarm thread won back only 0.6-1.5 µs of that. Creating a
texture cost 0.75 ms even from pooled memory.

Decision: a **deko3d-native backend** behind the existing `DestinosNativos` interface, not a line-by-line copy of the
Vulkan files. Everything up to "record this draw" is reused as is: the PM4 reading, the register shadow, shader
identification, texture fingerprints and NEON untiling, and each draw's decisions (what to skip, which shader
variant, the framing). The recording part is designed for deko3d:

- no pipelines, render passes or descriptor sets; each piece of state is emitted only when it changed since the last
  draw (`Grabado`);
- textures bound as handles; image creation is a layout over memory the renderer already owns (microseconds);
- texture uploads and descriptor writes go into a per-slot **pre-list** submitted before the slot's draws and ended
  with one Full barrier, as the Vulkan path's upload command buffer;
- quads drawn natively.

About half of the 19,000 Vulkan backend lines are NVK workarounds and measurements with no place here.

The Xenos helpers both backends need (tiling, formats, fetch constants) moved verbatim from `nfsmw_nativo_dibujos.cpp`
into `app/src/nfsmw_nativo_xenos.h`. The NVK configuration still compiles.

## 2026-10-09: P2a, the first frames

Files: `nfsmw_dk_render.h` (the `RenderDk` class), `nfsmw_dk_destinos.cpp` (frames and work slots, render targets,
resolves, clears, the output pass with the game's gamma ramp, the DKSH cache file, the report), `nfsmw_dk_dibujos.cpp`
(each draw), `nfsmw_dk_texturas.cpp` (textures, samplers, descriptors). `nfsmw_dk_presentador.cpp` gained
`CaptureGuestOutput`, so the existing PNG captures (`nfsmw_captura_cada_s`) work on deko3d.

Rules learned while writing it (all in [HowToDeko3d.md](HowToDeko3d.md) sections 5-6): flip with the viewport
swizzle, never a negative viewport height; clears respect the scissor; Full barriers around copy-engine transfers;
descriptors written with `dkCmdBufPushData` only; `DkCompareOp` and `DkStencilOp` are the Xenos value + 1;
`DkImageSwizzle` is the Vulkan swizzle − 1; `dkCmdBufCopyBufferToImage` takes the rectangle in texels and `DkCopyBuf`
row length and layer size in bytes.

### Run 1 (debug library). Logs: `out/consola/p2a/`

- The loading screen and the intro video were right: colors, orientation, text, gamma ramp.
- 0 draws rejected; 14 shaders from the PC cache, 3 exact vertex shader variants compiled in the background.
- 26.4 fps in the intro with the debug library. Per frame, 3.6 resolve copies and 7.1 Full barriers. (A note made at
  the time said "about 70 barriers per frame": it divided the running total by one interval. The report has been
  fixed since, see above.)
- **Crash after ~20 s, both launches.** `rex_crash.log`: `std::terminate: std::system_error: Function not
  implemented`. Symbolized with `addr2line` on the debug ELF: `RenderDk::GuardarCacheSiHaceFalta`, which saved the
  DKSH cache from a detached `std::thread`. libnx has no `pthread_detach`, so `std::thread::detach()` throws.
  The first save came when the background compiles finished. Fix: one member thread, joined before the next save
  and in the destructor. No other Switch code detaches a thread.

### Run 2 (debug library). Logs: `out/consola/p2a2/`

A 36-minute session (16:48-17:24): intro, title, attract video, a 16-minute console sleep, career menus, the car lot,
and a story race. Everything that is implemented draws, with 0 rejected draws. Three problems:

#### Crash in the race: a deko3d bug

At 17:24:38, 7 minutes into the race:

```
[dk] deko3d dkFenceWait: Queue (0) entered error state
[dk] deko3d dkFenceWait:   GPU method error (irq 0x00100000)
[dk] deko3d dkFenceWait:   [b197:7c4] = 0x200205f2
```

Then the renderer's own check (`la cola de deko3d esta en error`) ended the process: the `abort()` in
`rex_crash.log` and the Atmosphère report `01791559478` are that, symbolized to `Fatal` ← `Nucleo::TerminarTrabajo` ←
`RenderDk::Presentar`.

How it was read:

1. deko3d's `gpu_error.cpp` prints `(info[1] & 0xFFFF) / 4`, so `7c4` is a method index. Byte offset 0x1F10 in the
   Maxwell B 3D class (`clb197.h`, NVIDIA's open-gpu-doc) is `SET_VERTEX_STREAM_LIMIT_A_A(2)`: the high word of vertex
   stream 2's address limit. `0x200205f2` makes no sense as an address.
2. `0x200205f2` is a valid method header: increment, 2 words, subchannel 0, method 0x5F2, which is
   `IndexArrayStartIova` (`engine_3d.def`). That is what `dkCmdBufBindIdxBuffer` writes, and it is the call that
   follows `dkCmdBufBindVtxBuffers` in `RenderDk::Dibujar`. The GPU read the next command's header as data: the
   command stream was out of step.
3. deko3d 0.5.0's `dkCmdBufBindVtxBuffers` (`maxwell/gpu_3d_vbo.cpp`) reserves `5 * numBuffers` words and writes 6
   per buffer (a 3-word start address and a 3-word limit). A command buffer made with `dkCmdBufCreate` has no reserved
   slack words. So when the call lands near the end of a command memory piece, up to `numBuffers - 1` words go past
   the piece's end and still count in its GPU submission entry. The renderer gave deko3d its pieces back to back in
   the same block, so the next piece started exactly there, and its first command, the index buffer bind, overwrote
   those words before the GPU read them.
4. It needs 3 or more vertex streams and a draw that falls in the last few words of a 64 KB piece. Rare, which is why
   it took 36 minutes and a race.

Fix (2026-10-09):

- `RenderDk::Dibujar` binds one vertex buffer per call. With one buffer, the 5 reserved words still leave room for
  the sixth (`reserve` asks for more memory when `pos + n >= end`).
- `Nucleo` keeps the last 256 bytes of every command memory piece (`kHolguraComandos`) out of what it tells deko3d,
  so any other entry point that writes more than it reserves lands in memory nothing reuses.

Also in [HowToDeko3d.md](HowToDeko3d.md), section 6, item 17.

#### The 3D image is far too bright

Crash screenshot at 17:24:38 (`crash_1791559478.jpg`): white sky, blown-out bloom, washed-out colors. The title
screen and the videos look right, so the output pass and the gamma ramp are fine. Only the 3D scenes are wrong.

Cause: the game measures the scene's brightness for its automatic exposure by reading two 64x64 resolved textures
on the CPU (`08B46000` and `08B4A000` in this run). The Vulkan path copies every resolve of up to 4,096 texels back
into guest memory (`LeerResuelta`, `EscribirLecturas`). The deko3d backend did not yet, so the game read stale memory,
took the scene for very dark, and opened the exposure all the way. The project had already written down this exact
symptom for the emulated renderer (`app/src/nfsmw_app.h`, `readback_resolve`; `docs/native-renderer.md`).

Fix (2026-10-09), [HowToDeko3d.md](HowToDeko3d.md) section 5.7:

- `RenderDk::LeerResuelta` copies each resolved rectangle of up to `nfsmw_nativo_leer_resueltas_texels` texels (4,096
  by default, the same setting as the Vulkan build) with `dkCmdBufCopyImageToBuffer` into a CPU-cached, GPU-uncached
  block, 256 KB per work slot. A Full barrier follows: the copy engine runs beside the 3D engine, and the 3D engine
  releases the slot's fence.
- `RenderDk::EscribirLecturas` writes them into guest memory, tiled and in the guest's byte order, with the Vulkan
  path's code: once per frame for the slots whose fence has passed (oldest work first), and in any case when the slot
  is reused.
- The report counts the read-backs written per interval (`lecturas de vuelta al invitado`).

The black rear-view mirror in the same screenshot is a separate, known gap (P2b).

#### The clocks

`rex_perfil.log`: the game started with its GPU profile (`nfsmw_switch_gpu_mhz = 460`, configuration `0x92220008`:
CPU 1224, GPU 460.8 MHz). The console slept from 16:58:38 to 17:14:28 (959 s in one block) and kept those clocks
after waking. About two minutes later the user set the stock clocks (CPU 1020, GPU 307.2 MHz: block 72 for the CPU,
block 73 for the GPU) on purpose, to test at the target (see "The goal"), so the whole race ran at stock clocks.

A first reading took that for the system dropping the profile after the sleep, and a watchdog was added to
`sdk/src/ui/switch_apm.cpp` that asked for the profile again every 5 s. It was reverted the same day, before any
console run: it would fight the user's own clock choice (an overlay or a sys-clk fork). The file is as in git.

#### Performance of run 2

The race (17:17-17:24), debug library, **stock clocks** (CPU 1020, GPU 307.2 MHz, the target):

| Measure | Value |
|---|---|
| Frame rate | 21.4-27.3 fps, about 24 on average; frames mostly 36-50 ms apart |
| Draws per frame | 1,100-2,600 |
| Ring thread | busy 965-991 ms of every second: **it is the limit** |
| Ring thread time per draw | 10.7-16.4 µs (`[nativo] tiempos`) |
| The game waiting for the renderer | its frame preparer slept 5.8 s of every 10 s |
| Per frame | 21 resolve copies, 9 clears, 42 Full barriers (2 per copy) |
| Upload ring per frame | 10-18 MB (vertices, indices, constants), about 7 KB per draw |
| Command memory per frame | up to 640 KB, never past one slot's 2 MB |
| Textures | 2,081 created in 36 minutes and none freed: 213 MB; the image heap grew to 10 pieces, 291 MB |
| GPU time | not measured yet (`TiempoGpu` returns 0) |

For reference, the released NVK build averages 32-35 fps in races, but with the GPU profile (CPU 1224, GPU 460.8
MHz). At true stock clocks the only NVK figure on record is 26.0 fps, from before most of its CPU and GPU work
([performance-history.md](performance-history.md), step 3). The released NVK build has not been measured at stock
clocks: that run, on the same race, is the baseline to beat. This run is not comparable yet either: debug library,
and several race features missing (P2b).

Where the ring thread's time most likely goes, to check in this order:

1. **Every draw copies its vertices again.** 10-18 MB per frame of byte-swapped copies into uncached memory, with no
   reuse between draws or frames. The Vulkan path reuses vertex bindings whose data has not changed ("vertices sin
   repetir"). Port that first.
2. **The debug library** checks every call. The next run uses the release library.
3. **No stage timers per draw yet.** Only the whole `RenderDk::Dibujar` is timed (from the next build, in the report as
   `grabar un dibujo`). Add timers for lookup, textures, constants, state and vertices, sampled as
   [measuring.md](measuring.md) says.
4. **42 Full barriers per frame.** Each one makes the host wait for all engines and stops command prefetch. That is
   GPU time, not ring thread time, but it adds up. Batch the copies between two barriers, and port the
   resolve-without-copy swaps.
5. **Textures are never freed.** Not a speed problem yet, but memory grows for as long as the game streams new areas.

In the menus: the attract video runs at its 30 fps with 3-5 draws per frame (three texture uploads per video frame,
about 40 MB/s). The 3D menu scene after loading the career (about 330 draws per frame, 4.7 MB of upload per frame) ran
at a steady 30 fps at 460.8 MHz, and at 24-28 fps with the same draws once the clocks were set to stock: that scene
is limited by the GPU at stock clocks, not by the ring thread.

### Changes after run 2 (not yet run on the console)

- Crash fix: one vertex buffer per bind call; 256-byte gap at the end of every command memory piece.
- Exposure: read-back of resolves of up to 4,096 texels to guest memory.
- GPU time: a timestamp at the start and the end of each work's command list, read after its fence. The report gives
  `GPU X ms por fotograma`, and the system's `[nativo] C2: ... GPU X ms por Swap` line works again. It leaves out the
  uploads of the pre-list, which run before the first timestamp.
- Report: per-frame copies, clears, barriers and upload; per-draw recording time; per-interval texture creations,
  uploads and read-backs.
- Test settings on the console: `nfsmw_switch_gpu_mhz = 0` (stock clocks from the start); a capture every 20 s, up to
  120.

Builds: `nfsmw-dk.nro` (release library) and `nfsmw-dk-depurar.nro` (debug library), both on the console.

What the next run must show: a race with normal exposure (compare with an NVK capture of the same place), no GPU
method error over a long race, and at stock clocks the release library's frame rate, time per draw and GPU time per
frame. With the GPU time it is known which limit a race hits at 307.2 MHz: if the GPU ms per frame are close to the
frame time, the GPU; if they are well below it, the ring thread.

## 2026-10-09: run 3, deko3d against NVK on the same race at stock clocks

Logs: `out/consola/p2a3/` (`nfsmw_006.log` deko3d, release library; `nfsmw_007.log` the released NVK build, v1.0.0
PAL ES). Both at CPU 1020, GPU 307.2 MHz, the same career save and race. The user's impression: deko3d much smoother;
NVK with a nicer color, but stutters, slower loads and a lower frame rate.

| In the race | deko3d | NVK (release) |
|---|---|---|
| Frame rate | 21-26.5 fps, about 24 | 21-29 fps, about 26 |
| Frames of 100 ms or more in the race (from the `[nativo] C2: intervalos entre Swaps` histogram) | 5 in 300 s (1.0 per minute), the worst 834 ms | 9 in 120 s (4.5 per minute), the worst 2,533 ms; 7 of them over 150 ms. The ring thread created 151 new pipelines in the session, 49.0 ms each on average (cold pipeline cache: `C6 precalentado de pipelines`) |
| Usual frame time | 36-50 ms | 30-50 ms |
| GPU per frame | **35-43 ms** | **19-24 ms** |
| Ring thread busy | 975-992 ms/s | 942-987 ms/s |
| Ring thread per draw (`[nativo] tiempos`) | 8-19 µs | 14-21 µs |

So deko3d's ring thread is already a little cheaper per draw than NVK's, and it has no pipeline stalls, but at 307.2
MHz it needs almost twice NVK's GPU time per frame and is GPU-bound. NVK's GPU split per frame (its C2 report):
scene 7-10 ms, 320x180 (cube faces and blur) 4.8-5.6, reflection 2.0-2.5, copies 1.6-2.7, scene without depth
1.3-1.4, shadows ~1, clears 0.6-0.7, and 2.9-7.2 ms of GPU idle between submissions (it waits for the ring thread).

Why: the NVK path's GPU savings, measured over September, were not in the deko3d draw path yet. On by default in
NVK and missing in deko3d: no radial blur in the final composite (1.5-2.0 ms at 460.8 MHz), early depth testing for
draws that test but do not write depth (smoke, particles, glass), skipping clears that change nothing, resolve by
swapping images instead of copying (the second 1600x1600 shadow map alone: 2.4 ms), the shadow minimum, the deferred
sky, the lazy front-buffer and depth copies. deko3d also makes every resolve with the copy engine between two Full
barriers (21 copies and 43 barriers per frame).

The "nicer color" is NVK's default `nfsmw_resplandor_cielo = natural`: a modified bright pass for the glow, without
the saturated blue halo around trees and roofs that the game's own shader leaves. deko3d drew the game's shader.
Other visible differences in the captures: the rear-view mirror is black and the car's paint lacks its environment
reflections (the cubemap is built from six resolved 256x256 faces, `073D7000`-`07517000`, not assembled yet), and the
radial blur smears the screen edges.

The exposure read-back works: both builds read back `08B46000` and `08B4A000` (64x64) at the same rate, and the race
captures have the same brightness range.

The `grabar un dibujo` figure of the deko3d report included the waits for slot fences, which happen inside the first
draw of a frame when the GPU is behind: 400 µs "per draw" in a menu with 5 draws per frame. It now leaves them out.

### Changes after run 3 (not yet run on the console)

- **GPU time by category**, NVK's categories (`CategoriaDeDestino`): a timestamp at each change of pass type, copy or
  clear, up to 128 per work. The shared report prints the same `[nativo] C2: GPU por Swap: sombras ...` line as NVK,
  and the deko3d report adds `[deko3d] por fotograma: GPU sombras ...` with the GPU idle between works and the time
  the ring thread waited for the GPU.
- **No radial blur** (`nfsmw_nativo_sin_desenfoque`, default on as in NVK): bit 19 on the final composite
  (`p_000139`). The PC DKSH cache already has those variants.
- **Early depth** (`nfsmw_nativo_z_temprana`, default on): NVK's rule; the pixel shader's GLSL gets
  `layout(early_fragment_tests) in;` before its first declaration, and deko3d passes the DKSH flag to the GPU
  (method 0x084). Checked with uam on the PC: the DKSH changes in exactly that header byte. Compiled on the console in
  the background; the library's shader draws until it is ready.
- **Repeated clears skipped** (`nfsmw_nativo_saltar_borrados_repetidos`, default on): a clear to the value the target
  already holds with no draw since.
- **The glow variants** (`nfsmw_resplandor_cielo`: natural by default, suave). The formulas were read from the
  release NRO's SPIR-V (the two DXC modules in it); `tools/switch/dk_resplandor.py` applies them to the deko3d
  library's own HLSL of `p_000094`, compiles them with the library's DXC (v1.8.2502, which reproduces the library's
  SPIR-V byte for byte) and writes `app/src/nfsmw_dk_resplandor_spirv.h`. That header is derived from the game and
  gitignored, as NVK's; without it the setting keeps the game's shader.
- Pixel shader variants are part of the DKSH cache key (the field the vertex shaders use for their interpolators).

## 2026-10-09: run 4, where the GPU time goes

Logs: `out/consola/p2b1/nfsmw_008.log` (release library, stock clocks, a story race). With the first batch the race ran
at 22-27.5 fps (21-26.5 before) and the GPU still needed 35.5-41.8 ms per frame; the ring thread waited 13-21 ms per
frame for it, so the GPU is the limit. Early depth was applied to 19-210 draws per frame. GPU per frame by category,
against NVK's from run 3:

| Category | deko3d | NVK |
|---|---|---|
| Copies | **10.5 ms** | 1.6-2.7 |
| Scene (1280 with depth) | 9-15 | 7-10 |
| 320 (cube faces and blur) | 7.0-7.9 | 4.8-5.6 |
| Clears | 2.2 | 0.6-0.7 |
| Rest (the output pass and the start of the work) | 2.3-2.5 | 0 (NVK draws the output in the presenter) |
| Reflection | 1.7-2.9 | 2.0-2.5 |
| Scene without depth | 1.5 | 1.3-1.4 |
| Shadows | 0.3-0.7 | ~1 |
| GPU idle between works | 0.9 | 2.9-7.2 |

The copies: 21 per frame at about 0.78 ms per Mpixel on the copy engine (the figure NVK measured), about 9.9
Mpixels: the two 1600x1600 shadow maps (5.1), the scene's depth, three 1280x720 color targets, the cube faces and the
small blur and exposure targets; plus 43 Full barriers (each one drains every engine and stops command prefetch).

### Changes after run 4 (not yet run on the console)

**Copies and shadows.**

- **Resolve without copying** (`nfsmw_nativo_resolver_sin_copia`, `nfsmw_nativo_intercambiar_sin_borrado`, both on
  as in NVK): a whole depth target resolved to a texture of its size and format swaps images (and their
  descriptors) instead of copying. That is the two 1600x1600 shadow maps. The target remembers where its content went
  (`contenido_invalido`, `resuelta_base`) and gets it back by a copy if the game draws on it or resolves it again
  before clearing it.
- **The shadow minimum** (`nfsmw_nativo_sombra_minimo`), ported with NVK's guards: the world's resolve without a clear
  starts a cycle, the car pass is drawn over a cleared target instead of the world copied back, the second resolve
  gives the cars only, and every draw that samples that texture takes the minimum with the world's
  (`tfetch2DSombraMin`, spec bit 23, the pair at binding 16 + R). It watches 300 clean cycles (~10 s of racing),
  checks every car-pass draw and every read, and switches off for the session at the first difference (an ERROR line
  in the log). With both, no 1600x1600 copy is left.
- **Barriers only on a hazard:** a Full barrier before a transfer only if 3D work was recorded since the last one,
  and after it only before the next 3D work. The copy engine runs its transfers in order (non-pipelined, with flush),
  so a resolve and its read-back, or the six faces of a cube, need none between them. The work's fence waits for a
  pending transfer. ZCULL is invalidated after a depth target's image is swapped or restored by copy.

**Depth culling.** deko3d drops the ZCULL data whenever a depth target at another address is bound, and the game
clears its depth targets when it resolves them, long before it draws on them again: by then other depth targets
have been bound, so the scene started every frame with no ZCULL. Depth clears are now deferred
(`nfsmw_dk_borrado_z_aplazado`, on) to right after the target is next bound for drawing, so the ZCULL data starts the
pass valid; a pending clear is also done before the target is copied or swapped.

**Sky.** `nfsmw_nativo_cielo_aplazado` (on), NVK's criteria and guard (the sky's pixel shader, opaque, depth test
without writing, no stencil, 480 indices, first draw of the scene's pass; 90 frames with exactly one dome before
deferring). The deferred draw keeps the state it left in the draw state record and its buffers in the slot's ring; it
is replayed before the first draw that is not opaque with a depth write, a pass change, a copy or the end of the work.
NVK measured 3.4-4.3 ms of GPU saved.

**Correct image.** The dynamic cubemaps (the car's environment reflection and the rear-view mirror, "vista 20") are
six resolves to consecutive addresses, which the guest memory never holds. A cube texture whose six faces are
resolved RGBA8 textures is now a cube image the faces are copied into, once per frame at the first draw that samples
it (NVK does the same, a frame later). Not done: `nfsmw_tratamiento_visual` other than `original` (the console's
setting is `original`).

**Ring thread.** Vertex uploads deduplicated within a work slot (`DedupeVertices`, the Vulkan path's: exact within a
work, forgotten at every guest wait for the GPU). The `[deko3d] por fotograma` line now gives the ring thread's time
per draw by stage (preparing, textures, state, uploads and draw), the swaps, restores, cube refreshes, deferred skies,
the dedupe savings and the shadow minimum's phase.

What the next run must show: copies well under the 10.5 ms, the shadow minimum reaching APLICANDO with 0
differences, the sky guard passed, the car's reflections and the mirror drawn, and no new artefacts (shadows,
flickering sky, depth).

## 2026-10-09: run 5, the second batch on the console

Logs: `out/consola/p2b2/nfsmw_009.log` (release library, stock clocks, a story race). The user saw the car's
reflections, the mirror, the shadows and the sky right. The shadow minimum reached APLICANDO after its 300 clean
cycles with 0 differences, the sky guard passed (one deferred dome per frame), the cubemap is refreshed once per frame,
and 0.7-1.8 MB of vertices per frame were not copied again.

The race ran at **26-32 fps** (22-27.5 before; NVK did 21-29 in run 3). The GPU still sets the pace: 30.5-37.5 ms per
frame, and the ring thread waited 6-14 ms per frame for it.

| GPU per frame | Run 4 | Run 5 | NVK (run 3) |
|---|---|---|---|
| Copies | 10.5 | **6.0** | 1.6-2.7 |
| Scene | 9-15 | 8-14.6 | 7-10 |
| 320 (cube faces, blur) | 7.0-7.9 | 6.9-7.5 | 4.8-5.6 |
| Clears | 2.2 | 2.7 | 0.6-0.7 |
| Rest (output pass) | 2.3-2.5 | 2.4-2.6 | not counted (presenter) |

Ring thread per draw: preparing 1.0 µs, textures 1.8-2.5, state 1.5, uploads and draw 5.4-7.0: about 11 µs, so
about 19-27 ms of real ring thread work per frame. The CPU limit is close behind the GPU's.

### Changes after run 5 (not yet run on the console)

GPU:

- **The output ramp as a texture.** The gamma ramp was a 256-entry uniform array indexed by each pixel's color; a
  uniform read with a different index in every lane of a warp is served one lane at a time. It is now a 256x1 RGBA32F
  texture (`texelFetch`), uploaded in the pre-list when the ramp or the menu's post-processing changes.
- **Copies on the 3D engine** (`Copiar3D`): a resolve is a draw that reads the source with `texelFetch` and writes the
  destination as a render target (depth through `gl_FragDepth`). Fragments barriers instead of Full ones, only where
  there is a hazard (render target writes before, the copy's result after). The copy engine stays for the shrink blit
  and the resolves the game reads back (64x64), whose read-back follows on the same engine.
- **Depth compression** (`nfsmw_dk_compresion_profundidad`, on): depth targets and depth textures with
  `DkImageFlags_HwCompression`. Every depth copy and restore now goes through the 3D engine, never the copy engine
  (which broke compressed depth on nouveau, HowToDeko3d.md section 5.10).
- **Clears of the useful area only** (`nfsmw_dk_borrar_area_util`, on): each target learns the rows the game uses (the
  scissor bottom of its draws, the bottom of its resolves); clears stop there. When something reaches lower, the band
  is cleared first to the same value, so the result is exactly a whole clear.
- **Deferred color clears** (`nfsmw_dk_borrado_color_aplazado`, on), like the depth ones: done when the target is next
  bound, resolved or painted.
- **The front buffer from its target** (`nfsmw_nativo_frontal_perezoso`, on): the scene's resolve to the last Swap's
  front buffer is not copied; the Swap paints from the render target, whose clear is still pending. Anything that would
  change the target first, or a draw that samples the front buffer, makes the copy then; a draw that samples it after
  the Swap switches the mechanism off for the session (a WARN line).
- **Cube faces:** only the faces resolved since the last refresh are copied into the cube (per-image versions).

Ring thread:

- **Constants by differences** (`nfsmw_dk_constantes_por_diferencias`, on): one block per stage and work slot,
  written whole when the work starts, then kept in step by pushing only the 16-byte registers that changed
  (`dkCmdBufPushConstants`, inline and ordered with the draws). The comparison is exact (against what was pushed); the
  constant generations only decide whether to compare. The deferred sky keeps whole copies of its own.
- **The per-fetch texture cache across frames:** a game texture's answer holds until its next fingerprint check (up to
  39 frames); resolved textures and cubemaps are asked every frame (their per-frame hooks). Eviction waits 120 frames
  without use, so a texture in use is never released.
- The report splits the upload stage into vertices, indices, and constants with the draw, and counts the 3D copies,
  Fragments barriers, front buffers painted from their target, bands and pushed registers.

## 2026-10-09: run 6, the third batch in a race

Log: `out/consola/p2b3/nfsmw_010.log` (stock clocks; the third batch of changes after run 5, which had not been run
before this log). The NRO that wrote it is not recorded: the card's `nfsmw-dk.nro` was replaced by this round's build. From 19:37 to 19:45 (49 ten-second reports): 28-35 fps in the race (1,360-2,540
draws per frame), GPU 17-30 ms per frame, copies 3.5-20 per frame (the 3D engine's, plus the copy engine's), upload
0.2-9.9 MB per frame. No errors, no GPU method errors. Log 011 (19:45-19:48) is the released Mesa build
(`nfsmw-mesa.nro`, no deko3d lines). The per-category breakdown of run 6 is not in this entry yet: it needs the
[deko3d] por fotograma lines of 010 read the same way as run 5's.

## 2026-10-10: no music in races, and the fourth batch (not yet run on the console)

### No music in races

The user reported that races had no music. It is not a deko3d problem: the released NVK build has it too, because
the cause is in the runtime's XMP emulation (XMP is the Xbox 360's music player service, the one that lets a player
replace a game's soundtrack with their own).

How it was found:

1. **The music is read and decoded.** The race music is `SOUND\PFDATA\MW_MUSIC.MUS`. The disc's `ZDIR.BIN` is a
   table of 24-byte entries (the game's string hash of the upper-case path, archive number, sector, ..., size), and
   that file is all of `ZZDATA7.BIN`: EA `SCHl` streams, stereo 44.1 kHz, EA-XA ADPCM (the game decodes it on the CPU,
   in `sub_82618788` and `sub_82617B68`; it is not XMA). Runs 3 (NVK) and 5 (deko3d) both read it during the race, in
   128 KB pieces at the speed it plays. So the data arrived, and the sound was lost after the decoder.
2. **Nothing in the mix drops it.** The native audio functions report 0 differences against the recompiled code, and
   the output stage folds all six channels except the LFE.
3. **The game mutes it on purpose.** `sub_825E3230`, called by the audio code, polls the game's XNotify listener
   (created with mask 0x20, the XMP area) for `XN_XMP_PLAYBACKCONTROLLERCHANGED` (0x0A000003) and copies its parameter
   into a flag at `0x828F6018`. At start-up that flag is filled from `XMPTitleHasPlaybackControl` (`sub_826D6868`:
   TRUE when `XMPGetPlaybackController`, `sub_826D65F8`, answers controller 0 and not locked). When the flag becomes
   0, the game fades its music out, and while it stays 0 it clears the music buffer with `memset` on every pass: the
   console's behaviour when the player's own music is playing.
4. **The runtime said the opposite of what happened.** `XMPOverrideBackgroundMusic` (`sub_826D68E8`, the title
   taking control of the music) sends `XMPSetPlaybackController(2, 0, 1)`; `XMPRestoreBackgroundMusic`
   (`sub_826D69C0`) sends `(2, 4, 0)`. The SDK's handler (`sdk/src/kernel/xam/apps/xmp_app.cpp`, from Xenia) broadcast
   the notification with `!playback_client`, so the title taking control told the game "the user has control" (0),
   and the game silenced its own music until the next restore. Each wrapper has 47 call sites. The order of the calls on
   the way into a race was not traced; silence for the whole race means the last one before it is an override.

**Change:** the notification's parameter is now whether the title may play its music, the same thing
`XMPTitleHasPlaybackControl` answers: TRUE unless the system player is playing, which never happens here. Each call
writes an info line, `[xmp] XMPSetPlaybackController(...) -> el titulo controla la musica: 1`.

**Correction (run 7):** that line never appeared. The game does not call `XMPSetPlaybackController` on the way into
a race, so the inverted parameter, real as it was, is not why races have no music. The start-up query
(`XMPTitleHasPlaybackControl` through `XMsgStartIORequest` with no overlapped) succeeds in the SDK, so the flag at
`0x828F6018` should be 1. Run 7 still had no music; the cause is open (below).

### Changes after run 5's batch (the fourth batch)

Ring thread:

- **Vertices and indices kept across frames** (`nfsmw_dk_geometria.h/.cpp`, `nfsmw_dk_geometria_mb`, 64 MB, 0 = off).
  A persistent CPU-uncached block keeps the host form of guest ranges. A range is identified by its host pointer,
  bytes and mode (byte order; for indices also the format, restart index and offset), and its content by the XXH3
  of its guest bytes, hashed once per epoch (a work, a frame, or the time between guest waits for the GPU: the same
  scope `DedupeVertices` already trusts). A range enters the store the second time it is seen unchanged, so one-off
  data never fills it; a range that changes twice in a row goes to the ring unhashed for 64 frames. Stored indices
  keep their vmin/vmax, so neither the scan nor the conversion runs again. A block whose content changed is never
  rewritten: the new content gets a new block and the old one is freed only when the last work that bound it is
  done (stamped at bind time, after any change of work slot; never the work still being recorded, whose slot fence
  is the previous one's). When the store is full, the ranges not used for longest go, never one used in this frame
  or the last. The report adds `[deko3d] geometria entre fotogramas`: MB not copied, MB hashed, MB admitted,
  changes, ranges sent to the ring unhashed, MB used, evictions, admissions without room, collisions.
- **Indices in one pass:** the indices that are not in the store are no longer converted into a scratch vector and
  copied: the range is read first (read-only, NEON for 16 bits) and they are converted straight into the ring at the
  upload.
- **Sampled rechecks of stable textures** (`nfsmw_nativo_huellas_muestreo` = 8) and the **per-frame budget of
  texture checks** (`nfsmw_nativo_huellas_kb_fotograma` = 6144), the Vulkan path's C3 with its guard: a stable texture
  is rechecked with the hash of a sample (first and last 4 KB block and 1 in 8) instead of all its bytes; 1 in 8
  rechecks stays whole, and the first 3000 stable rechecks compute both hashes, so one disagreement switches
  sampling off (an ERROR line). Before, a stable texture got a whole XXH3 every 32-39 frames: about 6 MB per frame
  for the 218 MB of live textures. The report adds `[deko3d] comprobacion de texturas`.

GPU:

- **A per-draw GPU profile** (`nfsmw_dk_perfil_gpu_s` = 20, 0 = off): one frame every 20 s gets a timestamp before
  and after each draw (that frame is slower, the GPU finishes each draw before the next). The report then lists the
  frame's time per category, the 12 most expensive passes (category, size, target keys, draws) and the 15 most
  expensive pixel shaders and 6 vertex shaders, by library fingerprint, with uam's register count, code size and
  scratch for each. That is what decides the next GPU work: which shaders to compare with NAK and where to act.
- uam already runs nv50_ir at its highest level (3); nv50_ir has no latency-aware instruction scheduling, which is
  what this port's NAK patch adds (`mesa/README.md`). Register pressure in the console's DKSH cache (702 variants):
  most pixel shaders use 4-30 registers, but 28 use 39-64 (at 64 an SM runs half the warps it could). Whether those
  are the expensive ones is the profile's first question.

What the next run must show: music in races; the same image as run 5 (no wrong or flickering geometry: the store is
the risky change, `nfsmw_dk_geometria_mb = 0` switches it off); MB not copied per frame close to the 10 MB of upload;
the ring thread's per-draw times for vertices and indices down; the texture check MB per frame down from ~6; and the
profile lines.

## 2026-10-10: run 7, the fourth batch in a race

Logs: `out/consola/p2c1/` (`nfsmw_012.log` and `nfsmw_013.log`, `rex_perfil.log`, `rex_crash.log`, the Atmosphère
report `01791593022`). Release library, stock clocks. Still no music in the race.

**A crash at start-up (012), once.** 5 s after boot, while the front end loads its sounds (`MW_Music.mpf` was just
opened), a guest access violation: a read of guest `0x000C0E10` in `sub_82630728` (EA's sound code, called from
`sub_8264F168` ← `sub_826477A8` ← `sub_826484C0` ← `sub_8243CAA8`), symbolized with `nfsmw_release.elf`. The next
boot (013) ran normally. Nothing of this round writes guest memory; it is the first time this one is seen, and it
is noted to watch for.

**Performance (013, 2:44-2:46, a race with 2,000-2,650 draws per frame, more than run 5's 1,500):**

| Per frame | Run 5 | Run 7 |
|---|---|---|
| fps | 26-32 | 25-32 |
| GPU busy | 30.5-37.5 ms | 26-35 ms |
| GPU idle, waiting for the ring thread | 0.9 ms | 3-8 ms |
| Ring thread waiting for the GPU | 5-14 ms | 0-4 ms |
| Copies | 6.0 | 3.7-3.8 |
| Clears | 2.7 | 0.7 |
| Rest (output pass) | 2.4-2.6 | 0.9-1.1 |
| Scene | 8-14.6 | 9.7-14 |
| 320 | 6.9-7.5 | 7.5-8.2 |
| Upload | ~10 MB | 0.03-0.14 MB |
| Ring thread per draw | 10.1-11.0 us | 9.0-11.0 us |

The third batch's GPU work paid off (copies, clears and the output pass: -5.9 ms per frame), and the geometry store
took the upload to almost nothing (9.1 MB per frame served from it, 0.02 MB admitted). But the ring thread's time
per draw barely moved: the store hashes every byte it serves once per frame (7.2 MB), which costs about what the
copy did (vertices 2.2-3.2 us per draw), and the index lookups moved 0.4 us into "preparing". With more draws per
frame, the ring thread used 85 % of a core (71 % in run 5) and the process's 3 cores were 80-90 % busy (the game's
own threads 64 %, 55 % and 38 %, the XMA decoder 19 %): the frame now waits for the CPU, not the GPU.

**The GPU profile** (frame 5004, each draw timed alone, which adds about 4 us per draw):

- A single full-screen draw of a 320x720 pass right after the scene takes **3.6 ms**: pixel shader n77
  (`ABE81C36A1A89DEC`), a 4x4 box downsample (16 bilinear fetches, two outputs, 18 registers). On paper that is well
  under 1 ms; the next profile logs its viewport, scissor and source texture to find out why.
- The scene pass (1,610 draws) is led by n108 (`C81FF93456236DF5`): one texture fetch and an alpha test, 366 draws
  writing depth (no early depth possible) and 366 that only test it (early depth already on). Fill rate.
- The sky dome (n29, `C9B5898658247E8D`): 7 draws (the scene and the 6 cube faces), 2.8 ms.
- uam's register counts for these are low (9-25); register pressure is not their problem.

### Changes after run 7 (the fifth batch)

- **Sampled geometry checks** (`nfsmw_dk_geometria_muestreo`, on): a stored range of more than 8 lines of 64 bytes is
  rechecked by hashing one eighth of its lines (rotating, so every line is read at least once every 8 frames) plus
  the first and the last, against per-phase hashes kept when it was hashed whole. The first 20,000 sampled checks
  also hash everything, and one disagreement switches sampling off with an ERROR line (the texture checks' guard,
  which in run 7 saw 6,379 checks and no disagreement). The report adds the sampled checks per frame.
- **The profile names its 8 most expensive single draws** with their pass, viewport, scissor, color masks,
  RB_DEPTHCONTROL and first texture (size, format, sampler).
- **Music diagnostics:** `nfsmw_audio_diag_musica` (on) hooks `sub_825E3230` and logs every 10 s its calls, the flag
  at `0x828F6018`, how many calls found the music muted, and the peak of the buffer it guards (`[musica]` lines). The
  test settings run the three native audio functions in validation mode (the game's own result is used, differences
  are counted): if the music comes back, one of them is the cause.

## 2026-10-10: run 8, sampled geometry checks and the music diagnostics

Logs: `out/consola/p2c2/nfsmw_014.log`, `rex_perfil.log`. A race of about 3 minutes, stock clocks, the three native
audio functions in validation mode. Still no music.

**Music: two more suspects ruled out.**

- The native resampler, filter and gain sum ran in validation mode (the game used the recompiled code's result) and
  reported 0 differences over millions of samples per 10 s: they are not the cause, and the music was still silent
  with the game's own code.
- `sub_825E3230` never ran (no `[musica]` line, and nothing calls it directly), so it is not on the music's path.

The music stream is still read at its playback speed. The race track (519.5 MB into `MW_MUSIC.MUS`, 4 channels,
101 s) decodes on the PC to real audio (a Python EA-XA decoder: RMS 400-1,800, adjacent-sample correlation 0.999),
so the data on the disc is good.

**Performance:** 23-33 fps at 1,950-2,900 draws per frame; the geometry checks now read 1.0-1.9 MB per frame
instead of 7.2 (no sample disagreed with a whole hash), but the vertex stage only went from 2.2-3.2 to 2.35-2.6 us per
draw: hashing was not most of it. Memory latency and the lookups are; this needs a CPU profile, not more guesses. The
GPU still waits 1-12 ms per frame for the ring thread.

**The 3.6 ms draw, explained in part:** the profile's details show the bloom downsample (n77) draws only 320x180
pixels (viewport and scissor), 16 taps each from the 1280x720 scene texture (format 8_8_8_8), with two color outputs:
0.9 million texture fetches in 3.6 ms, about 1 GB/s, some 20 times too slow. The source is the scene render target
itself (resolve without a copy), which has hardware color compression; NVK (no compression) does the whole 320
category in 4.8-5.6 ms.

### Changes after run 8

- **Music, measured instead of guessed:** a hook on the EA-XA stream decoder (`sub_82618788`, "give me N samples")
  logs every 10 s the busiest decoders with their samples per second and peak (`[musica]`); the music is the decoder
  set at 44,100 samples per second. `tools/llamadas_directas.py` was run again (undo and redo) so the generated code
  calls the hooked name. The test settings dump the game's 6-channel mix (`audio_volcado_desde_s = 55`,
  `audio_volcado_s = 90`), and a script (decoder plus normalized cross-correlation, checked on a synthetic mix with
  the music 30 dB under noise) finds whether, where and how loud each music channel is in each mix channel.
- **GPU test:** `nfsmw_dk_compresion_color = false` in the test settings, to see whether compression is what makes
  the downsample slow.
- **CPU:** a draw with the same pixel shader and the same fetch words for its samplers as the previous draw reuses
  its textures (not across works, texture generations, expiries, or reads of the cars' shadow texture). The report
  gives the share of draws that reuse.
- **CPU profile:** `logs/rex/perfil_pilas.flag` on the SD card turns on the stack sampler (1 ms): per thread, the
  functions that take its time, for the ring thread and for the game's own threads (`funciones.py` sums them per
  function with `addr2line`).

## 2026-10-10: run 9, Heritage quick race: music plays, a CPU profile

Logs: `out/consola/p2c3/` (`nfsmw_015.log`, `rex_perfil.log` with stack sampling, `audio_volcado_0.wav`,
`funciones.txt`). A quick race on the Heritage map (known as CPU-heavy on the NVK build), stock clocks, color
compression off as a test.

**Music played.** The quick race read 2-channel segments of about 4 s around 335-352 MB of `MW_MUSIC.MUS`; every
career race so far read long 4-channel streams (101-114 s, two stereo layers) at 495-553 MB. So the audio path works,
and the problem is specific to the career's 4-channel music. The EA-XA decoder hooked in this build
(`sub_82618788`) never ran in the quick race: it is not the 2-channel segments' decoder, and probably the 4-channel
one, which the next career race will show.

**Color compression is not the downsample's problem:** off, the bloom downsample still took 3.62 ms (and copies went
from 3.7 to 4.0 ms), so compression goes back on. In every profile that draw is the first draw of the frame, in the
menus too.

**Performance:** 20-35 fps at 1,250-3,900 draws per frame; GPU 25-41 ms per frame (the scene pass 10-21 ms), so in
Heritage the GPU was the limit most of the time. Across 20 race profiles, the most expensive pixel shaders per frame:
the downsample n77 (3.6 ms, one draw), the alpha-tested n108 in its two passes (3.3 ms writing depth, 1.4 ms only
testing it), the sky n29 (2.2 ms, 5 domes at ~380 us: 5 fetches, ordinary), then a long tail.

**The CPU profile** (`funciones.txt`, sampled every 1 ms; percentages of one core): the ring thread used 67 %, of it
`RenderDk::Dibujar` 7 %, `EmpujarDiferencias` 6.6 %, `AlmacenGeometria::Consultar` 5.3 %, `XXH3_64bits` 4.7 % (many
small calls), the PM4 marker parsing about 4 %. The game's threads spread over hundreds of recompiled functions; the
largest single items are the already-native scenery, view render and visibility functions.

### Changes after run 9

- **The repeat test in the GPU profile:** the first draw of each pass, when drawing it twice changes nothing (no
  blending, no depth or stencil writes), is drawn and timed twice; the extra copy is left out of the totals. It tells
  whether the downsample's 3.6 ms is its own or a cost the frame's first draw pays.
- **CPU:** `EmpujarDiferencias` skips 4 equal registers at a time with NEON; the geometry store has an 8,192-slot
  direct-mapped cache in front of its map (emptied by the sweep, the only erase); the per-fetch texture cache's slot
  comes from an inline mix of the 6 fetch words instead of an XXH3 call.
- Test settings: compression back on; the mix dump kept for a career race; stack sampling off.

## 2026-10-10: run 10, a race with the repeat test

Logs: `out/consola/p2c4/` (`nfsmw_016.log`, `audio_volcado_0.wav`; `rex_crash.log` is still run 7's, no new crash).
The game sat on the title screen until the attract movie, and the race started at 13:09:01, 2 min 45 s after launch.

**Music.** The race read only 2-channel music, 335-352 MiB of `MW_MUSIC.MUS` at ~44 KB/s (one stereo EA-XA track),
the same as run 9's quick race. Every session with missing music (runs 7 and 8) read 4-channel streams at 495-553 MiB,
starting ~20 s after the world loaded. So this run never reached the 4-channel path:

- The mix dump (launch + 55..145 s) ended at 13:08:44, 17 s before the race: menus and loading only, with sound in the
  front and surround channels.
- The hook on `sub_82618788` never ran (the deferred-report path works, and the symbol is the hook's in the ELF).
  Searching the recompiled code for the EA-XA raw-frame test (`== 0xEE`) gives a family of decoders:
  `sub_826186B8`, `sub_82618788`, `sub_82618AE8`, `sub_82618D60`, `sub_8260FFA0` (plus `sub_82668A08`, `sub_82668BB8`,
  `sub_82669BB8`, `sub_82669C98`). `sub_82618D60` (which calls `sub_82618AE8`, called from `sub_82619190`) is in run
  9's CPU profile of the sound thread, so it is the 2-channel music's decoder. `sub_82618788` and `sub_826186B8` are
  called from `sub_826132xx-826137xx`: a second path, still the candidate for the 4-channel streams.
- The stereo fold keeps the surround channels (ITU weights, -3 dB) on the Switch output, so 4-channel music placed in
  the rear channels would still be heard: not the cause.

**Performance (race, stock clocks):** 27-37 fps at 1,400-3,300 draws per frame. GPU 26-36 ms per frame: GPU-bound
again. Per frame: scene 10-19.5 ms, 320 passes 6.4-7.8, copies 3.8, reflection 1.4-2.7, scene without depth 1.5,
clears 0.7, rest 1.1. The ring thread records a draw in 8.3-10 us and waits 1-15 ms per frame for the GPU.

**The downsample is slow on its own.** The repeat test drew n77 twice in each profile: 3.61-3.65 ms the first time,
3.72-3.75 ms the second. Its GLSL is 16 bilinear taps of one 1280x720 RGBA8 texture (offsets from constants 1-16)
written to two 320x180 targets: 0.92 M texel reads. The full-screen n19 reads 5 textures over 921,600 pixels (4.6 M
reads) in 0.85 ms, so n77 is about 20x slower per read. The sampler is built the same way as on NVK, and NVK's 320
category was 4.8-5.6 ms against deko3d's 7.0-7.9 (run 3): the gap is about n77's excess. The pattern (time scales with
the reads, not with the pixels) suggests reads that miss the texture cache: taps spread far apart, or a texture whose
layout does not suit the access. Not yet known which.

Other costly pixel shaders (one profile at 13:12:41): the alpha-tested n108 7.8 ms writing depth plus 3.6 ms only
testing it (211 draws each), the sky n29 2.7 ms (7 domes), n90 2.5 ms (220 draws).

**The texture-check sampling switched itself off** at 13:08:18, as in run 9, on the same texture: `0CB7D000`,
1024x1024 DXT1 (512 KB), changed by the game in a way the sample did not see. The guard works (it turned sampling
off), but one texture costs the saving for all of them: 2.1 MB hashed per frame in the race.

**Menus:** the GPU takes 21-28 ms per frame for 250-320 draws, the same fixed costs (n77, copies) as in the race.

### Changes after run 10

**Music parked.** The user reports that the career's prologue (up to the BMW being taken) has no music until free roam
is unlocked, and that quick races have music: the 4-channel streams of runs 7 and 8 were probably the prologue's
score, and the silence probably by design. Not confirmed; the decoder hook stays (off in the test settings).

**A lower internal resolution that really lowers the work** (`nfsmw_resolucion_interna = escena_1120x630`,
`escena_1024x576`, `escena_960x540`). The existing `1024x576` choice only shrinks the final resolve: the scene stays
at 1280x720 (measured on NVK: 2.14 ms saved). The new choices use the 1080p path at a smaller size
(`nfsmw_render_targets.cpp`, `Modo4()`): mode 4 of the game's AA table gets W x H (rounded up to 32 rows), and the
front buffer and the logical screen size globals get W x H too, as the 1080p path raises them. Scene, post-processing,
HUD and front buffer stay consistent with each other (mixing a scene size with another front buffer size was what
cropped the image in the 1080p work), and the presenter scales the frame to the screen. The scene's pass category
(which the sky deferral needs) follows the configured width (`g_ancho_escena`). The test settings use
`escena_960x540` (56 % of the pixels) to measure the most it gives.

**An honest per-draw profile.** In the profiled frame, the first draw of each pass is timed after a full barrier
(`nfsmw_dk_perfil_barrera`): if a timestamp is taken when the GPU reaches it rather than when earlier work ends, the
first draw of the frame (the downsample) absorbs whatever ran before it. Full-screen textured draws that can be
repeated are also drawn, before their real second copy, with sampler 0 on every texture, with the empty 1x1 textures,
and writing only target 0 (`nfsmw_dk_perfil_variantes`); the report lists those times next to the draw, and the
draw's pixel constants 0-16 are logged once (the downsample's tap offsets).

**Texture-check sampling per address.** A sample that misses a change now excludes that guest address only (it is
rechecked whole) and restarts the guard; the fifth such address switches sampling off for all, as before.

## 2026-10-10: run 11, the 960x540 scene: faster, effects shifted left

Logs: `out/consola/p2c5/` (`nfsmw_018.log`: the 960x540 session, 14:07-14:11; `nfsmw_017.log`: the run-10 build,
13:32-13:50, 18 min at 1280x720; `capturas/captura_006_0120s.png`). The user reported: better performance, and the
shadows, bloom and reflections shifted to the left.

**The shift.** The log has it: at 960x540 the bloom chain's resolved textures were created at 256x134 and 128x67, the
width of the copy destination's pitch (rounded to 32 texels), while the image inside is 240 and 120 wide. Sampled edge
to edge, the image lands to the left, and more at each step of the chain. At 1280x720 every resolved width is already
a multiple of 32, so it never showed. The fix (`Copiar` and the depth resolve): a resolve from the left edge that is
narrower than its pitch by less than 32 texels takes the texture's width from the resolve. Built and uploaded at
14:14; **not yet checked on the console** (the user's next race will show it).

**Performance at 960x540 (run 018, 4 min, 2,000-2,900 draws per frame):** 33-55 fps, GPU 20-29 ms per frame,
against run 10's 27-37 fps and 26-36 ms at 1280x720. The 320 passes went from 6.4-7.5 to 4.3-5.5 ms; the scene passes
are 6-16 ms. The bloom downscale (`n77`, a 240x134 target) costs 1.74 ms of its "small passes" bucket, down from 3.6 ms.

**Why the downscale is slow: a negative result.** The profile's variants (`nfsmw_dk_perfil_variantes`, sampler 0,
empty 1x1 textures, one output) say: the same 16 fetches on empty 1x1 textures take 0.10 ms, on the real texture
1.74 ms, whatever the sampler (sampler 0: 1.74 ms; the filter is not the cost). So the cost is in the reads of the
resolved texture: its layout or its cache behaviour, not the arithmetic.

I tried one hypothesis and it is wrong: that uam keeps the sums between the fetches and each sum waits for its fetch.
Two checks:
- The bloom shader's GLSL with its 16 fetches moved before the sums (SPIR-V reordered, each sample a statement of its
  own): its DKSH is byte-identical. Of the 702 variants, 12 changed, all of one other shader (fingerprint
  `C760F5D866FBDC5A`), not the bloom.
- The same two shaders (16 fetches interleaved with the sums, and grouped) compiled alone with uam: identical bytecode
  (`uam -r`). uam already issues the fetches ahead of their uses.
The reorder was reverted; the build on the console does not contain it.

Still open for the downscale: the texture's tile size (the resolved textures keep the default 16-GOB tall tiles, 128
rows: vertical taps 6 rows apart and horizontal ones across 8 KB columns) and whether the resolved texture is read
through the same texture cache path as the scene's textures. Next experiment: resolved textures with a custom tile
size (`DkImageFlags_CustomTileSize`, 1 GOB) and a variant whose taps all read one 8x8 block.

**Other changes in this build.** The texture-check sampling excludes a guest address that its sample misses (rechecked
whole) and switches off only after the fifth such address, instead of at the first (run 10's one texture: 0CB7D000).
The profile times each pass's first draw after a full barrier (`nfsmw_dk_perfil_barrera`).

**Music.** The user's report: the career's prologue (up to the BMW being taken) has no music until free roam is
unlocked, and quick races have music. So the silent sessions were likely the prologue's score, not a bug. Not
confirmed: the decoder diagnostic stays off in the test settings.

## 2026-10-10: run 12, the width fix at 960x540

Logs: `out/consola/p2c6/` (`nfsmw_019.log`, 14:42-14:53, `rex_crash.log` unchanged from run 7: no new crash). The build is
run 11's plus the resolved-texture width fix; `nfsmw_resolucion_interna = escena_960x540`, stock clocks (CPU 1020, GPU
307.2, memory 1331.2), the music diagnostics off. The user's report: "went great", the effects are where they belong.

**The fix works.** The resolved textures of the bloom chain are now `240x134` and `120x67` (run 11: 256x134 and
128x67); the others are the same as before (`960x540`, `480x270`, `256x256`, `64x64`, the `1600x1600` shadow maps).

**Race numbers** (59 ten-second windows with more than 800 draws per frame; fps from the shared `[recortes] carrera`
line, the one the NVK build prints too):

| | Run 12 (960x540) | Run 10 (1280x720, 85 windows) |
|---|---|---|
| fps | 35.2 average, 28.0-47.7, p10 28.9, median 35.0 | 30.9 average, 20.4-40.5, p10 27.2, median 30.0 |
| Draws per frame | 2,187 | 1,952 |
| GPU busy per frame | 24.6 ms | 29.5 ms |
| GPU idle between works (waiting for the ring thread) | 5.3 ms | 9.1 ms |
| Ring thread waiting for the GPU | 1.6 ms | 4.5 ms |
| Recording a draw (`grabar un dibujo`) | 8.0 µs | 8.8 µs |

These are two different sessions (run 10's is log 017, 13:32-13:50), not the same race driven twice, so the difference
between the columns is not the pure effect of the resolution. The closest pair is run 10's log 016 (1280x720: 30.0 fps on
average over 32 windows) and run 11's log 018 (960x540: 38.2 over 18 windows): two sessions again, with builds that
differ in the resolution option and the profile variants. Both the GPU (24.6 ms) and the ring thread (5.3 ms of GPU idle) now set the pace in turn, so the next
gain has to come from both.

**The two builds' categories at 960x540 are not comparable one to one with 1280x720:** the reflection pass (480 wide
now) falls in the "320" bucket and the bloom's 240-wide passes in "menores"; the total is what compares.

### Changes made while closing the work

- The `escena_*` choices of `nfsmw_resolucion_interna` exist only in the deko3d build (`#if NFSMW_DEKO3D` in
  `nfsmw_ajustes_graficos.cpp` and `nfsmw_render_targets.cpp`). The Vulkan path creates its resolved textures at the
  pitch too (`ObtenerResuelta(base, pitch_destino, ...)` in `nfsmw_nativo_destinos.cpp`), so it would show the same
  shift, and at 1080p (1920 / 8 = 240) it may already. Not touched: it cannot be tested here. Both configurations
  compile.
- Release and debug NROs of the final code are on the console (`nfsmw-dk.nro` 39,856,325 bytes,
  `nfsmw-dk-depurar.nro` 39,884,997).

## Open work

State after run 12 (35.2 fps at 960x540, 28.8-30.9 at 1280x720, stock clocks). Ordered by what it should give:

1. **A like-for-like comparison with the Mesa build on the heavy maps.** The same `nfsmw.toml`, the same race, both
   NROs (`nfsmw-dk.nro` and `nfsmw-mesa.nro` are on the card), at stock clocks; Heritage is reported to be CPU-bound on
   the Mesa build and has only been measured on deko3d (28.8 fps average over 33 windows, GPU-bound). The only
   same-race pairs on record are runs 3 and 6 (see the pull request description).
2. **The bloom downscale (`n77`)**: 1.74 ms at 960x540 for 240x134 pixels, 0.10 ms when its 16 reads hit one texel.
   Not the filter, not the arithmetic, not the fetch order (tested). Next: resolved textures with a custom tile size
   (`DkImageFlags_CustomTileSize`, 1 GOB), and a variant whose taps all read one 8x8 block.
3. **The scene pass (6-16 ms at 960x540):** the alpha-tested foliage `n108` (7.8 ms writing depth plus 3.6 ms only
   testing it, 211 draws each at 1280x720), the sky `n29` (2.7 ms, 7 domes), `n90` (2.5 ms, 220 draws). Compare uam's
   code for them with NAK's.
4. **The ring thread:** at 8.5 µs per draw and 2,200 draws per frame it is 19 ms of a 28 ms frame; the GPU idles 5.3 ms
   per frame waiting for it. The geometry store's lookups, the constant differences and the PM4 marker parsing are the
   largest items of its CPU profile (run 9).
5. **The resolution options for the Vulkan build:** `escena_*` needs the resolved-texture width fix in
   `nfsmw_nativo_destinos.cpp` first (see run 12), and at 1080p (240 = 1920 / 8) that build may already show the shift.
6. **Correctness:** `nfsmw_tratamiento_visual` (`suave`, `apagado`), 1D textures, the remaining formats.
7. **Music:** parked; the silent sessions were probably the career's prologue.

Earlier list, kept for reference:


1. **The remaining copies:** the lazy front-buffer copy (NVK's `nfsmw_nativo_frontal_perezoso`: the Swap paints from
   the retained target, 0.9 Mpixels a frame), and the color resolves with the 3D engine (Fragments barriers instead of
   Full ones).
2. **The 320 passes and the clears** (7-8 and 2.2 ms against NVK's 5 and 0.7): measure again after the deferred
   depth clears; then the useful area (`nfsmw_nativo_restaurar_area_util`, `nfsmw_nativo_borrar_area_util`: the
   targets are taller than what the game draws).
3. **The output pass** (2.3-2.5 ms in "rest"): check what it costs alone and whether FXAA is on.
4. **Depth compression** on deko3d, once ZCULL is measured.
5. **Ring thread:** constants through `dkCmdBufPushConstants`, then whatever the new per-stage times show.
6. **Correctness:** `nfsmw_tratamiento_visual` (`suave`, `apagado`), 1D textures, the remaining formats.

The older list, kept for reference:


- **P2b, races at parity:** cubemaps from resolved faces, the shadow minimum, the deferred sky, early depth variants,
  resolves without copies, occlusion queries, the rear-view mirror.
- **P4, ring thread time:** vertex reuse between draws, stage timers, constants through `dkCmdBufPushConstants`,
  texture eviction.
- **P4, GPU time** (it decides the frame rate at 307.2 MHz once the ring thread is fast enough): fewer Full barriers,
  resolves without copies, ZCULL and color compression checked on deko3d, then timestamps per pass to find the
  heavy ones.
- **Baseline:** the released NVK build on the same race at stock clocks. Set up on 2026-10-09: the v1.0.0 release's
  `nfsmw-nx-pal-es.nro` (GitHub release asset, the build the docs' 32-35 fps figures describe) is on the console as
  `sdmc:/switch/nfsmw-nx/nfsmw-mesa.nro` (not `nfsmw-nx.nro`: hbmenu shows a folder holding an NRO with its own name as that one app, and hides the other NROs), with the Vulkan shader library made from the same disc (`nfsmw_shaders.nfsp`,
  SHA-256 `a27aea23…`, identical to the installer's). Both NROs read the same `nfsmw.toml` and `saves/`, so the same
  race runs on both at the same clocks. NVK compiles its pipelines on first use, so its first run of a race
  stutters: measure a second run.
- **Test settings to remove before release:** `nfsmw_captura_cada_s` and `nfsmw_captura_max` in the console's
  `nfsmw.toml` (the clean file is `out/consola/nfsmw_instalador.toml`).

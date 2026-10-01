# Horizon and NVK: platform notes

This page collects facts about the Switch that took a long time to find out: its hardware, its operating system
([Horizon](glossary.md#horizon)), the homebrew library [libnx](glossary.md#libnx) and the Vulkan driver
[NVK](glossary.md#nvk). Read it before you port another game, and whenever the console does something you did not
expect. Nothing here is specific to this game: it applies to any Xbox 360 game that is
[statically recompiled](glossary.md#static-recompilation), run with the [ReXGlue](glossary.md#rexglue) runtime and
drawn through [Mesa](glossary.md#mesa) on the Switch.

## In short

- Plan for the normal clocks, without overclocking. A game gets three CPU cores at 1020 MHz and a GPU at 768 MHz
  docked. In handheld mode a program starts with the GPU at only 307.2 MHz; it can ask the system for 460.8 MHz with
  an official performance mode, and this port does.
- Do not read free memory from `svcGetInfo(InfoType_UsedMemorySize)`: in a homebrew program it counts the whole heap
  from the start (this port reads 3185 of its 3189 MB before loading anything). In a race the GPU uses about 500 MB
  of a 1.4-1.5 GB budget, so there is room left.
- Horizon limits how much memory a process may map, and libnx has one exception stack for the whole process. Both
  needed changes in the runtime (see [Guest memory](#guest-memory) and [Exceptions](#exceptions)).
- Only thread priority 0x3B takes turns on a core. At any other priority, a busy thread does not let threads of the
  same or lower priority run on its core.
- In NVK, every fence check and every queue submission is a call into the system, and GPU memory allocations are slow
  and limited in number.
- Presentation has fixed limits: one Vulkan queue, three swapchain images, and only the FIFO and IMMEDIATE modes.
- SaltyNX never loads itself into a homebrew program, so the console overlays show no FPS and ReverseNX-RT does
  nothing until the program writes their shared blocks itself. See [console-overlays.md](console-overlays.md).

## Hardware at stock clocks

"Stock clocks" are the speeds the console runs at without overclocking. They are what a game really gets, so always
measure and optimise against them:

| | CPU | GPU | Memory |
|---|---|---|---|
| Docked | 1020 MHz | 768 MHz | 1600 MHz, 25.6 GB/s |
| Handheld | 1020 MHz | 307.2 MHz by default; 460.8 MHz in the mode this port asks for | 1331.2 MHz, 21.3 GB/s |

- **The handheld GPU speed depends on the program.** In handheld mode the GPU has three speeds (307.2, 384 and
  460.8 MHz), and each title gets its own. A [homebrew](glossary.md#homebrew) application starts at 307.2 MHz. It can
  ask the system for a faster official mode (see [Clocks](#clocks)): this port asks for configuration `0x92220008`,
  the GPU at 460.8 MHz with memory at 1331.2 MHz (setting `nfsmw_switch_gpu_mhz`). That is not an overclock. Profiles
  of commercial games that report 460.8 MHz only show what those games ask for, not what your program gets by
  default.
- **Three of the four CPU cores** are available to the application. Its core mask (a number with one bit for each
  core it may use) is 0x7.
- **The GPU is small.** It is a GM20B, from NVIDIA's Maxwell generation: 256 cores, only **256 KB of L2 cache**, and
  more than 400 ns to get data from memory. The CPU and the GPU share the same memory and its bandwidth, so when the
  CPU copies vertices, it competes with the GPU.
- **It draws straight into memory.** Like every NVIDIA GPU, it is an *immediate-mode* GPU, not a *tiler* (a GPU that
  draws the screen in small tiles kept inside the chip). So the load and store settings of a render pass (whether the
  old image is read at the start and saved at the end) barely matter. What costs time is the work done for each pixel
  and for each vertex. Passes that cover the whole screen are expensive, because nothing that big fits in 256 KB of L2.
- **Half precision is not used.** 16-bit floating point math (FP16) runs twice as fast as 32-bit math (FP32), but the
  [shaders](glossary.md#shader) translated from Xbox 360 [microcode](glossary.md#microcode) do not use it.
- **A margin on Mariko is not a margin on Erista.** The Switch exists with two versions of its chip, Erista (the
  first one) and Mariko (the later one). Both run at the same stock clocks, but Erista heats up sooner in long
  sessions. So if a frame is only a little under its time budget on a Mariko console, that margin may not hold on an
  Erista one.
- **Free memory has to be measured inside the heap.** An application gets about 3.2 GB. When hbloader (the Homebrew
  Menu, or a forwarder) starts a homebrew program, it turns all of that memory except about 2 MB into the program's
  heap, and libnx takes that heap as it is (`__nx_heap_size` is ignored). So `svcGetInfo(InfoType_UsedMemorySize)`
  reads almost the total from the first moment: 3185 of 3189 MB here, before the game loads anything. It does not
  show what is free. Everything the port uses comes out of that heap: the guest memory (about 506 MB), the thread
  stacks and Mesa, GPU memory included. The GPU part can be read from Vulkan's memory budget
  (`VK_EXT_memory_budget`): in a race, about 480-515 MB used of 1.4-1.5 GB budgeted. So there is room for more
  buffers: each work slot of the renderer (the buffers of one more frame in flight, see
  [native-renderer.md](native-renderer.md)) costs 64 MB, and the renderer uses three.

## Guest memory

The [guest](glossary.md#guest) is the recompiled Xbox 360 game. Its memory is the Xbox 360's memory, which the
runtime recreates inside the Switch process. The Switch version of that code is in
`sdk/src/core/guest_memory_switch.cpp`.

- **Horizon limits how much memory a process may map**, apart from how much RAM is free. (To map memory is to make it
  visible at an address of the process.) The limit is `LimitableResource_Memory`, and you can read it with
  `svcGetInfo(InfoType_ResourceLimit)`. When it runs out, even committing 4 KB (asking for real memory behind an
  address) fails with `0xCE01`. That is error 2001-0103, `KernelError_ResourceExhausted`: it is not the
  out-of-memory code, so it is easy to misread.
- **The Xbox 360 sees the same memory at several addresses.** There are up to five views: 0x7F000000, 0xA0000000,
  0xC0000000, 0xE0000000 and the raw physical window. At first, every committed chunk was mapped into every view.
  That turned 472 MB of real memory into 1782 MB of mapped memory, and the game went black after the first logo.
  Changing the heap size (`__nx_heap_size`) did not move the point where it failed: under hbloader that value is not
  used at all (see [Hardware at stock clocks](#hardware-at-stock-clocks)).
- **What works:** commit physical memory only when it is needed, in 4 MB chunks, and map a chunk into a view only
  the first time that view touches it. The exception handler does this: touching such memory raises an exception,
  the handler commits or maps the chunk, and the access is retried.
- **The permissions of guest pages cannot be changed.** `svcSetProcessMemoryPermission` is refused on memory mapped
  with `svcMapProcessMemory`. So to watch guest pages for writes (to notice when the game changes them), the runtime
  has to unmap them instead.

## Exceptions

When a thread touches memory that is not mapped, or that it may not touch, the CPU raises an *exception* (here, a
*page fault*) and a handler runs. The runtime relies on this all the time: it is how guest memory gets committed and
mapped (see above). The code is in `sdk/src/core/exception_handler_switch.cpp`.

- **libnx has one exception stack and one exception dump for the whole process.** (The dump is where the registers
  of the faulting thread are saved.) This game has about 27 threads and 30 to 2,500 page faults per second, so two
  threads often enter the handler at the same time. The visible symptom was a crash in `mutexLock` with a mutex
  pointer of 1. The invisible one was worse: **a thread resumed with the registers saved by another thread**. That
  corrupts the game's state without leaving a trace. A test that only checked that the process stayed alive "proved"
  there was no problem.
- **The fix is a pool of stacks and dumps**, eight in this port. On entry, a thread claims a free slot with the
  atomic instructions `ldaxr`/`stlxr` and keeps the slot number in its dump. The slot is released when the thread
  resumes.
- **Review every global thing the exception path touches** in the same way. In the first profile of the game on the
  console, the fault path itself took most of the time of a busy thread: the exception entry, the handler, and a
  global guest-memory mutex taken three times per fault.

## Threads

- **`std::thread::detach()` can throw** a `std::system_error` on Horizon. It was seen with libnx 4.12, when the
  thread had already finished. The exception ends in `std::terminate`, which closes the game. Use persistent worker
  threads that take jobs from a queue instead of detached one-shot threads, or catch the exception.
- **Only priority 0x3B takes turns.** Threads at that priority are *time-sliced*: each one runs for about 10 ms and
  then lets the next one run. Every other priority is *cooperative*: a busy thread keeps its core, and threads of the
  same or lower priority wait, **even while other cores are idle**. For example, a presentation thread at the same
  priority as the render thread spent 998 ms of real time per second to get 300 ms of CPU, and the share of frames
  slower than 50 ms went from 8 % to 48 %.
- **When a thread starves, lower the priority of the thread that hogs the core.** Do not raise the starving one:
  that pushes it into priorities that other parts of the system depend on.
- The priorities this port uses (a lower number means a higher priority):

  | Priority | Threads |
  |---|---|
  | 0x2A | profiler |
  | 0x2B | audio worker, [XMA](glossary.md#xma) decoder and audio output |
  | 0x2C | presentation and other [host](glossary.md#host) threads |
  | 0x2D | [ring thread](glossary.md#ring-thread) of the native renderer (it reads the list of commands the game sends to the GPU) |
  | 0x3B | guest threads and bulk work (the only time-sliced priority) |

- **A preferred core does not pin a thread.** Horizon accepts the setting but keeps moving the thread between cores
  (0.24 moves per loop of the ring thread, whatever core was chosen). An exclusive core mask does pin it. But a render
  thread locked to one core fell short at the busiest moments: its minimum dropped to 16.3 FPS, against about 21
  without pinning. The moves themselves are cheap (about 0.05 % of CPU).
- **Decide to sleep, or to wake another thread, only while holding the lock.** A vertex copy thread hung twice
  because each side wrote its own atomic flag and then read the other side's flag without a lock. One wake-up in
  millions was lost. Sequentially consistent atomics (the strictest kind) are not enough on ARM either. Every new wait
  in this port has a timeout and logs how long it has been waiting, so a lost wake-up shows up as a log line and not
  as a silent hang.
- **Recompiled games spin.** Two loops in the game's [Direct3D](glossary.md#direct3d) layer checked a value again
  and again without sleeping (*busy-waits*). One waited for the GPU to read the command ring. The other handed each
  frame from one game thread to another by polling a flag with `Sleep(0)`. They kept two hardware threads close to
  100 % while doing nothing. The fix was to [hook](glossary.md#hook) the polling functions: the hook sleeps for a
  limited time, or until another thread wakes it up, and then calls the original function so the game's own checks
  still run. On the PC that brought the two threads down to 25 % and 17 %. The hooks are in
  `app/src/nfsmw_espera_anillo.cpp` and `app/src/nfsmw_espera_fotograma.cpp`.
- **When a hook reads guest memory, apply the same address offset as the recompiled code.** On the PC build,
  addresses at or above 0xE0000000 are shifted by 0x1000.

## Clocks

These notes are for programs that change the console's clock speeds.

- `apmSetPerformanceConfiguration` (the call that picks a set of clock speeds) returns before the clocks change. If
  you read the clock on the next line, you still see the old value.
- `pcv` (the system service that controls power and clocks) puts back the memory clock of the active performance
  configuration. So if you change the memory clock directly through `clkrst` (the clock and reset service), the
  change is accepted and then silently undone.
- The configuration IDs are opaque numbers: read the real table, do not guess. For example, 0x92220007 is GPU
  460.8 MHz with memory at 1600 MHz, and 0x92220008 is GPU 460.8 MHz with memory at 1331.2 MHz. 0x92220006 does not
  exist.

## NVK on Horizon

This port uses danfromtico's Switch port of Mesa 26.2.1, with the changes described in [mesa.md](mesa.md). This is
how that driver behaves on the console:

- **GPU timestamps run at another rate than the driver says.** `timestampPeriod` says one unit is 1 ns, but the
  counter really advances one unit every 1.627 ns. See [measuring.md](measuring.md).
- **There are two memory types.** Type 0 is `HOST_CACHED`: the CPU reaches it through its cache (an NvMap with CPU
  cache). After writing to it, call `vkFlushMappedMemoryRanges`, which cleans the data cache so the GPU sees the
  writes. Type 1 is `HOST_COHERENT`: an NvMap without CPU cache.
- **Checking a fence always costs a system call.** (A fence is the signal the GPU gives when it finishes a piece of
  work.) Every `vkGetFenceStatus` and `vkWaitForFences` makes an ioctl, a request to the system's GPU driver, even
  when the GPU finished long ago.
- **Every `vkQueueSubmit` is a call to another process** (an IPC call that starts the work on the GPU channel). On top
  of that, the WSI (the part of the driver that puts images on screen) adds one empty submission each time a frame is
  presented. Splitting the scene into two submissions left the GPU idle for 3.6 ms per frame. Keep the number of
  submissions per frame low.
- **Creating GPU memory is slow.** Each allocation is a block from the process heap, aligned to 64 KB and wrapped
  with `nvMapCreate`. Creating one texture with its own allocation (a *dedicated* allocation) cost about 1.9 ms of
  CPU, spent in these calls:

  | Call | Median | Calls per texture |
  |---|---|---|
  | `nvMapCreate` | 422 µs | 1 |
  | `nvAddressSpaceAllocFixed` | 127 µs | 2 |
  | `MapBufferEx` | 626 µs | 2 |

  A dedicated allocation pays twice for address space and for mapping. Its only benefit, compression, never applies
  to images with `SAMPLED | TRANSFER_DST` usage (read by shaders and filled by copies), like this game's textures. So
  the renderer takes its textures from large shared blocks instead (*sub-allocation*). With 32 MB blocks the cost
  fell to 0.75 ms per texture, and a burst of 15 new textures in one frame went from 28.5 to 11.3 ms. The code is in
  `app/src/nfsmw_nativo_texturas_pool.cpp`. Two facts help: `requiresDedicatedAllocation` is never true on this
  driver, and image sizes are already rounded to 64 KiB for sub-allocators.
- **The number of GPU allocations is limited, not their size.** libnx gives nvdrv (the system's GPU driver service)
  8 MB of transfer memory (`__nx_nv_transfermem_size`) to keep track of allocations. That is enough for roughly
  4,000 NvMap handles. When it runs out, creating one fails with `0x235C` (`LibnxNvidiaError_SharedMemoryTooSmall`).
  It looks like an out-of-memory crash, but it is not: the game died with 3,585 cached textures and about 600 MB in
  use. The symbol is weak (libnx only gives it a default value), so the application can raise it before Mesa calls
  `nvInitialize()`. In this port the setting `nfsmw_switch_nvmap_mb` in `nfsmw.toml` does that; 0, the default,
  keeps the 8 MB of libnx.
- **The driver's buffer object cache** (where it keeps freed GPU memory blocks for reuse) is limited by its 256
  entries, not by megabytes. Its size setting, `NOUVEAU_HORIZON_BO_CACHE_MB`, is 128 MB by default.
- **Driver options are environment variables.** The driver reads them with `getenv` when the Vulkan instance and the
  devices are created, so they must be set before creating the instance. In this port you write them in the setting
  `nfsmw_mesa_entorno` of `nfsmw.toml` (see [mesa/README.md](../mesa/README.md)). Useful ones:
  `MESA_SHADER_CACHE_DISABLE` (turns off the shader cache on the SD card, below), `NVK_SWITCH_PERF_LOG` (counters and
  timers of the driver's Horizon code, for diagnostics), `NVK_SWITCH_CPU_WRITE_MEM_UNCACHED` (whether memory the CPU
  only writes to skips the CPU cache; on by default) and `NOUVEAU_HORIZON_BO_CACHE_MB`.
  `MESA_VK_ENABLE_SUBMIT_THREAD` has no effect.
- **Mesa's shader cache on the SD card is a duplicate.** Mesa saves compiled shaders to `sdmc:/.mesa`, from `disk$`
  threads with 8 MB stacks. If the application keeps its own [pipeline cache](glossary.md#pipeline-cache)
  (`VkPipelineCache`), the disk cache only duplicates it: turn it off with `MESA_SHADER_CACHE_DISABLE`. This port
  turns it off by default (setting `nfsmw_mesa_cache_disco`). Both caches are thrown away when the compiler revision
  in the driver's compiler flags changes (the number in `nvk_shader.c`, see [mesa/README.md](../mesa/README.md)).
- **Creating a pipeline is slow.** (A [pipeline](glossary.md#pipeline) is the shaders and GPU state of a draw,
  compiled together.) Without a cache it took about 59 ms per pipeline on the console: 112 pipelines in 6.6 s. Save a
  `VkPipelineCache` and, on top of it, a list of the pipelines the game uses, so they can be created on a background
  thread before they are needed. See [native-renderer.md](native-renderer.md).
- **[ZCULL](glossary.md#zcull) is compiled out** of the Switch build of NVK in danfromtico's port. ZCULL (hierarchical
  Z) lets the GPU skip pixels hidden behind others before shading them. See [mesa.md](mesa.md) for what it takes to
  enable it and what it gave.

## Presentation

*Presentation* is the step that hands each finished frame to the system so it appears on screen. The Horizon WSI and
NVK on Maxwell put four limits on it that cannot be worked around:

1. **One Vulkan queue for the whole device.** Separate transfer queues (for copies) need a newer NVIDIA GPU
   (Turing). So a presentation thread shares the queue with rendering and cannot run in parallel with it.
2. **Swapchains have exactly three images.** (The swapchain is the set of images that take turns on screen.) If you
   ask for four, you silently get three.
3. **Only one image can be acquired at a time.** A second acquire returns `VK_NOT_READY` or `VK_TIMEOUT`.
4. **Only the FIFO and IMMEDIATE modes exist.** FIFO waits for the screen refresh to show each frame; IMMEDIATE does
   not wait. MAILBOX and FIFO_RELAXED silently fall back to FIFO. IMMEDIATE is `nwindowSetSwapInterval(nw, 0)` and
   **does not tear**: the system compositor (nvnflinger) keeps composing at 60 Hz. IMMEDIATE only means that the
   program sending the frames is no longer blocked.

The compositor shows frames in steps of 16.67 ms (one refresh at 60 Hz). What that means for smoothness is explained
in [measuring.md](measuring.md#frame-pacing-the-mean-is-not-what-the-player-feels).

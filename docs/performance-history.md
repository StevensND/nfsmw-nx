# Performance history

This page tells how a race went from 1-3 FPS to 30 FPS and more at the Switch's stock clocks (its normal speeds,
without overclocking), in the order it happened. Read it to see which changes paid off, and how much, before you plan
the work on another game. The reasoning behind each step is in the other documents; this page is the timeline.

All the numbers are from the console, in handheld mode, during the same race used for every test. The stock clocks a
game gets: the CPU at 1020 MHz, with three cores for the game; the GPU at 307.2 MHz in handheld mode (460.8 MHz with
the performance configuration described in step 5) and 768 MHz docked; the memory at 1331.2 MHz in handheld mode.

## In short

| Step | Result in a race |
|---|---|
| 1. The emulated GPU | 1-3 FPS |
| 2. The first native renderer, measured at stock clocks | 16.1 FPS (median) |
| 3. GPU work cut from 60 to 33 ms | 26.0 FPS |
| 4. Frame pacing fixed | every rendered frame reaches the screen |
| 5. CPU cost per draw cut, and the GPU at 460.8 MHz | 30.3 FPS |
| 6. Direct calls, LTO, PGO and function ordering | about 34 FPS |
| 7. Native versions of the hottest game functions | 35.4 FPS |
| 8. The released build | 32 to 35 FPS, depending on the area |
| 9. A deko3d renderer instead of NVK (branch `deko3d-implementation`, in progress) | 29-32 FPS at stock clocks, no pipeline stalls; 35 FPS with the scene drawn at 960x540 |

## 1. The emulated GPU: 1-3 FPS

The first builds used the GPU backend that comes with the SDK. It imitates the Xbox 360's GPU
([Xenos](glossary.md#xenos)): it translates the game's GPU commands while the game runs, emulates the
[EDRAM](glossary.md#edram) (the Xbox 360 GPU's 10 MB of embedded memory), and converts shaders the first time they
are used. The game logic, the menus and the audio worked, but a race ran at 1-3 FPS.

Emulating every draw (every drawing command sent to the GPU) cost far more CPU than three 1 GHz cores can give. So the
decision was to replace the GPU emulation with a native renderer, instead of optimising it (see
[native-renderer.md](native-renderer.md)).

## 2. The native renderer

The native renderer reads the list of commands that the game's [Direct3D](glossary.md#direct3d) layer writes for the
GPU (the [PM4 ring](glossary.md#pm4-ring)), and records Vulkan commands from it on its own thread. It was developed
on the PC first. In two days it drew videos, menus, the HUD and full races with the rear-view mirror, reflections and
shadows, at 1.5-1.8 µs of CPU per draw.

The first console numbers looked like 22-28 FPS, but they had been taken with the console overclocked. The first
measurement at stock clocks, a few days later, is the real starting point: **16.1 FPS median**, with the GPU busy for
the whole frame:

| GPU time per frame | ms |
|---|---|
| Scene | 27.2 |
| Shadow maps | 13.6 |
| Copies | 7.4 |
| Cubemap and blur | 7.0 |
| Post-processing | 3.9 |
| **Total** | **60.2** |

The CPU was not the limit: it used about 200 % of the 300 % available (three cores). The GPU was. It spent its time
shading fragments (the pixels of each triangle), not moving memory.

## 3. GPU work from 60 to 33 ms

The goal was 30 FPS at stock clocks, keeping the look of the Xbox 360 version. The GPU work came down in steps:

- **One pass instead of the Xbox 360 tiling.** The Xbox 360 draws the scene in pieces (tiles) so that each one fits in
  its 10 MB of EDRAM, with MSAA (multisample antialiasing, which smooths edges). The Switch needs neither, so the scene
  is drawn once.
- **Shader constants through a dynamic uniform buffer.** Shader constants are the values the game gives its
  [shaders](glossary.md#shader); a dynamic uniform buffer is a Vulkan buffer that holds the constants of each draw.
  Also, the **inverse texture size** is computed once instead of with 160 size queries (see [shaders.md](shaders.md)).
- **Resolves without copies.** A [resolve](glossary.md#resolve) is the Xbox 360's copy of a finished image out of
  EDRAM. Swapping images instead of copying [render targets](glossary.md#render-target) saved 2.36 ms.
- **[ZCULL](glossary.md#zcull)** enabled in NVK, so the GPU skips hidden pixels before shading them: about 2 ms of the
  scene (see [mesa.md](mesa.md)).
- **Shadow maps** (images of the scene seen from the light, used to draw the shadows):
  - a cheaper 3x3 PCF, the filter that softens shadow edges (when the nine samples of the filter land on one texel,
    they collapse to one);
  - no vegetation in the shadow maps;
  - a shadow pass that does not load the previous contents.
- **Simpler translated shaders.** Their predicated blocks (pieces of code that run under a condition) were merged,
  which removed most of their 1806 branches, and redundant `max(a, a)` was removed.

By the end of this stage the GPU work fit in 33.35 ms, and a race averaged **26.0 FPS** over nine minutes. It did
not reach 30, because the GPU sat idle for several milliseconds per frame, waiting for work.

## 4. Frame pacing

Frame pacing is how evenly the frames reach the screen. Two findings changed it, independently of the
[frame time](glossary.md#frame-time):

- **Half of the frames were thrown away.** At start-up, an invisible achievements dialog registered itself as a UI
  drawer (something drawn on top of the game). With one drawer registered, the presenter painted from the UI thread
  and merged frames, so every second rendered frame never reached the screen. With that fixed, every rendered frame
  is presented.
- **A presentation thread of its own** gained 1.5 FPS on average. But it made the range of frame times three times
  wider, and the frames over 50 ms 2.4 times more frequent. It is off: a steady frame time feels better than a higher
  average (see [measuring.md](measuring.md#frame-pacing-the-mean-is-not-what-the-player-feels)).

What is shown follows the screen's refresh: a 34 ms frame is shown for 50 ms. That is why frames a little over
33.3 ms matter so much, and why the distribution of frame times is the number to watch.

## 5. The CPU becomes the limit

With the GPU work under 33 ms, two CPU threads limited the frame: the [ring thread](glossary.md#ring-thread), which
records the Vulkan commands, and the game's main thread. The next weeks went to the CPU cost of each draw in the ring
thread (the full list is in [native-renderer.md](native-renderer.md)):

- caches for the shaders that the game loads straight from the command list ("immediate loads");
- uploads without duplicates: data that is already on the GPU is not copied again;
- far fewer query pool resets (1.2 ms);
- a fix for a swapchain format check that recreated the composition 6,000 times;
- no logging from the ring thread;
- the game's own busy-waits in its Direct3D layer replaced by real waits (see
  [platform-notes.md](platform-notes.md#threads)).

At the same time, the renderer asks the system, through `apm` (the system service for performance modes), for the
stock performance configuration `0x92220008`: GPU at 460.8 MHz with memory at 1331.2 MHz in handheld mode. See
[platform-notes.md](platform-notes.md#clocks) for the pitfalls of setting it. With it, and with the sky dome drawn
later in the frame (about 3 ms), the scene went from 22.3 to about 12 ms, and a race reached **30.3 FPS**. The limit
was now the CPU: the three cores were at 90, 99 and 93 %.

## 6. The build

The recompiled code is about 144 MB of C++. Compiling it for the target (the Switch) instead of for the PC paid off
in three steps (see [toolchain.md](toolchain.md)):

- **Direct calls** between recompiled functions, which let the compiler see and inline across them.
- **[LTO](glossary.md#lto)** (optimising the whole program at once, when it is linked) over the recompiled code and
  the app.
- **[PGO](glossary.md#pgo)** (optimising with a record of which code runs most) with a profile recorded on the
  console, and **[function ordering](glossary.md#function-ordering)** to keep the code that runs most together.

With these, a race averaged **about 34 FPS**.

## 7. Native game functions

The game's main thread spends much of its time in its own renderer front end (the part of the game that decides
what to draw: materials, effect parameters, visibility, matrices). The recompiled code runs it far slower than native
code would. So the hottest of those functions were rewritten in C++ as
[native replacements](glossary.md#native-replacement). Each one is behind a [guard](glossary.md#guard): it runs both
versions and compares them before trusting the native one, and keeps checking afterwards (see
[native-renderer.md](native-renderer.md)). The functions:

- material parameters (about 2-4 % of the game thread) and effect parameters (about 5 %);
- object visibility (82,000 calls per second);
- per-draw matrices (595 instructions down to 394, 265 memory accesses down to 78) and the view render loop;
- the Direct3D state of each draw, now passed to the ring as one marker instead of about 14 packets.

Result: **35.4 FPS** on average in a race, the heaviest alley from 27.6 to 30.0 FPS, and frames of 60 ms or more cut
from about 24 to 7 in the test race (-70 %).

## 8. Where it stands

The work after that brought the GPU time per frame down to about 23 ms, and cut the cost of each draw inside NVK (see
[mesa.md](mesa.md)). From there on, the ring thread sets the pace, and every millisecond of GPU saved is worth about
half a frame per second.

The released build averages 32 to 35 FPS in races, depending on the area. The heaviest place measured is the exit of
Heritage Heights, with about 3,000 draws per frame: it runs at 21-22 FPS for its first 20 seconds or so.

## 9. The deko3d renderer (in progress)

The NVK driver is a Vulkan implementation written for desktop GPUs, running on the Switch's GPU. Its costs on the
Switch are the pipeline objects (a draw with a state the game has not used yet makes the ring thread create one, 49 ms
on average in the cold-cache session below), the work NVK does inside each draw, and the size of the Mesa build. The
alternative is deko3d, the console's own graphics library: no pipeline objects, no driver to carry. The shaders are
compiled ahead of time into the console's native format. How it was built, run by run, is in
[deko3d-devlog.md](deko3d-devlog.md); the plan is in [HowToDeko3d.md](HowToDeko3d.md).

All numbers below are from the console at stock clocks (CPU 1020 MHz, GPU 307.2 MHz, memory 1331.2 MHz), in handheld
mode. The frame rate is counted from the `[recortes] carrera` line that both builds print every 10 seconds in a race;
slow frames from the `intervalos entre Swaps` histogram. The Mesa sessions are the released v1.0.0 build.

**Same save and same race, one session after the other:**

| | deko3d | Mesa (NVK) |
|---|---|---|
| First pair (deko3d at its first race-ready build): FPS in the race | 27.7 average, 21.9 lowest, 23.3 the 10th percentile (31 windows of 10 s) | 26.2 average, 15.2 lowest, 20.8 the 10th percentile (14 windows) |
| Frames of 100 ms or more | 1.0 per minute, the worst 834 ms | 4.5 per minute, the worst 2,533 ms |
| Frames of 50 ms or more | 28.8 per minute | 44.0 per minute |
| Pipelines created by the ring thread | none: nothing to create | 151, 49.0 ms each on average (cold pipeline cache) |
| Second pair (deko3d with the third batch, Mesa with a warm pipeline cache): FPS in the race | 32.3 average, 24.9 lowest, 28.7 the 10th percentile (43 windows) | 29.2 average, 27.2 lowest (only 9 windows) |
| Frames of 100 ms or more | 1.1 per minute | 1.0 per minute (60 s of racing); 8 new pipelines still cost 50.3 ms each |

**What is not better yet.** The GPU time per frame at 1280x720 (22.6-29.5 ms in the later deko3d sessions) is not below
NVK's 19-24 ms, and the cost of recording a draw (8-14 µs) is the same as NVK's best figures (9-21 µs). The heaviest
windows (2,900-3,700 draws per frame, the first half minute of the Heritage race) run at 20.8-23.4 FPS on deko3d, as
the exit of Heritage Heights does on NVK (21-22 FPS, section 8). The frame-rate gain at the game's own resolution
comes from the stalls and the lower floor, not from a faster average.

**What is new.** The internal-resolution choices `escena_1120x630`, `escena_1024x576` and `escena_960x540` really
draw the scene, the post-processing, the HUD and the front buffer at that size (the old `1024x576` only shrank the
image after drawing it at 1280x720). At `escena_960x540`: **35.2 FPS on average** in an 11 minute race (59 windows,
28.0-47.7, 0.6 frames of 100 ms or more per minute), GPU 24.6 ms per frame, against 28.8-30.9 FPS at 1280x720 in the
deko3d sessions before it. Those are different sessions, not the same race twice. Only the deko3d build offers these
choices for now.

**Size.** The deko3d NRO is 39.9 MB, the Mesa one 63.0 MB (-37 %).

## Lessons

- **The overclocked numbers of the first days were the most expensive mistake.** They hid for days that the GPU was
  the limit. Measure at stock clocks from the start.
- **A configuration file that overrides the code's defaults** (such as `nfsmw.toml`) can keep a change switched off
  for several builds without anyone noticing. Check in the log what is really running.
- **One millisecond of GPU is not one millisecond of frame.** When the CPU and the GPU work at the same time, only
  half of a GPU saving shows up.
- **The average hides what the player feels.** Look at the frames over 33.3 and 50 ms.

# Console overlays: FPS, resolution and ReverseNX-RT

This page explains how the port shows its frame rate and resolution in the console's overlays (Status Monitor
Overlay and its forks, such as Horizon OC Monitor), and how it follows the handheld or docked mode chosen in
ReverseNX-RT. None of these tools supports a [homebrew](glossary.md#homebrew) program out of the box, so the port does
their part itself. Everything here works the same for any other port or homebrew program.

The code is in `sdk/src/ui/switch_saltynx.cpp` (FPS, resolution and the ReverseNX-RT mode) and
`sdk/src/ui/switch_sysclk.cpp` (clocks that follow ReverseNX-RT).

## In short

- **The overlay does not measure the FPS.** A small program inside SaltyNX, called NX-FPS, writes them into a shared
  memory page, and the overlay only reads that page. NX-FPS gets them by hooking the game's present call. This port
  has no such call to hook, so **the port writes that block itself**.
- **SaltyNX never loads itself into a homebrew program**, so nothing is ever done for us. We ask SaltyNX for its
  shared page once at startup, and from then on we write into it like ordinary memory.
- **The overlay checks every 100 ms that the game is still alive.** The block has to be updated on every frame, not
  once per second. That costs a few bytes written to memory: no system calls.
- **ReverseNX-RT works the same way.** Its block is normally created by its plugin inside the game. We create it
  ourselves, and the game obeys the mode chosen in its overlay: resolution, window size and, if you allow it, clocks.
- **What you need on the console:** SaltyNX 0.7.0 or later. Without it, the port does nothing here and loses nothing.

## Making SaltyNX work with a homebrew program (.nro)

By default, SaltyNX only works with commercial games: the overlay shows "Game is not running or it's incompatible"
for any homebrew program, and ReverseNX-RT says "ReverseNX-RT is not running!". This section is the short version of
how to change that for your own .nro. The rest of the page explains each step in detail.

**Nothing changes in SaltyNX, and nothing has to be installed besides SaltyNX 0.7.0 or later.** SaltyNX cannot load
itself into a homebrew program (see [How the overlay gets its numbers](#how-the-overlay-gets-its-numbers)), so the
.nro does the work SaltyNX would do: it asks SaltyNX for its shared page and writes the data there. For the overlay,
the result is exactly the same as with a commercial game: it shows the FPS and the resolution, and ReverseNX-RT shows
its controls.

### Using this port's code in your .nro

The code only needs [libnx](glossary.md#libnx). Copy two files:

- `sdk/src/ui/switch_saltynx.cpp`
- `sdk/include/rex/ui/switch_saltynx.h`

The header includes `rex/platform.h` only to know that it is built for the Switch. Outside this project, replace that
line with `#define REX_PLATFORM_SWITCH 1`. Then call four functions from your program:

```cpp
#include "rex/ui/switch_saltynx.h"
namespace salty = rex::ui::switch_saltynx;

// 1. At startup. If SaltyNX is not installed it does nothing; if it is not ready yet, the next calls retry.
salty::Iniciar();

// 2. Every time you present a frame (after vkQueuePresentKHR, eglSwapBuffers, nwindowQueueBuffer...).
//    It writes about 25 bytes to memory that is already mapped: no system calls.
salty::Latir(width, height);

// 3. Once per second, from any thread: frames in the last second, their average, the resolution
//    and the number of frames since startup. If the block was lost, it finds it again.
salty::Actualizar(fps_last_second, fps_average, width, height, frames_since_startup);

// 4. Wherever your program asks whether the console is docked: obey ReverseNX-RT.
bool docked = salty::ModoBase(appletGetOperationMode() == AppletOperationMode_Console);
```

Two things to adjust:

- The block says which graphics API the game uses. The code writes `3` (Vulkan) in `SembrarBloque` and `Latir`. Use
  `1` for NVN or `2` for OpenGL.
- Warnings go to `stderr`, with lines starting with `[saltynx]`. Send `stderr` to a file on the SD card (libnx's
  `freopen` or your own log) so that you can see which port connected, or why it failed.

### How to tell that it works

Open Status Monitor Overlay (or Horizon OC Monitor) with your .nro running: the **FPS** and **RES** rows should show
values. In this port they appear about one second after startup. If they do not:

| What you see | What it means |
|---|---|
| No `[saltynx]` line at all | `Iniciar()` is not being called, or `stderr` goes nowhere. |
| `0xF201` | SaltyNX is not installed, or it has not started yet. |
| `0x10801` and `sesiones usadas / tope: 1 / 1` | Your process has no free session. The code makes room by itself (see [Why the connection may fail](#why-the-connection-may-fail-one-session-for-the-whole-process)); check the lines that follow. |
| The overlay says "Game is not running" | `Latir()` is not called on every frame. Once per second is too late. |
| The average shows `inf` | `FPSticks` is empty: `Latir()` has not run twice yet. |

## How the overlay gets its numbers

Status Monitor Overlay shows a frame rate (**FPS**) and a resolution (**RES**) for commercial games. It does not
compute either of them. SaltyNX ships a plugin called **NX-FPS** that is loaded into each game. It hooks the call
the game uses to show a frame (`nvnQueuePresentTexture`, `eglSwapBuffers` or `vkQueuePresentKHR`), counts the frames
and writes the results into a 4 KB page of memory that SaltyNX shares with the overlays.

This port goes through none of those three calls. Its Vulkan driver ([NVK](glossary.md#nvk)) is built into the NRO and
talks to the GPU directly, so there is nothing to hook. And there is a deeper reason: SaltyNX never loads anything into
a homebrew program. Its own log (`sdmc:/SaltySD/saltysd.log`) shows it:

```
SaltySD: PID 154 is not allowing debugging, error 0xfa01, aborting...
```

SaltyNX loads its plugins by debugging the game's process, which it cannot do with a homebrew program. Its code also
refuses on purpose: title IDs above `0x01FFFFFFFFFFFFFF` (the range that forwarders use, such as this port's
`05C7DBDFE0944000`) are treated as homebrew, and launching through hbloader (holding **R** over a game) is treated as
"title replacement".

So the port writes the block itself. Nothing has to be loaded into the game for that.

## Step 1, once at startup: get the shared page

SaltyNX creates two named ports, and each one accepts **a single connection at a time**:

| Port | What it does |
|---|---|
| `SaltySD` | Serves the shared page. This is the one to use. |
| `InjectServ` | Accepts the connection and then closes it (error `0xF601`). It does not serve the page. |

The commands are plain CMIF, so `serviceDispatch` over a `Service` holding the port's handle works. All of them send
the process ID:

| Command | What it does |
|---|---|
| 7 | Returns the handle of the shared page (one page, 0x1000 bytes). |
| 6 | Reserves room in the page. You send a size, it returns an offset. |
| 0 | Closes the session on SaltyNX's side. |

The order the port follows:

1. Connect to `SaltySD`. If a port connects but does not give the page, close it and try the other one in the same
   round. (Trying `InjectServ` first cost 65 seconds of detection time.)
2. Ask for the handle (command 7), then map it with `shmemLoadRemote` and `shmemMap`.
3. Look for our blocks in the page. **Only reserve (command 6) the ones that are missing.**
4. Send command 0, and **also close our own handle** with `svcCloseHandle`. The port accepts one connection: if we
   kept it open, the user's overlay could never connect again. The page stays mapped after the session is closed.

If SaltyNX is not ready yet, the port retries once per second.

### Why step 3 matters: the page runs out

SaltyNX hands out the page with a counter that only goes back to zero when it loads into a game. It never loads into
us, so the counter never goes back. If the port reserved a new block on every launch, the 4 KB would be used up after
about twenty launches. That is why it first looks for the block it left last time and reuses it.

### Why the connection may fail: one session for the whole process

A forwarder (hbloader inside an installed NSP) runs with a very small resource limit. Measured on the console:

```
sessions used / limit: 1 / 1
```

[libnx](glossary.md#libnx) already uses that one session for `sm:`, the service manager. So connecting to **any** port
fails with `0x10801` ("resource exhausted"), `sm:` included. It is not SaltyNX's fault.

Why a whole game can live with one session: a process is only charged for the objects it creates itself with a system
call. Everything it gets through another service (every `Service` that `sm` hands back, every open file) is charged to
that service. So one session is enough, until the process needs a second one.

The port makes room this way, in this order:

1. **Raise its own limit.** `svcGetInfo(InfoType_ResourceLimit, ...)` gives the handle of the process's resource
   limit, and `svcSetResourceLimitLimitValue` raises the number of sessions. The kernel asks for no special
   permission. But that call is SVC 0x7E, and calling a system call the process is not allowed to use **closes the
   game**, so the port asks first with `envIsSyscallHinted(0x7E)`.
2. **If that is not possible, release `sm:` for a moment** (`smExit`), check that the session was really freed,
   talk to SaltyNX, and give `sm:` back (`smInitialize`). It takes a few milliseconds. The port only does this from
   the third attempt, when the game has finished opening its services.

A useful check: try to connect to `sm:`. It always exists and accepts many sessions. If it fails too, the limit is
the process's, not the port's.

## Step 2, on every frame: the FPS block

The block is SaltyNX's `NxFpsSharedBlock`: **174 bytes**, packed, starting with the magic number `0x465053`. The overlay
walks the page four bytes at a time and uses the first block with that magic.

```c
struct resolutionCalls { uint16_t width, height, calls; } __attribute__((packed));

struct NxFpsSharedBlock {
    uint32_t MAGIC;              // 0x465053
    uint8_t  FPS;                // frames in the last second, as a whole number
    float    FPSavg;
    bool     pluginActive;       // the "are you alive?" answer
    uint8_t  FPSlocked, FPSmode, ZeroSync, patchApplied;
    uint8_t  API;                // 0 unknown, 1 NVN, 2 OpenGL, 3 Vulkan
    uint32_t FPSticks[10];       // time between the last ten frames
    uint8_t  Buffers, SetBuffers, ActiveBuffers, SetActiveBuffers;
    uint8_t  displaySync;
    resolutionCalls renderCalls[8];
    resolutionCalls viewportCalls[8];
    bool     forceOriginalRefreshRate, dontForce60InDocked, forceSuspend;
    uint8_t  currentRefreshRate;
    float    readSpeedPerSecond;
    uint8_t  FPSlockedDocked;
    uint64_t frameNumber;
    int8_t   expectedSetBuffers;
} __attribute__((packed));
```

### The overlay asks whether the game is alive

The overlay does not trust a block just because it found it. It asks:

```cpp
NxFps->pluginActive = false;
svcSleepThread(100'000'000);                       // 100 ms
if (NxFps->pluginActive) GameRunning = true;       // somebody set it back
```

The resolution screen asks the same way, writing `0xFFFF` into `renderCalls[0].calls` and checking whether it changed.

If the block is only updated once per second, the answer comes far too late and the overlay says "Game is not
running or it's incompatible". So the port writes the block **every time it presents a frame** (`Latir` in
`switch_saltynx.cpp`, called from the presentation counter in `switch_perf.cpp`). It writes:

- `pluginActive = true` and `API = 3` (Vulkan);
- the time since the previous frame into `FPSticks`, a ring of ten values, with `armGetSystemTick()`, the same
  19.2 MHz clock the overlay uses;
- `frameNumber`;
- the resolution into `renderCalls[0]` and `viewportCalls[0]`: width, height, and the frames of the last second as
  the count. The count must never be `0xFFFF`, because that is the overlay's question.

This is cheap: about 25 bytes written to memory that is already mapped. There are no system calls, no messages and
no locks.

Once per second, the profiler thread also writes `FPS` (frames in the last second) and `FPSavg` (average of the last
ten seconds), and checks that the magic is still there. If another program rewrote the page, the port finds or
reserves its block again. So the overlay can be opened at any moment without restarting the game.

### Why the overlay showed `inf`

The overlay computes its average from `FPSticks`: the tick frequency divided by the average of the ten values. With
the array full of zeros, that is a division by zero. When a block is created, the port fills `FPSticks` with the last
measured rate, and real times replace those values from the next frame.

## ReverseNX-RT

ReverseNX-RT lets you pretend the console is docked while it is in your hands ("Fake Docked"), or the opposite
("Fake Handheld"). Its overlay said "ReverseNX-RT is not running!", for the same reason: its block is created by its
plugin inside the game, and that plugin is never loaded into a homebrew program. The port creates the block itself:

```c
struct Shared {                 // magic "NXRT" = 0x5452584E
    uint32_t MAGIC;
    bool isDocked;              // the mode chosen in the overlay
    bool def;                   // "Controlled by system": if true, the real mode rules
    bool pluginActive;          // "the game has asked for the mode"
    uint8_t res;
    bool wasDDRused;
} __attribute__((packed));     // 9 bytes
```

With that block in place, its overlay works:

- **`pluginActive`** means "the game has asked for the mode". Without it, the overlay says "Game didn't check any
  mode!" and hides its controls. The port sets it every time it reads the mode.
- **`def`** ("Controlled by system") decides who rules. If it is `true`, the console rules, and `isDocked` is only a
  copy of the real mode. The port mirrors the real mode there, so the overlay shows it correctly. `isDocked` only
  counts when `def` is `false`.
- A new block starts with `def = true`: the console decides until the player chooses otherwise.

All the places that used to ask the console for its real mode now ask `ModoBase` in `switch_saltynx.cpp`, which
applies those rules. That covers the game's output resolution, the window size and the clocks.

### The resolution is chosen at startup

The game chooses its output resolution once, about one second after it starts: 1280x720 in handheld mode and
1920x1080 docked (with `nfsmw_resolucion_interna = "automatico"`). Changing the mode while playing changes the window
size, but the game keeps the resolution it chose, scaled to the screen, until you restart it. The same happens when
you dock or undock the console.

The shared page is kept until the console restarts, and the port reuses its ReverseNX-RT block on every launch. So a
mode chosen in the overlay is still there the next time the game starts, and that is when it applies.

Changes from ReverseNX-RT do not send any system event, so the window code checks the mode every 100 ms
(`windowed_app_context_switch.cpp`). `WindowSwitch::QueryDisplayResolution` only uses the console's display
resolution when it matches the mode that rules; otherwise it uses 1920x1080 or 1280x720.

## Clocks that follow ReverseNX-RT

ReverseNX-RT only changes what the game sees. The console's clocks are set by the clock sysmodule (sys-clk or one of
its forks, such as Horizon OC). It picks the profile column from `Board::GetProfile()`, which asks the hardware
(`apmExtGetPerformanceMode()`), so "Fake Docked" with the console in your hands keeps the handheld clocks. sys-clk has
a command to follow ReverseNX-RT, but in Horizon OC's forks (`sys-clk-plus`, `sys-clk-bugfixes`) it does nothing:
`IpcService::SetReverseNXRTMode(mode) { return 0; }`, with the call to `rnxSync->ToggleSync(...)` commented out.
`hanai3Bi/Switch-OC-Suite` and `rashevskyv/sys-clk-OC` do follow it by themselves.

When the setting `nfsmw_switch_relojes_reverse` is on (it is **off** by default), the port asks the clock sysmodule
itself. It does this whenever the mode that rules is different from the real one, in both directions:

- **Fake Docked** with the console in your hands: the port reads the clocks you set in the docked column of your
  profile (the game's profile, otherwise the global one) and applies them. That is an overclock, so it is not useful
  for measuring.
- **Fake Handheld** with the console in the dock: it applies your handheld column. If that column is empty, it applies
  the stock handheld clocks: GPU 307.2 MHz and memory 1331.2 MHz.

How it talks to the sysmodule:

- It looks for the service under several names, because they change between forks: `sys:clk`, `hoc:clk` (Horizon
  OC), `hocclk`, `sysclk`, `clk:sys` and `sys:oc`. Then it checks with `GetApiVersion` (command 0) that the service
  really speaks the sys-clk interface.
- `GetProfiles` (command 5, an output buffer of `u32 mhz[5][3]`) gives the MHz you configured for each mode. The port
  asks for the game's title ID first and then for the global profile (`0xA111111111111111`), as the sysmodule
  itself does. `SetOverride` (command 8, in Hz, so MHz × 1,000,000) applies them. An override wins over the profile.
- An override never expires. The port releases it when the modes match again, when the setting is turned off and when
  the game closes normally. It only releases the clocks it set itself, never one you forced by hand. If the game
  crashes, the override stays until you change it in the overlay or restart the console.

Horizon OC Monitor keeps saying "Profile: Docked" or "Handheld" from the real hardware, even when the MHz have
changed.

> [!WARNING]
> **Never call `smGetService` for a service that may not exist.** If the service is not registered, the call does not
> fail: the thread waits forever for someone to register it. And since the process has a single `sm` session, every
> other request to `sm` waits behind it. This once froze the thread that updates the overlay block, the ReverseNX-RT
> mode and, behind it, the audio. It looked like a game bug. Ask first with `sm` command **65100**
> (`AtmosphereHasService`), which answers yes or no and never waits. It is TIPC on firmware 12.0.0 and later, CMIF
> before.

## Settings

| Setting | Default | What it does |
|---|---|---|
| `nfsmw_switch_saltynx` | on | Publishes the FPS and the resolution for the overlays, and follows the ReverseNX-RT mode. Without SaltyNX it does nothing. |
| `nfsmw_switch_relojes_reverse` | off | Lets the clocks follow ReverseNX-RT when its mode does not match the real one. |

Both are in the **Graficos** category of the Debug Menu and in `nfsmw.toml`.

## Checking that it works

Everything is written to the logs in the `logs/rex` folder next to the NRO (`sdmc:/switch/nfsmw/logs/rex/`):

- `rex_stderr.log`, lines starting with `[saltynx]`: which port connected, whether the blocks were created or reused,
  the seconds it took to publish (`PUBLICADO a los ... s`), and, if it fails, the error code and the sessions in use
  against the limit. Lines starting with `[relojes]` say which clock sysmodule was found and which clocks were
  applied or released.
- `rex_perfil.log`, the `Reverse-NX:` line, in the report written every 10 seconds: what ReverseNX-RT says, who rules ("Controlled by
  system"), whether the game has asked, the real mode, and the mode the game obeys. When the resolution does not
  change as expected, read this line first.

## Error codes

| Code | Meaning | Where |
|---|---|---|
| `0x10801` | Resource exhausted: the process cannot open another session, or the port accepts no more | `svcConnectToNamedPort` |
| `0xF601` | Connection closed: the server closed the session, because it does not serve that command | command 7 on `InjectServ` |
| `0xF201` | Not found: the port does not exist, so SaltyNX is not installed | `svcConnectToNamedPort` |
| `0xfa01` | Invalid state: the process does not allow debugging | `svcDebugActiveProcess`, in SaltyNX's log |

## Checklist for another port

1. Connect to `SaltySD` first, and do not count a connection as success until you have the page.
2. Make room for the session before connecting (raise the limit, or release `sm:` for a moment).
3. Command 7, map the page, look for your blocks, reserve only the missing ones (command 6), command 0, then
   `svcCloseHandle`.
4. On every frame: `pluginActive`, `FPSticks`, `renderCalls[0]`, `viewportCalls[0]` and `frameNumber`.
5. Once per second: `FPS`, `FPSavg`, `API`, and check that the magic is still there.
6. Create the `NXRT` block too, set `pluginActive` when the game reads the mode, and obey `isDocked` only when `def` is
   `false`.
7. Log what happens to `stderr`. Without it, every test on the console is done blind.

## Sources

- `masagrator/SaltyNX`: `saltysd_proc/source/service.c` (the commands) and `saltysd_proc/source/hijack.c` (why it does
  not load into homebrew, and when the page counter resets).
- `masagrator/Status-Monitor-Overlay`: `source/Utils.hpp` (the block, the magic search and the 100 ms check) and
  `source/modes/Resolutions.hpp` (the resolution check).
- `masagrator/ReverseNX-RT`: `Overlay/source/main.cpp` (the 9-byte block and which fields the overlay writes).
- `Horizon-OC/sys-clk-plus`: `common/include/sysclk/ipc.h` and `sysmodule/src/clock_manager.cpp` (the clock commands).
- `Atmosphere-NX/Atmosphere`: `sm_user_interface.hpp` (command 65100).

Thanks to **MasaGratoR**, the author of SaltyNX and Status Monitor Overlay, for confirming that the conversation with
SaltyNX happens only once, and that after that the page is used like any other memory.

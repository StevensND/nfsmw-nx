# SaltyNX template for homebrew programs

Two files that make SaltyNX work with a homebrew program (.nro): Status Monitor Overlay (and its forks, such as
Horizon OC Monitor) shows its FPS and resolution, and ReverseNX-RT can switch it between handheld and docked mode.
They only need [libnx](https://github.com/switchbrew/libnx), and they are the same code NFSMW-NX uses
(`sdk/src/ui/switch_saltynx.cpp`), without anything specific to this project.

How it works, step by step: [docs/console-overlays.md](../../docs/console-overlays.md).

## Requirements

- SaltyNX 0.7.0 or later on the console. Without it, the code does nothing.
- C++17 or later (devkitA64).

## Usage

Add `switch_saltynx.cpp` and `switch_saltynx.h` to your program and call:

```cpp
#include "switch_saltynx.h"

// 1. At startup.
saltynx::Init();

// 2. Every time you present a frame (after vkQueuePresentKHR, eglSwapBuffers, nwindowQueueBuffer...).
saltynx::Heartbeat(width, height);

// 3. Once per second, from any thread.
saltynx::Update(fps_last_second, fps_average, width, height, frames_since_startup);

// 4. Wherever your program asks whether the console is docked, to obey ReverseNX-RT.
bool docked = saltynx::IsDocked(appletGetOperationMode() == AppletOperationMode_Console);
```

`switch_saltynx.h` has one setting, `kApi`: the graphics API your program uses (1 NVN, 2 OpenGL, 3 Vulkan).

Warnings go to `stderr`, on lines that start with `[saltynx]`. Send `stderr` to a file on the SD card (for example
with `freopen`) to see which port connected, or why it failed.

## License

GPL-3.0, like the rest of NFSMW-NX.

<div align=center>

<img src="extras/banner.png" alt="Need for Speed: Most Wanted" width="60%">

</div>
<h1 align=center>Need for Speed: Most Wanted — Nintendo Switch port</h1>

A native Nintendo Switch port of the Xbox 360 version of **Need for Speed: Most Wanted** (2005).

It's not an emulator. The PowerPC code of the game's `default.xex` is statically recompiled to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and compiled for the Switch and the game is drawn by a native
Vulkan renderer running on NVK (Mesa). It targets **30 FPS at the console's stock clocks** with no overclock.

> [!NOTE]
> You need your own copy of the Xbox 360 game: a disc image (`.iso`) or the extracted disc with its `default.xex`.

## Supported editions

One NRO per edition of the game (the installer page picks the right one automatically):

| File | Edition |
|---|---|
| `nfsmw-nx-pal-es.nro` | PAL - Spanish & Brazilian Portuguese (Fan Version) |
| `nfsmw-nx-pal-en.nro` | PAL - English & Russian (Fan Version) |
| `nfsmw-nx-pal-fr.nro` | PAL - French |
| `nfsmw-nx-pal-de.nro` | PAL - German |
| `nfsmw-nx-pal-it.nro` | PAL - Italian |
| `nfsmw-nx-pal-pl.nro` | PAL - Polish (Fan Version) |
| `nfsmw-nx-usa.nro` | NTSC-U |
| `nfsmw-nx-jpn.nro` | NTSC-J |

## How to install

1. Open the installer page: **https://stevensnd.github.io/nfsmw-nx-installer/**
2. Choose your format (**Disc image (.iso)** or **XEX format**, the folder with `default.xex`, `Movies` and `NFS`)
   and press **Create nfsmw-nx.zip**. Everything runs in your browser and the game files never leave your
   computer: the page detects your edition, downloads its build and makes the shaders from your disc.
3. Extract the downloaded `nfsmw-nx.zip` into `sdmc:/switch/`.

Your `nfsmw-nx` folder should look like this:

```text
/switch/nfsmw-nx/
  nfsmw-nx.nro
  nfsmw.toml                (settings)
  nfsmw_shaders.nfsp        (the game's shaders, made from your disc)
  game_root/                (the files of your disc)
```

Launch with a game override (hold **R** while starting an installed title) or a forwarder. Album applet mode does
not provide enough memory. If you use a forwarder, set it to a **39-bit address space**.

I suggest using the latest version of [Sphaira](https://github.com/NaGaa95/sphaira) to generate the forwarder.

Finally **CHECK [THIS SCREENSHOT](https://i.imgur.com/W4hpDcY.jpeg)** to know which options you must use to generate
the forwarder.

> [!NOTE]
> **Updating:** open the installer page again and press **Create nfsmw-nx-update.zip**. It only rebuilds
> `nfsmw-nx.nro`, `nfsmw.toml` and `nfsmw_shaders.nfsp`, so there is no need to copy the game files again: extract
> it over your existing folder.

> [!CAUTION]
> **The first race after** installing or updating compiles the graphics pipelines it has not seen yet, so **IT CAN STUTTER
> for a few seconds**. They are saved on the SD card and later sessions start smoothly.

## Controls

Each Switch button acts as the Xbox 360 button with the same letter, so the letter in the on-screen prompts is the
button to press: **A** accepts and **B** goes back, as in other Switch games.

<div align="center">

| Switch | Xbox 360 |
| --- | --- |
| A | A |
| B | B |
| X | X |
| Y | Y |
| ZR / ZL | RT / LT |
| R / L | RB / LB |
| + | Start |
| − | Back |
| Sticks / D-pad | Sticks / D-pad |

</div>

To use the face buttons by position instead, as on an Xbox pad (B acts as A, A as B, Y as X and X as Y), set
`input_xbox_layout = true` in `nfsmw.toml`.

## Settings

`nfsmw.toml`, next to the NRO, holds the settings and each one is described in the file. Several of them can also be
changed while playing from the Debug Menu (L + R + Right).

### Color filter (the "piss filter")

The game's "visual treatment", which players call the "piss filter", gives the city its yellow tint, with some
desaturation and a dark vignette. To make it softer or turn it off, change `nfsmw_tratamiento_visual` in
`nfsmw.toml`, or in the Debug Menu (L + R + Right, category **Graficos**) while playing:

- `original` (default): as on the Xbox 360;
- `suave`: half as strong;
- `apagado`: no filter. The glow and the fades to black stay the same.

It costs nothing.

## Debug Menu

**It's only available in Spanish for now.** Press **L + R + Right** (D-pad) while playing to open it and again to close it.
It can be used with the Joy-Con, moving with the D-pad and scrolling with the left stick, or with the touch screen.
While it is open, the controls go to the menu and not to the game.

- **Search box** at the top: finds a setting by its name or description.
- **Categories** on the left and the settings of the selected one on the right, each with its value (a checkbox, a
  list or a number). The color of the name tells when a change takes effect: green at once, yellow after restarting
  the game, and red is read-only.
- **Save to config** at the bottom: writes the changes to `nfsmw.toml` so they are kept. It rewrites the file with only
  the settings that differ from the program's defaults, without its comments. Without saving, the changes last until
  you close the game.

The categories:

- **Graficos**: internal resolution, frame rate limit (60 or 30), the GPU clock requested in handheld mode, FXAA
  antialiasing, the sky glow, the game's color filter, and the options for the console overlays (SaltyNX) and
  ReverseNX-RT.
- **Graficos > Posproceso**: color filters for the final image, as presets (`cine`, `sepia`, `noir`, `frio`,
  `calido`, `vivo`, `matrix`, `crt`) or `personalizado` with your own brightness, contrast, saturation, vibrance,
  temperature, gamma, vignette and scanlines.
- **NFSMW**: the port's own settings: renderer, shadows, reflections, streaming, audio, the game functions that run as
  native code, and diagnostics. Many of them are described in `nfsmw.toml`; change them only to try something.
- **The rest** (Audio, GPU, Input, Kernel, Log, UI...) come from the ReXGlue SDK and are mostly for development and
  debugging.

> [!NOTE]
> To see the FPS, the resolution and information about the clocks, Horizon OC Monitor is a better choice: it shows more
> information than the Debug Menu currently does.

## Resolution

The resolution is automatic by default (`nfsmw_resolucion_interna = "automatico"`): **1280x720** in handheld mode and
**1920x1080** docked. It's chosen when the game starts: if you dock or undock the console while playing, the game keeps
the resolution, scaled to the screen until you restart it. ReverseNX-RT's Fake Docked and Fake Handheld count as well:
choose the mode in its overlay and restart the game. The choice is kept until you restart the console.

To use one resolution in both modes, set it in `nfsmw.toml` (or in the Debug Menu, category **Graficos**, then
**Save to config**) and restart the game:

- `1280x720`: the resolution of the Xbox 360 version. Docked, it can give a higher frame rate than `1920x1080`, which
  has 2.25 times the pixels while the docked GPU clock is only 1.67 times the handheld one.
- `1920x1080`: sharper, at a high GPU cost, above all in handheld mode.
- `1024x576`, `640x360` and `640x480` only shrink the image after it is drawn at 1280x720, so they do not raise the
  frame rate.

> [!NOTE]
> If you're having bad performance on Docked and don't want to apply a higher OC + you want more FPS, set the resolution to 1280x720.

## Performance

In handheld mode the port asks the system for its official 460.8 MHz GPU mode, a standard performance configuration
of the console and not an overclock. Races average 32 to 35 FPS; the heaviest area measured, the exit of Heritage
Heights, runs at 21-22 FPS for about 20 seconds.

How it went from a few frames per second to this is explained step by step in
[docs/performance-history.md](docs/performance-history.md).

## Overclocking

The port is made to run at the console's stock clocks **with no overclock**. These are the clocks it runs at by default:

<div align="center">

| Mode | CPU | GPU | Memory |
| --- | --- | --- | --- |
| Handheld | 1020 MHz | 460.8 MHz | 1331.2 MHz |
| Docked | 1020 MHz | 768 MHz | 1600 MHz |

</div>

**[GAMEPLAY USING THE DEFAULT CLOCKS](https://youtu.be/xtk1l8mpr4o?si=a5XEbRV5VwV_Ym1O)**

The CPU is what limits it most (the game runs
on three cores at 1020 MHz) and in handheld mode the GPU as well.

> [!CAUTION]
> Overclocking pushes the console beyond what it was designed for and you do it at your own risk. An unstable memory
> overclock can corrupt the NAND or the SD card: make a backup first and test every change.

These guides explain how to do it:

- **[How to get 60 FPS](https://rentry.org/howtoget60fps)**: the tools you need (Horizon OC, Ultrahand, Status
  Monitor...) and how to set them up.
- **[Mariko guide](https://rentry.co/mariko)**: overclocking the Switch V2, Lite and OLED.
- **[Erista guide](https://rentry.co/erista)**: overclocking the original 2017 Switch.
- **[How to test stability](https://rentry.co/howtoteststability)**: how to check that your CPU, GPU and memory clocks
  are stable.

For this port:

- In Horizon OC, set the clocks with **Edit App Profile** while the game is running. The App ID is the one of the game
  you hold **R** on to launch it, or your forwarder's.
- Status Monitor shows the port's frame rate and resolution if SaltyNX is installed ([how it works](docs/console-overlays.md)).

## How it works

- **The game's code is translated, not emulated.** [ReXGlue](https://github.com/rexglue/rexglue-sdk) translates every
  function of the game's program (`default.xex`) to C++, which is then compiled for the Switch. ReXGlue also gives the
  game what it expects from an Xbox 360: its system, files, audio and controllers. This port adds the Switch part:
  memory, threads, crash handling, clocks, audio output and showing the frames on screen.
- **Its own renderer.** Imitating the Xbox 360 GPU was far too slow on the Switch. Instead, the port reads the list of
  commands the game sends to the GPU and draws the same frame with Vulkan. The busiest parts of the game's graphics
  code run as native code, each one checked against the original.
- **Shaders translated beforehand.** The game's GPU programs (shaders) are translated with
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and DXC into one library file. The installer page does this
  from your disc.
- **Its own graphics driver.** Vulkan runs on NVK, from the Mesa project, through
  [mesa-switch](https://github.com/danfromtico/mesa-switch), with this port's changes in `mesa/`: skipping hidden
  pixels (ZCULL), shader compiler fixes and cheaper draws.
- **Optimization.** The code is built with LTO, PGO and function ordering, the CPU cost of each draw is cut down, the
  busiest game functions run as native code, and the GPU time per frame is reduced. The few changes that alter the
  image (a cheaper shadow filter, no radial blur on the final image, no vegetation in the shadow maps) are settings in
  `nfsmw.toml`.

> [!NOTE]
> The PGO profile is not complete. It comes from a small test on the console, done only to check that PGO improves the
> port's performance, not from playing through the whole game.

## Documentation

For anyone porting another Xbox 360 game, or curious about how this one was done. **Start with
[docs/README.md](docs/README.md)**: it explains in six steps how the whole port fits together, and what to read for
what you want to do. Every technical word is explained in the [glossary](docs/glossary.md).

<div align="center">

| Document | What it explains |
| --- | --- |
| [docs/glossary.md](docs/glossary.md) | Every technical word, in plain words |
| [docs/building.md](docs/building.md) | How to build the NRO, the driver and the shader library, step by step |
| [docs/porting-another-game.md](docs/porting-another-game.md) | What you can reuse for another game, and in which order to work |
| [docs/native-renderer.md](docs/native-renderer.md) | How the port draws the game with Vulkan |
| [docs/shaders.md](docs/shaders.md) | How the game's shaders are translated, and what had to be fixed |
| [docs/toolchain.md](docs/toolchain.md) | How the game's code is translated and compiled, and the build options that make it faster |
| [docs/mesa.md](docs/mesa.md) | The graphics driver and this port's changes to it |
| [docs/platform-notes.md](docs/platform-notes.md) | Things about the Switch system that cost a lot of time to find out |
| [docs/audio-and-video.md](docs/audio-and-video.md) | The game's audio and cutscenes on the Switch |
| [docs/editions.md](docs/editions.md) | How every edition and language of the game is supported |
| [docs/measuring.md](docs/measuring.md) | How to measure performance on the console without being misled |
| [docs/performance-history.md](docs/performance-history.md) | How the frame rate went from a few FPS to about 30, step by step |

</div>

## Build

devkitA64 and libnx from [devkitPro](https://devkitpro.org), CMake, Ninja, Python, the Vulkan driver built from
[mesa-switch](https://github.com/danfromtico/mesa-switch) with `mesa/mesa-switch-nfsmw.patch`, and your own copy of
the game for the code generator. The whole process is in [docs/building.md](docs/building.md).

<div align="center">

| Folder | Contents |
| --- | --- |
| `app/` | The game: hooks, native renderer, audio, video, configuration, CMake project |
| `sdk/` | ReXGlue SDK with the Horizon layer and the code generator changes |
| `shaders/` | XenosRecomp with this port's changes, the library tools and their WebAssembly builds |
| `mesa/` | The patch for mesa-switch |
| `pgo/` | The profiles for profile guided optimization, one per edition |
| `tools/` | Code generation steps, build scripts, edition support |
| `docs/` | Documentation |

</div>

## Credits

- **Electronic Arts and EA Black Box** — creators of Need for Speed: Most Wanted. This is an unofficial, fan-made
  port with no affiliation.
- **[madelrandel-blip](https://github.com/madelrandel-blip/NFSMW-Recompiled)** — NFSMW Recompiled, the recompilation
  project this port started from.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)** — the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)** — XenosRecomp, the Xenos shader translator.
- **[danfromtico](https://github.com/danfromtico/mesa-switch)** and **[NaGaa95](https://github.com/NaGaa95/mesa-switch)** — mesa-switch: Mesa, NVK and NAK on Horizon.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)** — devkitA64 and libnx.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Support

**[HERE](https://linktr.ee/stevensmods)** are my social media

If you enjoy my work and want to support me:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/stevenss)

## Legal

No affiliation with Electronic Arts. "Need for Speed" and "Need for Speed: Most Wanted" are trademarks of Electronic
Arts Inc. This repository contains no assets or program code from the original game. Users must provide their own
legally obtained copy, and the installer page makes the package from it on their own computer. Running homebrew
requires custom firmware, which violates Nintendo's ToS and can get a console banned — your call.

Source code is provided under the GPL-3.0 License (see [LICENSE](LICENSE)), inherited from NFSMW Recompiled. The SDK
changes are under the SDK's BSD-3-Clause license and the shader translator and Mesa changes under MIT, so other ports
can reuse them (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

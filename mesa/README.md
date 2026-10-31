# The graphics driver (Mesa and NVK)

## In short

- The Switch has no Vulkan driver that homebrew can use, so this port carries its own inside the NRO: **NVK**, the
  open source Vulkan driver for NVIDIA GPUs, which is part of the **Mesa** project.
- It comes from [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch), a version of Mesa 26.2.1 that
  runs on the Switch's operating system (called Horizon).
- This port adds its own changes on top. They are all in one file of this folder: `mesa-switch-nfsmw.patch`.
- Building Mesa gives a library, `libvulkan.a`, and the NRO includes it.

## Getting the same driver as the released NROs

1. Download mesa-switch and go to the version this port started from (commit `1a8c1a66d6f`):

   ```sh
   git clone https://github.com/danfromtico/mesa-switch.git
   cd mesa-switch
   git checkout 1a8c1a66d6f
   ```

2. Apply this port's changes. After this you have exactly the driver source the released NROs were built with
   (35 changed files):

   ```sh
   git apply ../nfsmw-nx/mesa/mesa-switch-nfsmw.patch
   ```

3. Build it. The first time, use mesa-switch's own script, `build-unified.sh` (its README explains how). Later
   rebuilds are quicker with `build_mesa_msys2.sh`, in this folder. The result is an SDK folder. On Linux and WSL,
   `build-unified.sh` does not run (it needs MSYS2): use `linux/build.sh`, in this folder, instead
   ([docs/building-linux.md](../docs/building-linux.md)).
4. Tell the app where that SDK folder is when you configure it:
   `-DREXGLUE_SWITCH_NVK_SDK=<sdk>/opt/devkitpro/portlibs/switch`. The whole process is in
   [docs/building.md](../docs/building.md).

## What the patch changes

One line per change. Why each one was needed, and how much it gained, is in [docs/mesa.md](../docs/mesa.md).

| Change | What it does | Files |
|---|---|---|
| ZCULL | The GPU skips pixels hidden behind others before painting them. A big saving of GPU time. | `nouveau_horizon*.c/h`, `nvkmd_switch_pdev.c`, `nvkmd_switch_dev.c`, `nvk_image.c` |
| Faster draws | Less CPU work inside the driver for each draw the game makes. | `nvk_cmd_draw.c`, `nvk_cmd_buffer.c/h`, `vk_pipeline.c/h` |
| Set 4 by differences | For each draw, only the game constants that changed since the previous draw are sent (the game keeps them in descriptor set 4). | `nvk_cmd_buffer.c/h`, `nvk_cmd_draw.c` |
| Memory without CPU cache | Memory the CPU only writes to (commands, uploads) skips the CPU cache, which is cheaper on the Switch. | `nvk_device.c/h`, `nvk_cmd_pool.c`, `nvk_mem_stream.c` |
| Copy engine | Buffer copies use the GPU's copy unit instead of a small shader. Off unless you turn it on. | `nvk_cmd_meta.c` |
| Uniform buffers | Shaders read their uniform buffers through the GPU's constant memory. The upstream way hung the GPU on some shaders; this way they work and come out smaller. | `nvk_nir_lower_descriptors.c` |
| Shader compiler (NAK) | Instructions are ordered for how slow the Switch GPU's memory is, small branches become straight code, and a wrong result of one instruction on this GPU generation is fixed (it made the sea black). | `opt_instr_sched_common.rs`, `nak_nir.c`, `sm50.rs` |
| Shader cache | Shaders compiled before these compiler changes are not reused. | `nvk_shader.c` |
| Newer mesa-switch fixes | Fixes taken from later commits of danfromtico/mesa-switch. | `nvk_mem_arena.*`, `nvk_descriptor_table.*`, `nvk_descriptor_set.c`, `nvk_query_pool.c`, `nvk_heap.h`, `nvk_queue.c` |
| Build | Makes it build on Windows, in MSYS2. | `build-*.sh`, `rustc-*-wrapper.sh`, `bindgen-switch-wrapper.sh`, `meson.build` |

## Which changes only affect the Switch

If you build this source for another platform, for example to compare with a PC:

- **Only on the Switch, because the files only exist there:** `src/nouveau/horizon/` and
  `src/nouveau/vulkan/nvkmd/switch/`. ZCULL is here.
- **Only on the Switch, because the code is inside `#ifdef HAVE_SWITCH_PLATFORM` blocks:** the faster draws, the set 4
  by differences and a structure the driver shares with the app to measure them. They are in `nvk_cmd_buffer.c/h`,
  `nvk_cmd_draw.c` and `vk_pipeline.c/h`.
- **On every platform:** the shader compiler changes, the newer mesa-switch fixes and the build changes.

### Where does `HAVE_SWITCH_PLATFORM` come from?

You will not find it written in the patch or in any header, because Mesa's build file, `meson.build`, creates it:

1. Every build has a list of platforms. When the build is for the Switch's system (`horizon`), the list is `switch`
   ([lines 478-479](https://github.com/danfromtico/mesa-switch/blob/1a8c1a66d6f/meson.build#L478-L479)).
2. For each platform in the list, it adds a flag called `HAVE_<PLATFORM>_PLATFORM`
   ([lines 620-622](https://github.com/danfromtico/mesa-switch/blob/1a8c1a66d6f/meson.build#L620-L622)). It is the
   same way Mesa gets `HAVE_X11_PLATFORM` or `HAVE_WAYLAND_PLATFORM` on Linux.
3. So a Switch build compiles every file with `HAVE_SWITCH_PLATFORM`, and the code inside those blocks is included.
   A PC build does not have the flag, and leaves that code out.

## Turning changes on and off

There are two ways:

- **The port's own settings.** ZCULL, the faster draws and the set 4 by differences are requested by the port, with
  settings in `nfsmw.toml` (also in the Debug Menu), all on: `nfsmw_nativo_zcull`, `nfsmw_nativo_set4_diferencias`
  and, for the faster draws, `nfsmw_nativo_nvk_emision`, `nfsmw_nativo_nvk_cbufs`, `nfsmw_nativo_nvk_dinamico` and
  `nfsmw_nativo_nvk_precarga`. The faster draws and the set 4 by differences also check themselves: on some draws they
  compare their result with the normal way, and if they ever see a difference, they turn off.
- **Environment variables for the driver.** You do not set them in Windows: write them in `nfsmw.toml`, in the setting
  `nfsmw_mesa_entorno`, separated by `;`, for example `nfsmw_mesa_entorno = "NVK_COPY_ENGINE=1;NVK_SHADER_STATS=1"`.
  Empty, as released, means the driver's normal behavior.

| Variable | Normally | What it does |
|---|---|---|
| `NVK_COPY_ENGINE` | off | `1` makes buffer copies use the copy unit. |
| `NVK_SWITCH_CPU_WRITE_MEM_UNCACHED` | on | `0` puts the CPU cache back on the memory the CPU only writes to, to compare. |
| `NVK_SWITCH_DYN_UBO_DELTA` | on | `false` turns off the set 4 by differences. |
| `NVK_SWITCH_NO_UBO_CBUF` | off | `1` goes back to the upstream way of reading uniform buffers. |
| `NVK_SHADER_STATS` | off | `1` writes to the log how many registers each shader uses. |
| `NVK_SUBTILING_KNOB` | the Switch GPU's value | A number, to try other values of this GPU tuning setting without rebuilding. Only for experiments. |

## After changing the driver

1. Rebuild and install it with `build_mesa_msys2.sh`, from an MSYS2 MINGW64 window.
2. Copy `libvulkan.a` (the shader compiler, NAK, is inside it), `libnvk.a` and `libnak_rs.a` to the SDK folder the app
   uses.
3. Delete `app/out/sw8/nfsmw` and `app/out/sw8/nfsmw.nro` before building the app. Otherwise the build does not notice
   that the driver changed, and the NRO keeps the old one.
4. If you changed the shader compiler (NAK), raise the revision number in `nvk_shader.c`. On the Switch that number is
   what tells old compiled shaders from new ones: without it, the game would keep using shaders compiled with the old
   compiler.

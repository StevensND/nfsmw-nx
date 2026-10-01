/*
 * SaltyNX support for a homebrew program (.nro): FPS and resolution in the console overlays (Status Monitor Overlay
 * and its forks, such as Horizon OC Monitor), and the handheld/docked mode chosen in ReverseNX-RT.
 *
 * Template taken from NFSMW-NX (sdk/src/ui/switch_saltynx.cpp). It only needs libnx.
 * How it works, step by step: docs/console-overlays.md.
 */
#ifndef SWITCH_SALTYNX_H_
#define SWITCH_SALTYNX_H_

#include <cstdint>

namespace saltynx {

// Graphics API written in the block: 0 unknown, 1 NVN, 2 OpenGL, 3 Vulkan. Change it to match your program.
constexpr uint8_t kApi = 3;

// At startup. Connects to SaltyNX once, maps its shared page and finds or creates the two blocks (FPS and
// ReverseNX-RT). Without SaltyNX it does nothing. It can be called again: until there is a valid block it retries.
void Init();

// Once per second, from any thread: frames presented in the last second, their average, the resolution being
// presented and the frames since startup. If the block was lost (or SaltyNX started after you), it finds it again.
void Update(double fps_last_second, double fps_average, uint32_t width, uint32_t height, uint64_t frames);

// Every time a frame is presented. The overlay clears its "alive" mark and only waits 100 ms for it to come back,
// so once per second is not enough. It writes about 25 bytes to memory that is already mapped: no system calls.
void Heartbeat(uint32_t width, uint32_t height);

// Turns everything on or off. On by default.
void SetEnabled(bool enabled);

// The docked mode to obey: ReverseNX-RT's when the player chose one, otherwise `real`, the console's real mode
// (appletGetOperationMode() == AppletOperationMode_Console).
bool IsDocked(bool real);

// What the ReverseNX-RT block says, for your own logs. `controlled_by_system` is its "Controlled by system": while it
// is true, `docked` only mirrors the real mode. Without a block, `present` is false and the rest means nothing.
struct ReverseNxState {
  bool present;
  bool docked;
  bool controlled_by_system;
  bool game_asked;
};
ReverseNxState GetReverseNxState();

}  // namespace saltynx

#endif  // SWITCH_SALTYNX_H_

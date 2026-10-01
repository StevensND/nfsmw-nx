/*
 * SaltyNX support for a homebrew program (.nro). Template taken from NFSMW-NX (sdk/src/ui/switch_saltynx.cpp).
 *
 * The FPS counter of the console overlays (Status Monitor Overlay and its forks, such as Horizon OC Monitor) is not
 * computed by them: they read it from a shared memory page published by SaltyNX. That page is filled in by NX-FPS, a
 * plugin SaltyNX loads into each game, which hooks the game's present call (nvnQueuePresentTexture, eglSwapBuffers
 * or vkQueuePresentKHR). SaltyNX never loads itself into a homebrew program, so here the program writes the block
 * itself. The format is SaltyNX's (struct NxFpsSharedBlock, 174 packed bytes, magic 0x465053), and the overlay finds
 * it by scanning the page 4 bytes at a time.
 *
 * The ReverseNX-RT block (magic "NXRT") is created the same way, so the program can follow the handheld/docked mode
 * chosen in its overlay without docking the console.
 *
 * If SaltyNX is not installed, its port does not exist and nothing is done here.
 *
 * Warnings go to stderr, with lines starting with "[saltynx]". Send stderr to a file to read them on the console.
 *
 * References: masagrator/SaltyNX (saltysd_proc/source/service.c and hijack.c), masagrator/Status-Monitor-Overlay
 * (source/Utils.hpp and source/modes/Resolutions.hpp) and masagrator/ReverseNX-RT (Overlay/source/main.cpp).
 */

#include "switch_saltynx.h"

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace saltynx {
namespace {

constexpr uint32_t kMagicFps = 0x465053;          // "SPF": the NX-FPS block
constexpr uint32_t kMagicReverseNx = 0x5452584E;  // "NXRT" in little-endian
constexpr size_t kSharedSize = 0x1000;            // SaltyNX maps one page

/* Resolution calls as the overlay reads them: width, height and how many times. */
struct ResolutionCalls {
  uint16_t width;
  uint16_t height;
  uint16_t calls;
} __attribute__((packed));

/* SaltyNX's struct NxFpsSharedBlock. The overlay checks that it is 174 bytes. */
struct FpsBlock {
  uint32_t magic;
  uint8_t fps;
  float fps_average;
  bool plugin_active;
  uint8_t fps_locked;
  uint8_t fps_mode;
  uint8_t zero_sync;
  uint8_t patch_applied;
  uint8_t api;
  uint32_t ticks[10];
  uint8_t buffers;
  uint8_t set_buffers;
  uint8_t active_buffers;
  uint8_t set_active_buffers;
  uint8_t display_sync;
  ResolutionCalls render[8];
  ResolutionCalls viewport[8];
  bool force_original_refresh_rate;
  bool dont_force_60_in_docked;
  bool force_suspend;
  uint8_t current_refresh_rate;
  float read_speed_per_second;
  uint8_t fps_locked_docked;
  uint64_t frame_number;
  int8_t expected_set_buffers;
} __attribute__((packed));

static_assert(sizeof(FpsBlock) == 174, "the overlay expects 174 bytes");

/* ReverseNX-RT's struct Shared (9 bytes). */
struct ReverseNxBlock {
  uint32_t magic;
  bool docked;
  bool controlled_by_system;  // "def" in ReverseNX-RT
  bool plugin_active;         // "the game has asked for the mode"
  uint8_t resolutions;
  bool ddr_used;
} __attribute__((packed));

static_assert(sizeof(ReverseNxBlock) == 9, "ReverseNX-RT uses 9 bytes");

void Warn(const char* text) { std::fprintf(stderr, "%s\n", text); }
void WarnNum(const char* text, long long number) {
  std::fprintf(stderr, "%s 0x%llX (%lld)\n", text, (unsigned long long)number, number);
}
void WarnPair(const char* text, long long a, long long b) {
  std::fprintf(stderr, "%s %lld / %lld\n", text, a, b);
}

SharedMemory g_memory{};
bool g_mapped = false;
uint8_t* g_base = nullptr;  // start of the shared area, whether from IPC or from the scan
size_t g_bytes = 0;
/* Written by Init/Update and read by Heartbeat on every present, possibly from another thread, hence atomic. */
std::atomic<FpsBlock*> g_fps{nullptr};
uint64_t g_previous_tick = 0;  // Heartbeat's thread only
unsigned g_tick_pos = 0;       // idem
uint64_t g_frames = 0;         // idem
std::atomic<ReverseNxBlock*> g_reverse{nullptr};
std::atomic<int> g_enabled{1};
/* Attempts so far. The sysmodule may start later than the program, so Update keeps retrying once per second. */
int g_attempts = 0;

/*
 * The last values published. If the block appears late (the overlay was opened later, SaltyNX was slow to hand out
 * the memory, or the first allocation did not fit), it is seeded right away with these values: otherwise the overlay
 * shows no FPS until the next second and no resolution until the next present. Only Init/Update touch them.
 */
uint8_t g_last_fps = 0;
float g_last_average = 0.0f;
uint16_t g_last_width = 0;
uint16_t g_last_height = 0;
uint64_t g_last_frames = 0;

/* Tick of the first attempt, for the "published after N s" message. */
uint64_t g_first_attempt_tick = 0;
/* Warnings issued only once; repeating them on every retry would fill the log. */
bool g_warned_published = false;
bool g_warned_connection = false;
bool g_warned_block = false;
bool g_warned_no_room = false;
bool g_warned_reverse = false;
bool g_warned_no_memory = false;

/*
 * --- SaltySD IPC. Modern libnx no longer ships the old ipc.h API, but the service speaks plain CMIF, so
 * serviceDispatch works. All the commands send the process ID. ---
 */

Result ReserveMemory(Service* s, uint64_t size, uint64_t* offset) {
  return serviceDispatchInOut(s, 6, size, *offset, .in_send_pid = true);
}

Result GetMemoryHandle(Service* s, Handle* out) {
  return serviceDispatch(s, 7, .in_send_pid = true, .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                         .out_handles = out);
}

Result EndSession(Service* s) {
  const uint64_t zero = 0;
  return serviceDispatchIn(s, 0, zero, .in_send_pid = true);
}

/* Looks for a magic by scanning the page 4 bytes at a time, which is how the overlay does it. */
void* FindMagicIn(uint8_t* base, size_t bytes, uint32_t magic) {
  if (!base) {
    return nullptr;
  }
  for (size_t offset = 0; offset + sizeof(uint32_t) <= bytes; offset += 4) {
    uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    if (value == magic) {
      return base + offset;
    }
  }
  return nullptr;
}

void* FindMagic(uint32_t magic) { return FindMagicIn(g_base, g_bytes, magic); }

/*
 * The block is only valid while it keeps its magic. The page is shared by several clients and its allocation is not
 * reset when the program starts, so a good pointer can go bad. Checking it is a 4-byte read, done on every use.
 */
bool IsValid(const FpsBlock* block) { return block != nullptr && block->magic == kMagicFps; }

/*
 * Writes into the block, right away, everything the overlay needs for its two rows (FPS and RES): the alive mark,
 * the API, the frame rate and the resolution. Seeding it as soon as the block exists is what makes the data appear
 * without restarting the program.
 */
void SeedBlock(FpsBlock* block) {
  block->plugin_active = true;  // the overlay sets it to false to check that we are still alive
  block->api = kApi;
  block->fps = g_last_fps;
  block->fps_average = g_last_average;
  block->frame_number = g_last_frames;
  if (g_last_width && g_last_height) {
    // `calls` cannot be 0xFFFF: that is the mark the overlay uses to ask whether we know the resolution.
    const uint16_t count = g_last_fps ? uint16_t(g_last_fps) : uint16_t(1);
    const ResolutionCalls r = {g_last_width, g_last_height, count};
    block->render[0] = r;
    block->viewport[0] = r;
  }
  // The overlay computes its average from ticks[]: with the array at zero it shows "inf". A new block has it at
  // zero, so if a rate has already been measured it is filled in here. Heartbeat replaces it with real times as soon
  // as there are two presents.
  if (g_last_fps != 0 && block->ticks[0] == 0) {
    // By index, not by reference: the block is packed and ticks[] sits at an odd offset.
    const uint32_t per_frame = uint32_t(armGetSystemTickFreq() / g_last_fps);
    for (unsigned i = 0; i < 10; ++i) {
      block->ticks[i] = per_frame;
    }
  }
}

/* A single log line when publishing succeeds, with the seconds since startup. */
void WarnPublished(const FpsBlock* block) {
  if (g_warned_published) {
    return;
  }
  g_warned_published = true;
  const uint64_t freq = armGetSystemTickFreq();
  const double seconds = freq ? double(armGetSystemTick() - g_first_attempt_tick) / double(freq) : 0.0;
  const unsigned offset =
      g_base ? unsigned(reinterpret_cast<const uint8_t*>(block) - static_cast<const uint8_t*>(g_base)) : 0u;
  std::fprintf(stderr, "[saltynx] PUBLISHED %.2f s after startup: FPS=%u and RES=%ux%u at offset 0x%X\n", seconds,
               unsigned(g_last_fps), unsigned(g_last_width), unsigned(g_last_height), offset);
}

/*
 * With the page already mapped, looks for both magics and keeps them. It is cheap (1024 comparisons on memory that
 * is already there) and uses no session, so it can be repeated every second while the block is missing.
 * Returns true if there is an FPS block.
 */
bool Attach() {
  auto* block = static_cast<FpsBlock*>(FindMagic(kMagicFps));
  if (block) {
    g_fps.store(block, std::memory_order_release);
    SeedBlock(block);
    WarnPublished(block);
  }
  if (!g_reverse.load(std::memory_order_acquire)) {
    if (auto* reverse = static_cast<ReverseNxBlock*>(FindMagic(kMagicReverseNx))) {
      g_reverse.store(reverse, std::memory_order_release);
    }
  }
  return block != nullptr;
}

/*
 * If SaltyNX was loaded into this process (it never is into a homebrew program, but it costs nothing to check), its
 * shared memory is already mapped here. The memory map is walked with svcQueryMemory.
 */
bool FindOwnSharedMemory() {
  uint64_t address = 0;
  for (int regions = 0; regions < 4096; ++regions) {
    MemoryInfo info{};
    u32 pages = 0;
    if (R_FAILED(svcQueryMemory(&info, &pages, address))) {
      return false;
    }
    if (info.size == 0) {
      return false;
    }
    if (info.type == MemType_SharedMem && (info.perm & Perm_R) != 0 && info.size >= kSharedSize) {
      auto* base = reinterpret_cast<uint8_t*>(uintptr_t(info.addr));
      const size_t bytes = size_t(info.size) < 0x10000 ? size_t(info.size) : 0x10000;
      if (FindMagicIn(base, bytes, kMagicFps) || FindMagicIn(base, bytes, kMagicReverseNx)) {
        g_base = base;
        g_bytes = bytes;
        return true;
      }
    }
    const uint64_t next = info.addr + info.size;
    if (next <= address) {
      return false;
    }
    address = next;
  }
  return false;
}

/*
 * A connection error can come from two very different things:
 *   - the single session of the SaltyNX port is busy (its limit, fixed by retrying), or
 *   - this process is out of sessions (0x10801 is "resource exhausted", and the kernel also returns it when the
 *     process cannot reserve another session).
 * They are told apart by connecting to "sm:", which always exists and accepts many sessions. If "sm:" fails too, the
 * limit is ours; the sessions in use and the process limit are printed right there.
 */
void Diagnose(Result rc_saltysd) {
  if (rc_saltysd == 0) {
    Warn("[saltynx] not even tried: the process has no room for another port session");
  } else {
    WarnNum("[saltynx] cannot connect to the SaltyNX ports. Last error", rc_saltysd);
  }

  // Our own title ID: SaltyNX rejects those above 0x01FFFFFFFFFFFFFF ("is a homebrew application"), which is the
  // range forwarders fall in.
  u64 title = 0;
  if (R_SUCCEEDED(svcGetInfo(&title, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0))) {
    std::fprintf(stderr, "[saltynx] our title ID: %016llX (SaltyNX accepts <= 01FFFFFFFFFFFFFF without 0x1F00)\n",
                 (unsigned long long)title);
  }

  // Each port separately: "does not exist" (0xF201, not installed) is not the same as "resource exhausted".
  static const char* const kBothPorts[2] = {"InjectServ", "SaltySD"};
  for (const char* p : kBothPorts) {
    Handle h = INVALID_HANDLE;
    const Result rc = svcConnectToNamedPort(&h, p);
    if (R_SUCCEEDED(rc)) {
      svcCloseHandle(h);
      Warn(p[0] == 'I' ? "[saltynx] port InjectServ: connects" : "[saltynx] port SaltySD: connects");
    } else {
      WarnNum(p[0] == 'I' ? "[saltynx] port InjectServ: error" : "[saltynx] port SaltySD: error", rc);
    }
  }

  Handle control = INVALID_HANDLE;
  const Result rc_sm = svcConnectToNamedPort(&control, "sm:");
  if (R_SUCCEEDED(rc_sm)) {
    svcCloseHandle(control);
    Warn("[saltynx] check: 'sm:' gives a new session, so the limit is the SaltyNX port's");
  } else {
    WarnNum("[saltynx] check: 'sm:' gives no session either, so the limit is OURS. Error", rc_sm);
  }

  u64 raw = 0;
  Result rc_limit = svcGetInfo(&raw, InfoType_ResourceLimit, INVALID_HANDLE, 0);
  if (R_FAILED(rc_limit)) {
    rc_limit = svcGetInfo(&raw, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0);
  }
  if (R_FAILED(rc_limit)) {
    WarnNum("[saltynx] cannot read the process limits. Error", rc_limit);
    return;
  }
  const Handle limit = static_cast<Handle>(raw);
  struct Resource {
    const char* name;
    LimitableResource which;
  };
  const Resource kResources[] = {
      {"[saltynx] sessions used / limit:", LimitableResource_Sessions},
      {"[saltynx] events used / limit:", LimitableResource_Events},
      {"[saltynx] threads used / limit:", LimitableResource_Threads},
      {"[saltynx] transfer memories used / limit:", LimitableResource_TransferMemories},
  };
  for (const Resource& r : kResources) {
    s64 used = 0;
    s64 max = 0;
    if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&used, limit, r.which)) &&
        R_SUCCEEDED(svcGetResourceLimitLimitValue(&max, limit, r.which))) {
      WarnPair(r.name, (long long)used, (long long)max);
    }
  }
  svcCloseHandle(limit);
}

/*
 * --- Making room for a session. ---------------------------------------------------------------------------------
 * Measured on the console: a forwarder (hbloader inside an installed NSP) has a resource limit of one port session,
 * and libnx already uses it for "sm:". That is why both SaltyNX ports, and also "sm:", failed with 0x10801.
 *
 * Two ways to make room, in this order:
 *   1. Raise the limit of our own process. The kernel requires no privilege for that, only that the new limit is not
 *      lower than what is in use. But the call (SVC 0x7E) may not be allowed in the process, and using a forbidden
 *      SVC closes the program, so envIsSyscallHinted is asked first.
 *   2. If that is not possible, release "sm:" for a moment (smExit) and bring it back afterwards. It is only needed
 *      once: the shared memory handle stays with us after the session is closed.
 */

Handle OpenResourceLimit() {
  u64 raw = 0;
  if (R_SUCCEEDED(svcGetInfo(&raw, InfoType_ResourceLimit, INVALID_HANDLE, 0))) {
    return static_cast<Handle>(raw);
  }
  if (R_SUCCEEDED(svcGetInfo(&raw, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) {
    return static_cast<Handle>(raw);
  }
  return INVALID_HANDLE;
}

bool RoomForOneSession(Handle limit) {
  s64 used = 0;
  s64 max = 0;
  if (R_FAILED(svcGetResourceLimitCurrentValue(&used, limit, LimitableResource_Sessions)) ||
      R_FAILED(svcGetResourceLimitLimitValue(&max, limit, LimitableResource_Sessions))) {
    return true;  // if it cannot be read, try anyway
  }
  return used < max;
}

/*
 * Sets *sm_released to true if "sm:" had to be released (it has to be brought back later). Releasing "sm:" is not
 * tried on every attempt: it is a few milliseconds without the service manager. Raising the limit, on the other
 * hand, is permanent and done only once.
 */
bool MakeRoom(bool* sm_released, bool allow_release_sm) {
  *sm_released = false;
  const Handle limit = OpenResourceLimit();
  if (limit == INVALID_HANDLE) {
    return true;
  }
  bool room = RoomForOneSession(limit);

  if (!room) {
    if (envIsSyscallHinted(0x7E)) {  // svcSetResourceLimitLimitValue
      s64 max = 0;
      svcGetResourceLimitLimitValue(&max, limit, LimitableResource_Sessions);
      const Result rc = svcSetResourceLimitLimitValue(limit, LimitableResource_Sessions, static_cast<u64>(max + 4));
      if (R_SUCCEEDED(rc)) {
        room = RoomForOneSession(limit);
        WarnPair("[saltynx] raised the process session limit:", (long long)max, (long long)(max + 4));
      } else {
        WarnNum("[saltynx] cannot raise the session limit. Error", rc);
      }
    } else {
      Warn("[saltynx] the loader does not allow svcSetResourceLimitLimitValue (SVC 0x7E)");
    }
  }

  if (!room && allow_release_sm) {
    smExit();  // libnx counts references; if it really closes, there is room
    if (RoomForOneSession(limit)) {
      *sm_released = true;
      room = true;
      Warn("[saltynx] released 'sm:' for a moment to make room");
    } else {
      smInitialize();  // it did not close: restore the count and leave it as it was
      Warn("[saltynx] no room for a session even after releasing 'sm:'");
    }
  }

  svcCloseHandle(limit);
  return room;
}

/* Brings "sm:" back on exit, whatever happens. */
struct RestoreSm {
  bool active = false;
  ~RestoreSm() {
    if (active) {
      smInitialize();
    }
  }
};

}  // namespace

void Init() {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (g_first_attempt_tick == 0) {
    g_first_attempt_tick = armGetSystemTick();
  }

  /*
   * The goal is not "the page is mapped" but "we have a good block". If mapping succeeded but there was no room for
   * the block, or another client rewrote the page, this is tried again.
   */
  FpsBlock* current = g_fps.load(std::memory_order_acquire);
  if (!IsValid(current)) {
    g_fps.store(nullptr, std::memory_order_release);
  } else if (g_reverse.load(std::memory_order_acquire) != nullptr) {
    return;  // both in place: nothing to do
  }

  // The cheap part first: if the page is already mapped, it is enough to search for the magics again.
  if (g_mapped && Attach()) {
    return;
  }

  // Path 1: IPC. SaltyNX creates two ports and each accepts a single session.
  // First there has to be room: a forwarder only allows one port session and libnx uses it for "sm:".
  RestoreSm restore;
  // Raising the limit is tried from the start. Releasing "sm:" only from the third attempt on, when the program has
  // opened its services, then until attempt 15, and after that once per minute.
  const bool release_sm = g_attempts >= 3 && (g_attempts < 15 || (g_attempts % 60) == 0);
  const bool has_room = MakeRoom(&restore.active, release_sm);
  // With "sm:" released it has to be quick: a single pass. The first attempt insists (the sysmodule may be starting
  // up); later ones try twice, since the overlay also needs the port's only session.
  const int passes = restore.active ? 1 : (g_attempts == 0 ? 20 : 2);
  bool done = false;

  /*
   * "SaltySD" first: it is the port that serves commands 6 and 7. "InjectServ" accepts the connection and then closes
   * it (0xF601). If a port connects but does not give the memory, it is closed and the next one is tried in the same
   * pass.
   */
  static const char* const kPorts[2] = {"SaltySD", "InjectServ"};

  uint64_t offset = 0;
  bool reserved = false;
  uint64_t offset_nx = 0;
  bool reserved_nx = false;
  Result rc_port = 0;
  Result rc_handle = 0;
  const char* name = nullptr;

  for (int i = 0; i < passes && !done && has_room; ++i) {
    for (const char* candidate : kPorts) {
      Handle port = INVALID_HANDLE;
      rc_port = svcConnectToNamedPort(&port, candidate);
      if (R_FAILED(rc_port)) {
        continue;
      }
      Service service{};
      service.session = port;

      // Map first, and only then reserve what is missing. SaltySD hands out the page with a counter that only goes
      // back to zero when it loads into a game, which it never does with a homebrew program: reserving on every
      // launch would use up the 4 KB in about twenty launches. Later launches reuse the same block.
      bool have_page = g_mapped;
      if (!have_page) {
        Handle memory = INVALID_HANDLE;
        rc_handle = GetMemoryHandle(&service, &memory);
        if (R_SUCCEEDED(rc_handle)) {
          shmemLoadRemote(&g_memory, memory, kSharedSize, Perm_Rw);
          if (R_SUCCEEDED(shmemMap(&g_memory))) {
            g_base = static_cast<uint8_t*>(shmemGetAddr(&g_memory));
            g_bytes = kSharedSize;
            g_mapped = true;
            have_page = true;
          }
        }
      }
      if (have_page) {
        done = true;
        name = candidate;
        if (!FindMagic(kMagicFps)) {
          reserved = R_SUCCEEDED(ReserveMemory(&service, sizeof(FpsBlock), &offset));
        }
        if (!FindMagic(kMagicReverseNx)) {
          reserved_nx = R_SUCCEEDED(ReserveMemory(&service, sizeof(ReverseNxBlock), &offset_nx));
        }
      }

      EndSession(&service);  // command 0: the server closes its side
      // And our end has to be closed too. The port accepts a single session, so leaving the handle open would keep
      // the overlay from ever connecting again. The shared memory handle is ours and does not depend on the session.
      svcCloseHandle(port);
      if (done) {
        break;
      }
    }
    if (!done) {
      svcSleepThread(10 * 1000 * 1000);  // 10 ms
    }
  }

  if (restore.active) {  // room again: bring "sm:" back right away
    smInitialize();
    restore.active = false;
  }

  if (name && !g_warned_connection) {
    g_warned_connection = true;
    Warn(name[0] == 'S' ? "[saltynx] connected through SaltySD" : "[saltynx] connected through InjectServ");
  }
  if (!g_mapped) {
    if (R_FAILED(rc_handle) && !g_warned_no_memory) {
      g_warned_no_memory = true;
      WarnNum("[saltynx] a port connects but does not give the shared memory; error", rc_handle);
    }
    // Path 2: no free session. If SaltyNX was loaded into this process, its shared memory is already mapped here.
    if (!FindOwnSharedMemory()) {
      ++g_attempts;
      // The first diagnosis waits until attempt 5, after releasing "sm:" has been tried.
      if (g_attempts == 5 || g_attempts == 30) {
        Diagnose(rc_port);
      } else if (g_attempts % 600 == 0) {
        WarnNum("[saltynx] still retrying without success. Error", rc_port);
      }
      return;
    }
    g_mapped = true;
    Warn("[saltynx] no session on the ports, but their shared memory was already mapped here");
  }

  // The FPS block: if there is one already, it is reused; otherwise the slot reserved through IPC is used.
  auto* existing = static_cast<FpsBlock*>(FindMagic(kMagicFps));
  if (existing) {
    g_fps.store(existing, std::memory_order_release);
    if (!g_warned_block) {
      g_warned_block = true;
      Warn("[saltynx] FPS block already present: reusing it");
    }
  } else if (reserved) {
    auto* block = reinterpret_cast<FpsBlock*>(g_base + offset);
    std::memset(block, 0, sizeof(*block));
    block->magic = kMagicFps;
    g_fps.store(block, std::memory_order_release);
    WarnNum("[saltynx] FPS block created at offset", (long long)offset);
  } else if (!g_warned_no_room) {
    g_warned_no_room = true;
    Warn("[saltynx] no room for the FPS block; retrying once per second");
  }
  if (FpsBlock* block = g_fps.load(std::memory_order_acquire)) {
    SeedBlock(block);
    WarnPublished(block);
    g_attempts = 0;
  } else {
    // Mapped but without a block: not a success. The counter keeps going up so the next call asks for room again.
    ++g_attempts;
  }

  /*
   * ReverseNX-RT. Its block is created by the plugin SaltyNX loads into a game, so with a homebrew program its
   * overlay says "ReverseNX-RT is not running!". Creating it here makes its overlay work. The overlay only looks for
   * the "NXRT" magic, but it shows its controls only when `plugin_active` is set, which means "the game has asked for
   * the mode": IsDocked sets it.
   */
  auto* reverse = static_cast<ReverseNxBlock*>(FindMagic(kMagicReverseNx));
  if (!reverse && reserved_nx) {
    reverse = reinterpret_cast<ReverseNxBlock*>(g_base + offset_nx);
    std::memset(reverse, 0, sizeof(*reverse));
    reverse->magic = kMagicReverseNx;
    reverse->controlled_by_system = true;  // the system decides until the player says otherwise
    WarnNum("[saltynx] ReverseNX-RT block created at offset", (long long)offset_nx);
  } else if (!g_warned_reverse) {
    g_warned_reverse = true;
    Warn(reverse ? "[saltynx] ReverseNX-RT block already present" : "[saltynx] no room for the ReverseNX-RT block");
  }
  // Only stored if there is one, so a retry never writes nullptr over a good pointer.
  if (reverse) {
    g_reverse.store(reverse, std::memory_order_release);
  }
}

void Update(double fps_last_second, double fps_average, uint32_t width, uint32_t height, uint64_t frames) {
  // Remembered before publishing: if the block appears later, it is seeded with these values the moment it exists.
  const double clamped = fps_last_second < 0.0 ? 0.0 : (fps_last_second > 255.0 ? 255.0 : fps_last_second);
  g_last_fps = uint8_t(clamped + 0.5);
  g_last_average = float(fps_average);
  if (width && height) {
    g_last_width = uint16_t(width);
    g_last_height = uint16_t(height);
  }
  g_last_frames = frames;

  FpsBlock* block = g_fps.load(std::memory_order_acquire);
  if (!g_enabled.load(std::memory_order_relaxed)) {
    // Turned off: the block is released and Heartbeat stops writing too. Turning it on again recovers it.
    if (block) {
      block->plugin_active = false;
      g_fps.store(nullptr, std::memory_order_release);
    }
    return;
  }
  // The magic is checked on every call and, if it is missing, the block is found again. This is what lets the
  // overlay be opened at any time without restarting the program.
  if (!IsValid(block)) {
    Init();
    block = g_fps.load(std::memory_order_acquire);
  }
  if (block) {
    // FPS and resolution, always: Heartbeat only runs while frames are presented, and during loading the overlay
    // would be left without the RES row.
    SeedBlock(block);
  }
}

/*
 * The heartbeat, once per presented frame.
 *
 * The overlay does not believe the program is alive until it answers two short questions (Status-Monitor-Overlay,
 * source/Utils.hpp and source/modes/Resolutions.hpp):
 *
 *   NxFps->pluginActive = false;  svcSleepThread(100'000'000);  if (NxFps->pluginActive) GameRunning = true;
 *   NxFps->renderCalls[0].calls = 0xFFFF;  ... if (renderCalls[0].calls != 0xFFFF) resolutionLookup = 2;
 *
 * With one update per second the overlay says "Game is not running or it's incompatible". It also computes the FPS
 * average from FPSticks[10] (tick frequency / average of the ticks), so with the array at zero it shows "inf". Here
 * it is filled with the real time between presents.
 */
void Heartbeat(uint32_t width, uint32_t height) {
  FpsBlock* block = g_fps.load(std::memory_order_acquire);
  // If the magic is gone, the pointer is not valid: it is dropped and Update recovers it. No IPC and no searching
  // here, since this runs on every frame.
  if (!IsValid(block)) {
    if (block) {
      g_fps.store(nullptr, std::memory_order_release);
    }
    return;
  }
  block->plugin_active = true;
  block->api = kApi;

  // If the block is a different one (just found because the overlay was opened now), the previous time is minutes
  // old: that first measurement is discarded, or the overlay would show one very long frame in its average.
  static const FpsBlock* last_seen = nullptr;  // Heartbeat's thread only
  if (last_seen != block) {
    last_seen = block;
    g_previous_tick = 0;
  }

  const uint64_t now = armGetSystemTick();
  if (g_previous_tick != 0) {
    const uint64_t delta = now - g_previous_tick;
    block->ticks[g_tick_pos] = uint32_t(delta > 0xFFFFFFFFull ? 0xFFFFFFFFull : delta);
    g_tick_pos = (g_tick_pos + 1) % 10;
    ++g_frames;
    block->frame_number = g_frames;
  }
  g_previous_tick = now;

  if (width && height) {
    // `calls` cannot be 0xFFFF: that is the mark the overlay asks with. It is set to the frames of the last second,
    // which is what NX-FPS counts.
    const uint16_t count = block->fps ? block->fps : uint16_t(1);
    const ResolutionCalls r = {uint16_t(width), uint16_t(height), count};
    block->render[0] = r;
    block->viewport[0] = r;
  }
}

void SetEnabled(bool enabled) { g_enabled.store(enabled ? 1 : 0, std::memory_order_relaxed); }

bool IsDocked(bool real) {
  ReverseNxBlock* reverse = g_reverse.load(std::memory_order_acquire);
  if (!reverse) {
    return real;
  }
  // In ReverseNX-RT, `plugin_active` means "the game has asked for the mode"; without it its overlay says "Game
  // didn't check any mode!" and does not show its controls.
  reverse->plugin_active = true;
  if (reverse->controlled_by_system) {
    reverse->docked = real;  // the system decides: mirror it so the overlay shows the real mode
    return real;
  }
  return reverse->docked;
}

ReverseNxState GetReverseNxState() {
  const ReverseNxBlock* reverse = g_reverse.load(std::memory_order_acquire);
  if (!reverse) {
    return {false, false, false, false};
  }
  return {true, reverse->docked, reverse->controlled_by_system, reverse->plugin_active};
}

}  // namespace saltynx

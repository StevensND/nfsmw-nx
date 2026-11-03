/**
 * @file        input/switch/switch_input_driver.cpp
 * @brief       Input driver for Nintendo Switch pads through libnx HID
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/switch/switch_input_driver.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

#include <switch.h>

// Face buttons (1.0.1). By default each Switch button acts as the Xbox 360 button with the same
// letter: A accepts and B goes back, as in other Switch games. With true they go by position, as on
// an Xbox pad: the bottom one (B on the Switch) acts as A, the mapping of 1.0.0. It applies at once;
// the settings menu reads it too (window_switch.cpp).
REXCVAR_DEFINE_BOOL(input_xbox_layout, false, "Input",
                    "Face buttons by position, as on an Xbox pad (the bottom button, B, acts as A). "
                    "false = by letter: A accepts and B goes back");

/*
 * Left stick response, for steering. The game ignores the first part of the stick's travel (its
 * own dead zone, assumed to be XInput's usual 24 %, see nfsmw_prueba_piloto_zona_muerta) and a
 * Switch stick travels less than an Xbox 360 one, so the range left for small corrections is very
 * short. With an anti dead zone, the smallest push past input_left_stick_deadzone already reaches
 * the edge of the game's dead zone and the rest of the travel is spread over what is left.
 *
 * Each axis on its own, not the stick's length: the game steers with X only, and a round shape
 * would shrink X whenever the stick is also a little up or down. With every value at its default
 * the stick goes to the game untouched, as before. They apply at once, also from the Debug Menu.
 */
REXCVAR_DEFINE_DOUBLE(input_left_stick_deadzone, 0.0, "Input",
                      "Left stick: fraction of the travel ignored around the centre, so a resting stick "
                      "sends 0. Needed with an anti dead zone (0.05-0.08); 0 = none")
    .range(0.0, 0.5);
REXCVAR_DEFINE_DOUBLE(input_left_stick_anti_deadzone, 0.0, "Input",
                      "Left stick: output where the travel starts, past the dead zone. Set it near the "
                      "game's own dead zone (about 0.2-0.24) so small corrections steer; 0 = none")
    .range(0.0, 0.9);
REXCVAR_DEFINE_DOUBLE(input_left_stick_outer_deadzone, 0.0, "Input",
                      "Left stick: fraction of the travel at the edge that already counts as fully "
                      "pushed, for sticks that never report the full range; 0 = none")
    .range(0.0, 0.5);
REXCVAR_DEFINE_DOUBLE(input_left_stick_curve, 1.0, "Input",
                      "Left stick: response curve over the travel. 1 = linear, above 1 = finer near the "
                      "centre, below 1 = quicker near the centre")
    .range(0.2, 4.0);

namespace rex::ui {
// rex/ui/overlay/debug_overlay.h (not included here because it pulls in ImGui).
bool DebugOverlayAbierto();
}  // namespace rex::ui

namespace rex::input::nx {

namespace {

constexpr int16_t kThumbThreshold = 0x4E00;
constexpr uint8_t kTriggerThreshold = 0x1F;
constexpr uint32_t kRepeatDelayMs = 400;
constexpr uint32_t kRepeatRateMs = 100;

// Low byte of a DeviceId is the slot, the upper half a per-run connection
// counter, so a pad that reconnects never gets an id it had before.
constexpr uint64_t kDeviceIdTag = 0x4E5800;  // "NX"

struct ButtonMapping {
  uint64_t npad;
  uint16_t xinput;
};

// Face buttons by letter (the default): the Switch's A is the Xbox A.
constexpr std::array<ButtonMapping, 4> kFrontalesPorLetra = {{
    {HidNpadButton_A, X_INPUT_GAMEPAD_A},
    {HidNpadButton_B, X_INPUT_GAMEPAD_B},
    {HidNpadButton_X, X_INPUT_GAMEPAD_X},
    {HidNpadButton_Y, X_INPUT_GAMEPAD_Y},
}};

// By position (input_xbox_layout), as on an Xbox pad: the bottom one is A.
// On Switch pads that button is labelled B.
constexpr std::array<ButtonMapping, 4> kFrontalesPorPosicion = {{
    {HidNpadButton_B, X_INPUT_GAMEPAD_A},
    {HidNpadButton_A, X_INPUT_GAMEPAD_B},
    {HidNpadButton_Y, X_INPUT_GAMEPAD_X},
    {HidNpadButton_X, X_INPUT_GAMEPAD_Y},
}};

// The other buttons are the same with both layouts.
constexpr std::array<ButtonMapping, 10> kButtonMappings = {{
    {HidNpadButton_Up, X_INPUT_GAMEPAD_DPAD_UP},
    {HidNpadButton_Down, X_INPUT_GAMEPAD_DPAD_DOWN},
    {HidNpadButton_Left, X_INPUT_GAMEPAD_DPAD_LEFT},
    {HidNpadButton_Right, X_INPUT_GAMEPAD_DPAD_RIGHT},
    {HidNpadButton_Plus, X_INPUT_GAMEPAD_START},
    {HidNpadButton_Minus, X_INPUT_GAMEPAD_BACK},
    {HidNpadButton_L, X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {HidNpadButton_R, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {HidNpadButton_StickL, X_INPUT_GAMEPAD_LEFT_THUMB},
    {HidNpadButton_StickR, X_INPUT_GAMEPAD_RIGHT_THUMB},
}};

// The order of this list is also the order in which events are sent if
// multiple buttons change at once. Same table as the SDL driver.
constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
    // 00 - True buttons from xinput button field
    rex::ui::VirtualKey::kXInputPadDpadUp,
    rex::ui::VirtualKey::kXInputPadDpadDown,
    rex::ui::VirtualKey::kXInputPadDpadLeft,
    rex::ui::VirtualKey::kXInputPadDpadRight,
    rex::ui::VirtualKey::kXInputPadStart,
    rex::ui::VirtualKey::kXInputPadBack,
    rex::ui::VirtualKey::kXInputPadLThumbPress,
    rex::ui::VirtualKey::kXInputPadRThumbPress,
    rex::ui::VirtualKey::kXInputPadLShoulder,
    rex::ui::VirtualKey::kXInputPadRShoulder,
    rex::ui::VirtualKey::kNone, /* Guide has no VK */
    rex::ui::VirtualKey::kNone, /* Unknown */
    rex::ui::VirtualKey::kXInputPadA,
    rex::ui::VirtualKey::kXInputPadB,
    rex::ui::VirtualKey::kXInputPadX,
    rex::ui::VirtualKey::kXInputPadY,
    // 16 - Fake buttons generated from analog inputs
    rex::ui::VirtualKey::kXInputPadLTrigger,
    rex::ui::VirtualKey::kXInputPadRTrigger,
    // 18
    rex::ui::VirtualKey::kXInputPadLThumbUp,
    rex::ui::VirtualKey::kXInputPadLThumbDown,
    rex::ui::VirtualKey::kXInputPadLThumbRight,
    rex::ui::VirtualKey::kXInputPadLThumbLeft,
    rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
    rex::ui::VirtualKey::kXInputPadLThumbUpRight,
    rex::ui::VirtualKey::kXInputPadLThumbDownRight,
    rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
    // 26
    rex::ui::VirtualKey::kXInputPadRThumbUp,
    rex::ui::VirtualKey::kXInputPadRThumbDown,
    rex::ui::VirtualKey::kXInputPadRThumbRight,
    rex::ui::VirtualKey::kXInputPadRThumbLeft,
    rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
    rex::ui::VirtualKey::kXInputPadRThumbUpRight,
    rex::ui::VirtualKey::kXInputPadRThumbDownRight,
    rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
};

enum class RepeatState {
  kIdle,       // no buttons pressed or repeating has ended
  kWaiting,    // a button is held and the delay is awaited
  kRepeating,  // actively repeating at a rate
};

int16_t ClampStickAxis(s32 value) {
  return static_cast<int16_t>(std::clamp<s32>(value, INT16_MIN, INT16_MAX));
}

struct StickResponse {
  double deadzone;
  double anti_deadzone;
  double outer_deadzone;
  double curve;
};

StickResponse LeftStickResponse() {
  return {REXCVAR_GET(input_left_stick_deadzone), REXCVAR_GET(input_left_stick_anti_deadzone),
          REXCVAR_GET(input_left_stick_outer_deadzone), REXCVAR_GET(input_left_stick_curve)};
}

bool IsIdentity(const StickResponse& r) {
  return r.deadzone <= 0.0 && r.anti_deadzone <= 0.0 && r.outer_deadzone <= 0.0 && r.curve == 1.0;
}

// One axis through the left stick response (see the input_left_stick_* cvars): dead zone, then the
// travel stretched from the anti dead zone to full, shaped by the curve.
int16_t ShapeStickAxis(int16_t value, const StickResponse& r) {
  const double magnitude = std::abs(static_cast<double>(value)) / 32767.0;
  if (magnitude <= r.deadzone) {
    return 0;
  }
  const double span = std::max(1.0 - r.deadzone - r.outer_deadzone, 0.01);
  const double travel = std::pow(std::min((magnitude - r.deadzone) / span, 1.0), r.curve);
  const double output = std::min(r.anti_deadzone + (1.0 - r.anti_deadzone) * travel, 1.0);
  const auto shaped = static_cast<int16_t>(std::lround(output * 32767.0));
  return value < 0 ? static_cast<int16_t>(-shaped) : shaped;
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > kTriggerThreshold) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > kTriggerThreshold) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > kThumbThreshold;
    uint64_t d = thumb_y < ~kThumbThreshold;
    uint64_t r = thumb_x > kThumbThreshold;
    uint64_t l = thumb_x < ~kThumbThreshold;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace

struct SwitchInputDriver::Slot {
  HidNpadIdType npad_id = HidNpadIdType_No1;
  PadState pad{};
  bool connected = false;
  DeviceId id = DeviceId::kInvalid;

  X_INPUT_STATE state{};
  bool state_changed = false;
  bool is_active = true;

  /*
   * One bit per menu shortcut, so it fires only on press and not while held.
   * See DispatchMenuShortcuts.
   */
  uint64_t menu_shortcuts = 0;

  // Keystroke synthesis, per pad rather than per guest user.
  uint64_t key_buttons = 0;
  RepeatState repeat_state = RepeatState::kIdle;
  uint8_t repeat_index = 0;
  uint32_t repeat_time = 0;

  // Vibration devices are initialized for one controller layout; redone only
  // when the attached layout changes.
  HidNpadIdType vibration_npad_id = HidNpadIdType_No1;
  uint32_t vibration_style = 0;
  int vibration_count = 0;
  std::array<HidVibrationDeviceHandle, 2> vibration_handles{};
  // The last value sent to the controller. Stack sampling put 5.8 % of the thread that prepares
  // each frame inside SetState: the game calls XInputSetState very often, and every call ended
  // in hidSendVibrationValues, an IPC request that blocks the thread. With vibration off
  // (input_vibracion = false) it kept sending "zero" over and over. If the requested value is
  // the same as the controller already has, nothing is sent: the controller is already in that
  // state.
  bool vibration_sent = false;
  uint16_t vibration_left = 0;
  uint16_t vibration_right = 0;
};

SwitchInputDriver::SwitchInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

SwitchInputDriver::~SwitchInputDriver() {
  if (!initialized_) {
    return;
  }
  // XInput vibration holds until changed, so a pad left rumbling would keep
  // rumbling after the process exits.
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& slot : slots_) {
    if (slot && slot->vibration_count) {
      std::array<HidVibrationValue, 2> stop{};
      for (auto& value : stop) {
        value.freq_low = 160.0f;
        value.freq_high = 320.0f;
      }
      hidSendVibrationValues(slot->vibration_handles.data(), stop.data(), slot->vibration_count);
    }
  }
}

X_STATUS SwitchInputDriver::Setup() {
  // Reference counted in libnx. MarathonRecomp-NX found that without it the
  // first padUpdate can abort with 2345-0008 when HID was not brought up by
  // __appInit.
  Result rc = hidInitialize();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchInputDriver: hidInitialize failed: 0x{:08X}", rc);
    return X_STATUS_UNSUCCESSFUL;
  }
  padConfigureInput(kSlotCount, HidNpadStyleSet_NpadStandard);

  static constexpr std::array<HidNpadIdType, kSlotCount> kNpadIds = {
      HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4};
  for (size_t i = 0; i < kSlotCount; ++i) {
    auto slot = std::make_unique<Slot>();
    slot->npad_id = kNpadIds[i];
    if (i == 0) {
      padInitialize(&slot->pad, HidNpadIdType_No1, HidNpadIdType_Handheld);
    } else {
      padInitialize(&slot->pad, kNpadIds[i]);
    }
    slots_[i] = std::move(slot);
  }
  hidPermitVibration(true);
  initialized_ = true;
  return X_STATUS_SUCCESS;
}

void SwitchInputDriver::Poll(size_t index) {
  Slot& slot = *slots_[index];
  padUpdate(&slot.pad);

  const bool connected = padIsConnected(&slot.pad);
  if (connected != slot.connected) {
    slot.connected = connected;
    if (connected) {
      slot.id = DeviceId((uint64_t(++connection_counter_) << 32) | (kDeviceIdTag << 8) | index);
      slot.state = {};
      slot.state_changed = true;
      slot.key_buttons = 0;
      slot.repeat_state = RepeatState::kIdle;
      slot.vibration_count = 0;
    } else {
      slot.id = DeviceId::kInvalid;
    }
  }
  if (!connected) {
    return;
  }

  const uint64_t held = padGetButtons(&slot.pad);
  DispatchMenuShortcuts(slot, held);
  X_INPUT_GAMEPAD gamepad{};
  uint16_t buttons = 0;
  const auto& frontales =
      REXCVAR_GET(input_xbox_layout) ? kFrontalesPorPosicion : kFrontalesPorLetra;
  for (const ButtonMapping& mapping : frontales) {
    if (held & mapping.npad) {
      buttons |= mapping.xinput;
    }
  }
  for (const ButtonMapping& mapping : kButtonMappings) {
    if (held & mapping.npad) {
      buttons |= mapping.xinput;
    }
  }
  gamepad.buttons = buttons;
  // ZL and ZR are digital.
  gamepad.left_trigger = (held & HidNpadButton_ZL) ? 0xFF : 0;
  gamepad.right_trigger = (held & HidNpadButton_ZR) ? 0xFF : 0;
  // HID reports up as positive Y, like XInput, so no inversion is needed.
  const HidAnalogStickState left = padGetStickPos(&slot.pad, 0);
  const HidAnalogStickState right = padGetStickPos(&slot.pad, 1);
  int16_t left_x = ClampStickAxis(left.x);
  int16_t left_y = ClampStickAxis(left.y);
  if (const StickResponse response = LeftStickResponse(); !IsIdentity(response)) {
    left_x = ShapeStickAxis(left_x, response);
    left_y = ShapeStickAxis(left_y, response);
  }
  gamepad.thumb_lx = left_x;
  gamepad.thumb_ly = left_y;
  gamepad.thumb_rx = ClampStickAxis(right.x);
  gamepad.thumb_ry = ClampStickAxis(right.y);
  /*
   * With the debug overlay open, L+R and the right stick move it (debug_overlay.cpp,
   * on the UI thread). While L and R are held, the game sees neither those two
   * buttons nor the right stick.
   */
  constexpr uint64_t kMoverOverlay = HidNpadButton_L | HidNpadButton_R;
  if ((held & kMoverOverlay) == kMoverOverlay && rex::ui::DebugOverlayAbierto()) {
    // The XInput state fields are stored big-endian: they are rewritten whole.
    gamepad.buttons = static_cast<uint16_t>(
        buttons & ~(X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER));
    gamepad.thumb_rx = static_cast<int16_t>(0);
    gamepad.thumb_ry = static_cast<int16_t>(0);
  }

  if (std::memcmp(&gamepad, &slot.state.gamepad, sizeof(gamepad)) != 0) {
    slot.state.gamepad = gamepad;
    slot.state_changed = true;
  }
}


/* From the profiler (switch_perf.cpp): turns the GPU A/B test lap on and off. */
extern "C" void RexSwitchPerfToggleAb(void);
/*
 * The SDK menus, on the controller
 *
 * On the PC they are function keys (see RegisterBind in rex_app.cpp): F3 debug,
 * F4 settings, F7 achievements and the tilde key for the log console. The Switch
 * has no keyboard, so they go to combinations with L+R, which no game uses
 * together with the D-pad:
 *
 *   L + R + Up    -> debug overlay (counters, statistics)
 *   L + R + Right -> settings
 *   L + R + Down  -> log console
 *   L + R + Left  -> achievements
 *   L + R + ZL    -> GPU A/B test lap (for measuring, not for playing)
 *   L + R + right stick -> moves the debug overlay if it is open
 *                          (its position is saved; see Poll and debug_overlay.cpp)
 *
 * It fires once per press, not while held: each shortcut stores its bit in
 * slot.menu_shortcuts with the state from the previous poll.
 */
void SwitchInputDriver::DispatchMenuShortcuts(Slot& slot, uint64_t held) {
  struct Atajo {
    uint64_t boton;
    rex::ui::VirtualKey tecla;
  };
  constexpr uint64_t kModificador = HidNpadButton_L | HidNpadButton_R;
  static const Atajo kAtajos[] = {
      {HidNpadButton_Up, rex::ui::VirtualKey::kF3},
      {HidNpadButton_Right, rex::ui::VirtualKey::kF4},
      {HidNpadButton_Down, rex::ui::VirtualKey::kOem3},
      {HidNpadButton_Left, rex::ui::VirtualKey::kF7},
  };

  const bool modificador = (held & kModificador) == kModificador;
  for (size_t i = 0; i < std::size(kAtajos); ++i) {
    const uint64_t bit = uint64_t(1) << i;
    const bool ahora = modificador && (held & kAtajos[i].boton) != 0;
    const bool antes = (slot.menu_shortcuts & bit) != 0;
    if (ahora && !antes) {
      /*
       * Each shortcut creates or destroys an ImGui menu, and the UI thread
       * draws them continuously while any is open. From this game thread a
       * menu could be deleted in the middle of its Draw, so it is done on the
       * UI thread.
       */
      const rex::ui::VirtualKey vk = kAtajos[i].tecla;
      auto pulsar = [vk] {
        rex::ui::KeyEvent tecla(nullptr, vk, 1, false, false, false, false, false);
        rex::ui::ProcessKeyEvent(tecla);
      };
      rex::ui::Window* const ventana = ventana_ui_.load(std::memory_order_acquire);
      if (!ventana || !ventana->app_context().CallInUIThread(pulsar)) {
        pulsar();
      }
      REXLOG_INFO("atajo de menu: {}", rex::ui::VirtualKeyToString(vk));
    }
    slot.menu_shortcuts = ahora ? (slot.menu_shortcuts | bit) : (slot.menu_shortcuts & ~bit);
  }

  /*
   * L+R+ZL starts the A/B test lap (switch_perf.cpp). It sends no key to the
   * game: it talks directly to the profiler. It is kept apart from the loop
   * above because that one translates buttons into keys and this is not a key.
   */
  const uint64_t kBitAb = uint64_t(1) << 8;
  const bool ab_ahora = modificador && (held & HidNpadButton_ZL) != 0;
  const bool ab_antes = (slot.menu_shortcuts & kBitAb) != 0;
  if (ab_ahora && !ab_antes) {
    RexSwitchPerfToggleAb();
    REXLOG_INFO("atajo de menu: pruebas A/B de GPU");
  }
  slot.menu_shortcuts = ab_ahora ? (slot.menu_shortcuts | kBitAb)
                                 : (slot.menu_shortcuts & ~kBitAb);
}

SwitchInputDriver::Slot* SwitchInputDriver::FindSlot(DeviceId id) {
  const size_t index = static_cast<uint64_t>(id) & 0xFF;
  if (id == DeviceId::kInvalid || index >= kSlotCount) {
    return nullptr;
  }
  Poll(index);
  Slot* slot = slots_[index].get();
  return slot->connected && slot->id == id ? slot : nullptr;
}

void SwitchInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  if (!initialized_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < kSlotCount; ++i) {
    Poll(i);
    const Slot& slot = *slots_[i];
    if (!slot.connected) {
      continue;
    }
    DeviceInfo info;
    info.id = slot.id;
    const uint32_t style = padGetStyleSet(&slot.pad);
    if (i == 0 && slot.pad.active_handheld) {
      info.name = "Nintendo Switch (handheld)";
    } else if (style & HidNpadStyleTag_NpadFullKey) {
      info.name = "Pro Controller";
    } else if (style & HidNpadStyleTag_NpadJoyDual) {
      info.name = "Joy-Con (L/R)";
    } else if (style & HidNpadStyleTag_NpadJoyLeft) {
      info.name = "Joy-Con (L)";
    } else if (style & HidNpadStyleTag_NpadJoyRight) {
      info.name = "Joy-Con (R)";
    } else {
      info.name = "Nintendo Switch controller";
    }
    info.guid = "switch-npad-" + std::to_string(i + 1);
    info.synthetic = false;
    out.push_back(std::move(info));
  }
}

X_RESULT SwitchInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  const bool is_active = this->is_active();
  if (is_active != slot->is_active || (is_active && slot->state_changed)) {
    slot->state.packet_number = uint32_t(slot->state.packet_number) + 1;
    slot->is_active = is_active;
    slot->state_changed = false;
  }
  std::memcpy(out_state, &slot->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SwitchInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  const bool handheld = slot->npad_id == HidNpadIdType_No1 && slot->pad.active_handheld;
  out_caps->flags = handheld ? 0 : X_INPUT_CAPS_WIRELESS;
  out_caps->gamepad.buttons = 0xF3FF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  out_caps->vibration.left_motor_speed = 0xFFFFu;
  out_caps->vibration.right_motor_speed = 0xFFFFu;
  return X_ERROR_SUCCESS;
}

X_RESULT SwitchInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  HidNpadIdType npad_id = slot->npad_id;
  uint32_t style = 0;
  int count = 0;
  if (slot->npad_id == HidNpadIdType_No1 && slot->pad.active_handheld) {
    npad_id = HidNpadIdType_Handheld;
    style = HidNpadStyleTag_NpadHandheld;
    count = 2;
  } else {
    const uint32_t attached = hidGetNpadStyleSet(npad_id);
    if (attached & HidNpadStyleTag_NpadFullKey) {
      style = HidNpadStyleTag_NpadFullKey;
      count = 2;
    } else if (attached & HidNpadStyleTag_NpadJoyDual) {
      style = HidNpadStyleTag_NpadJoyDual;
      count = 2;
    } else if (attached & HidNpadStyleTag_NpadJoyLeft) {
      style = HidNpadStyleTag_NpadJoyLeft;
      count = 1;
    } else if (attached & HidNpadStyleTag_NpadJoyRight) {
      style = HidNpadStyleTag_NpadJoyRight;
      count = 1;
    } else {
      // Nothing that can rumble.
      return X_ERROR_SUCCESS;
    }
  }

  if (!slot->vibration_count || slot->vibration_npad_id != npad_id ||
      slot->vibration_style != style) {
    Result rc = hidInitializeVibrationDevices(slot->vibration_handles.data(), count, npad_id,
                                              static_cast<HidNpadStyleTag>(style));
    if (R_FAILED(rc)) {
      slot->vibration_count = 0;
      return X_ERROR_FUNCTION_FAILED;
    }
    slot->vibration_npad_id = npad_id;
    slot->vibration_style = style;
    slot->vibration_count = count;
    slot->vibration_sent = false;  // new or reassigned controller: it needs the state sent
  }

  // See the vibration_sent comment in Slot: same value as the last one sent, nothing to do.
  const uint16_t izquierdo = uint16_t(vibration->left_motor_speed);
  const uint16_t derecho = uint16_t(vibration->right_motor_speed);
  if (slot->vibration_sent && slot->vibration_left == izquierdo &&
      slot->vibration_right == derecho) {
    return X_ERROR_SUCCESS;
  }

  // XInput's left motor is the heavy low-frequency one, the right motor the
  // light high-frequency one.
  HidVibrationValue value{};
  value.amp_low = float(uint16_t(vibration->left_motor_speed)) / 65535.0f;
  value.freq_low = 160.0f;
  value.amp_high = float(uint16_t(vibration->right_motor_speed)) / 65535.0f;
  value.freq_high = 320.0f;
  std::array<HidVibrationValue, 2> values = {value, value};
  if (R_FAILED(hidSendVibrationValues(slot->vibration_handles.data(), values.data(),
                                      slot->vibration_count))) {
    slot->vibration_sent = false;  // retried on the next call
    return X_ERROR_FUNCTION_FAILED;
  }
  slot->vibration_sent = true;
  slot->vibration_left = izquierdo;
  slot->vibration_right = derecho;
  return X_ERROR_SUCCESS;
}

X_RESULT SwitchInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                               X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = FindSlot(id);
  if (!slot) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // If input is not active (e.g. due to a dialog overlay), force buttons to
  // "unpressed". The algorithm will automatically send UP events when
  // 'is_active()' goes low and DOWN events when it goes high again.
  const bool is_active = this->is_active();
  const uint64_t curr_butts =
      is_active ? (static_cast<uint64_t>(static_cast<uint16_t>(slot->state.gamepad.buttons)) |
                   AnalogToKeyfield(slot->state.gamepad))
                : uint64_t(0);

  // Handle repeating
  const auto guest_now = static_cast<uint32_t>(rex::chrono::Clock::QueryGuestUptimeMillis());
  if (slot->repeat_state == RepeatState::kWaiting &&
      (slot->repeat_time + kRepeatDelayMs < guest_now)) {
    slot->repeat_state = RepeatState::kRepeating;
  }
  if (slot->repeat_state == RepeatState::kRepeating &&
      (slot->repeat_time + kRepeatRateMs < guest_now)) {
    slot->repeat_time = guest_now;
    rex::ui::VirtualKey vk = kVkLookup.at(slot->repeat_index);
    out_keystroke->virtual_key = uint16_t(vk);
    out_keystroke->unicode = 0;
    // InputSystem stamps the guest user this device is assigned to.
    out_keystroke->user_index = 0;
    out_keystroke->hid_code = 0;
    out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    return X_ERROR_SUCCESS;
  }

  const uint64_t butts_changed = curr_butts ^ slot->key_buttons;
  if (!butts_changed) {
    return X_ERROR_EMPTY;
  }

  // First try to clear buttons with up events. This is to match xinput
  // behavior when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
  // up before THUMB_LEFT is down.
  for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
    for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
      const uint64_t fbutton = uint64_t(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      rex::ui::VirtualKey vk = kVkLookup.at(i);
      if (vk == rex::ui::VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;

      const bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        slot->key_buttons &= ~fbutton;
        slot->repeat_state = RepeatState::kIdle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        slot->key_buttons |= fbutton;
        slot->repeat_state = RepeatState::kWaiting;
        slot->repeat_index = i;
        slot->repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::nx

// nfsmw - scene render targets without tiling or MSAA
//
// ===========================================================================
//  What the original game does
//
//  To get 4x MSAA at 720p with the 360's 10 MB of EDRAM, NFSMW does not draw
//  the scene into a 1280x720 surface: it uses the XDK Direct3D "tiling".
//  BeginTiling records the scene commands and EndTiling replays them once
//  per tile, on 1280x256 surfaces with 4 samples, resolving each tile into
//  the 1280x720 texture. Three tiles = the scene is drawn three times every
//  frame.
//
//  Console D3D trace: Count=3 and 1280x256 surfaces with
//  D3DMULTISAMPLE_4_SAMPLES (r6=2) in the menu. A/B test: menu +30 % FPS; in
//  a race the scene was not replayed per tile (no change).
//
//  Why it is so expensive here
//  On the 360, replaying the scene per tile is almost free. Under Xenos
//  emulation each replay goes through the command processor again (~80 us
//  of CPU per draw) and through the GPU, with 4 samples per pixel, three
//  4-sample resolve dumps per tile and transfers between the tiles' 4x
//  render targets and the 1x ones of the rest of the frame. Those are
//  exactly the three costs measured with the A/B bench on the console:
//  draws -95 ms, transfers -50 ms, resolves -50 ms per frame.
//
//  How the game chooses (recompiled code)
//    sub_82458310  renderer constructor: table of 6 AA modes at
//                  object+4 (tiles), +28 (width), +52 (height), +76 (MSAA)
//                  and +100+64*mode (tile rectangles).
//                    mode 2: 1 tile  1280x736  1x   <- no antialiasing
//                    mode 3: 2 tiles  640x736  2x
//                    mode 4: 3 tiles 1280x256  4x   <- the one used at 720p
//                    mode 5: 4 tiles  320x736  4x
//    sub_824402F0  XGetVideoMode: in HD with width >= 1280 it sets mode 4.
//    sub_82441990  switches live between modes 2, 3 and 4. So the retail
//                  game already renders in mode 2 when it lowers quality:
//                  it is not an invented state.
//    sub_82458850  registers one set of render targets per mode.
//    sub_8245D320  copies the descriptor (128 bytes) to the table 0x82A4527C.
//    sub_8245D5F8  binds a set; if byte +41 is 1 -> BeginTiling with Count
//                  at +124 and the rectangles at +44.
//
//  What we change
//  1. Before the sets are registered, modes 3, 4 and 5 get the
//     configuration of mode 2 (1 tile of 1280x736 without MSAA). The game's
//     mode selector is untouched: whichever it picks, the scene is drawn
//     once. Everything else that depends on the mode stays as in retail.
//  2. Any set registered with MSAA is changed to 1 sample. This covers the
//     256x256 reflection with 4x (sub_8243C1C8) and the SD modes.
//
//  MarathonRecomp-NX does the same in its renderer: SurfaceSize returns 0
//  (no tiling) and on Switch it leaves MSAA off. Antialiasing is lost.
//
//  How to disable it without a new build
//  In nfsmw.toml, next to the .nro:   nfsmw_render_sin_mosaico = false
// ===========================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/platform.h>
#if REX_PLATFORM_SWITCH
// The handheld/docked mode to obey, Reverse-NX included.
#include <switch.h>

#include <rex/ui/switch_saltynx.h>
#endif
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "nfsmw_nativo_xenos.h"

REXCVAR_DEFINE_BOOL(nfsmw_render_sin_mosaico, REX_PLATFORM_SWITCH != 0, "NFSMW",
                    "Dibujar la escena una sola vez, sin tiling de 3 tiras ni MSAA "
                    "(evita repetir la escena por tira bajo la emulacion de Xenos)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// With the mode 4 table made equal to mode 2's, the game stayed in mode 4 with a one-tile scene. With
// this it really uses its mode 2, the game's own mode without antialiasing (sub_82441990 picks it when
// lowering quality). It was done on the theory that this mismatch caused the blue edges against the sky,
// but no: they look the same in both modes and in the emulated renderer; they come from the game's bright
// pass (see nfsmw_resplandor_cielo).
REXCVAR_DEFINE_BOOL(nfsmw_render_modo_sin_aa, true, "NFSMW",
                    "Con nfsmw_render_sin_mosaico: el juego usa de verdad su modo 2 (una tira sin antialiasing) en vez "
                    "del 3, 4 o 5 con la tabla del 2. No cambia los bordes azules del cielo (eso es "
                    "nfsmw_resplandor_cielo). false: como antes de la build 148")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Testing only: split the two parts of the hook to see which one causes the blue edges.
REXCVAR_DEFINE_BOOL(nfsmw_render_prueba_mantener_tiras, false, "NFSMW",
                    "Solo pruebas: con nfsmw_render_sin_mosaico, no igualar los modos 3-5 al 2 (se quedan sus tiras)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(nfsmw_render_prueba_mantener_msaa, false, "NFSMW",
                    "Solo pruebas: con nfsmw_render_sin_mosaico, no quitar el MSAA ni de los modos ni de los conjuntos")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The internal resolution comes from nfsmw_resolucion_interna (Graficos category), and with
// "automatico" it follows the dock live: 1920x1080 docked and 1280x720 handheld.
//
// How it works without a restart: the game already switches AA mode on the fly (sub_82441990 jumps
// between 2, 3 and 4 when lowering quality) and each mode has its own set of render targets, registered
// at startup. So mode 2 keeps its usual 1280x736 and mode 4 gets a 1920x1088 with a visible area of
// 1920x1080; then it only takes writing 2 or 4 into the mode index the game reads. It is the same path
// retail uses.
//
// "Docked" is the effective mode: the real dock or the one Reverse-NX fakes. With Reverse-NX the clocks
// do not go up, so 1080p there costs a third of the FPS; that is a deliberate choice.
REXCVAR_DECLARE(std::string, nfsmw_resolucion_interna);

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfResolution(unsigned ancho, unsigned alto);
#endif

namespace nfsmw::render_targets {
namespace {

// Mode table inside the renderer object (see the header comment).
constexpr uint32_t kModos = 6;
constexpr uint32_t kOffTiras = 4;
constexpr uint32_t kOffAncho = 28;
constexpr uint32_t kOffAlto = 52;
constexpr uint32_t kOffMsaa = 76;
constexpr uint32_t kOffRects = 100;
constexpr uint32_t kBytesPorModoRects = 64;  // 4 D3DRECT de 16 bytes
constexpr uint32_t kModoSinAa = 2;
constexpr uint32_t kModo1080p = 4;  // its set becomes the 1920x1088 one, or the reduced scene's

/*
 * Mode 4's size. By default the 1080p one: 1920x1088 (1080 rounded up to a multiple of 32, like the game's own
 * 720 -> 736) with 1920x1080 visible, output mode 3.
 *
 * With nfsmw_resolucion_interna = escena_WxH (handheld performance choices), the same path at a smaller size: mode
 * 4 becomes W x H rounded up to 32 rows, and the front buffer and the game's logical screen size become W x H too,
 * exactly as the 1080p path raises them. So the scene, the post-processing chain, the HUD and the front buffer are
 * all drawn at W x H, consistent with each other, and the presenter scales the finished frame to the screen. Output
 * mode 2 (the HD 16:9 one): only 1080p uses 3. Decided at startup (the front buffer is created once).
 */
struct TamanoModo4 {
  uint32_t ancho = 1920, alto = 1088, visible = 1080, modo_salida = 3;
  bool reducida = false;  // an escena_WxH choice
};

TamanoModo4 LeerTamanoModo4() {
  TamanoModo4 t;
#if !NFSMW_DEKO3D
  // Only the deko3d build creates its resolved textures at the resolve's width (nfsmw_dk_destinos.cpp, Copiar): the
  // Vulkan path uses the copy destination's pitch, and a scene width that is not a multiple of 32 in the
  // post-processing chain shifts its effects to the left.
  return t;
#endif
  const std::string r = REXCVAR_GET(nfsmw_resolucion_interna);
  constexpr const char kPrefijo[] = "escena_";
  if (r.rfind(kPrefijo, 0) != 0) {
    return t;
  }
  const size_t x = r.find('x', sizeof(kPrefijo) - 1);
  if (x == std::string::npos) {
    return t;
  }
  const uint32_t ancho = uint32_t(std::strtoul(r.c_str() + sizeof(kPrefijo) - 1, nullptr, 10));
  const uint32_t alto = uint32_t(std::strtoul(r.c_str() + x + 1, nullptr, 10));
  // Widths in multiples of 32 (the game's own are), heights within the 720p ones.
  if (ancho < 640 || ancho > 1280 || (ancho & 31) || alto < 360 || alto > 720) {
    return t;
  }
  t.ancho = ancho;
  t.visible = alto;
  t.alto = (alto + 31) & ~31u;
  t.modo_salida = 2;
  t.reducida = true;
  return t;
}

const TamanoModo4& Modo4() {
  static const TamanoModo4 t = LeerTamanoModo4();
  return t;
}

// The four video size globals that sub_82441990 hard-codes to 1280/720, next to the output mode variable
// (0x82A2CF80). The game's front buffer size is taken from them (see below).
constexpr uint32_t kGlobalAncho0 = 0x82A2CF68;
constexpr uint32_t kGlobalAlto0 = 0x82A2CF6C;
constexpr uint32_t kGlobalAncho1 = 0x82A2CF70;
constexpr uint32_t kGlobalAlto1 = 0x82A2CF74;
constexpr uint32_t kGlobalModoSalida = 0x82A2CF80;
// The output mode global: its table 0 -> 640x480, 2 -> 1280x720, 3 -> 1920x1080 (Modo4().modo_salida).

// Puntero global al renderizador (lis r11,-32093 / lwz -11860).
constexpr uint32_t kRenderizadorGlobal = 0x82A2D1AC;

// Set descriptor that sub_8245D320 receives in r5.
constexpr uint32_t kDescAncho = 8;
constexpr uint32_t kDescAlto = 12;
constexpr uint32_t kDescMsaa = 36;
constexpr uint32_t kDescTiling = 41;  // byte
constexpr uint32_t kDescTiras = 124;

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

void Escribir32(uint8_t* base, uint32_t direccion, uint32_t valor) {
  const uint32_t v = __builtin_bswap32(valor);
  std::memcpy(base + direccion, &v, sizeof(v));
}

struct Modo {
  uint32_t tiras, ancho, alto, msaa;
};

// Original table MSAA per mode, valid once g_tabla_igualada is true.
std::array<std::atomic<uint32_t>, kModos> g_msaa_original{};
std::atomic<bool> g_tabla_igualada{false};

Modo LeerModo(const uint8_t* base, uint32_t obj, uint32_t m) {
  return {Leer32(base, obj + kOffTiras + 4 * m), Leer32(base, obj + kOffAncho + 4 * m),
          Leer32(base, obj + kOffAlto + 4 * m), Leer32(base, obj + kOffMsaa + 4 * m)};
}

void IgualarModosAlModoSinAa(uint8_t* base, uint32_t obj) {
  if (obj == 0) {
    REXLOG_WARN("[render] renderizador nulo al registrar los modos: no se toca nada");
    return;
  }

  const uint32_t modo_actual = Leer32(base, kRenderizadorGlobal) == obj ? Leer32(base, obj) : ~0u;
  for (uint32_t m = 0; m < kModos; ++m) {
    const Modo x = LeerModo(base, obj, m);
    REXLOG_INFO("[render] modo {} del juego: {} tira(s), {}x{}, MSAA {}", m, x.tiras, x.ancho,
                x.alto, x.msaa);
  }

  // Only act if the table is the one that was analyzed. If another version of the
  // executable had a different one, better not to write blindly.
  const Modo base_2 = LeerModo(base, obj, kModoSinAa);
  const uint32_t rect2 = obj + kOffRects + kBytesPorModoRects * kModoSinAa;
  const uint32_t r2_x1 = Leer32(base, rect2), r2_y1 = Leer32(base, rect2 + 4);
  const uint32_t r2_x2 = Leer32(base, rect2 + 8), r2_y2 = Leer32(base, rect2 + 12);
  const bool tabla_esperada = base_2.tiras == 1 && base_2.msaa == 0 && base_2.ancho == 1280 &&
                              base_2.alto >= 720 && r2_x1 == 0 && r2_y1 == 0 &&
                              r2_x2 == base_2.ancho && r2_y2 >= 720 && r2_y2 <= base_2.alto;
  if (!tabla_esperada) {
    REXLOG_WARN("[render] la tabla de modos no coincide con la analizada "
                "(modo 2: {} tira(s) {}x{} MSAA {}, rect {},{},{},{}): se deja como esta",
                base_2.tiras, base_2.ancho, base_2.alto, base_2.msaa, r2_x1, r2_y1, r2_x2, r2_y2);
    return;
  }

  for (uint32_t m = 0; m < kModos; ++m) {
    // The MSAA of each mode before touching it (MuestrasOriginalesModoActual).
    g_msaa_original[m].store(LeerModo(base, obj, m).msaa, std::memory_order_relaxed);
  }
  g_tabla_igualada.store(true, std::memory_order_relaxed);
  if (REXCVAR_GET(nfsmw_render_prueba_mantener_tiras)) {
    REXLOG_INFO("[render] prueba: los modos 3-5 se quedan con sus tiras (nfsmw_render_prueba_mantener_tiras)");
    return;
  }
  // Mode 2 keeps its usual size (1280x736). The 1080p one is mode 4, below: that way two sets are
  // registered, one per resolution, and it switches live by choosing the mode.
  const uint32_t ancho = base_2.ancho;
  const uint32_t alto = base_2.alto;
  for (uint32_t m = 3; m < kModos; ++m) {
    Escribir32(base, obj + kOffTiras + 4 * m, 1);
    Escribir32(base, obj + kOffAncho + 4 * m, ancho);
    Escribir32(base, obj + kOffAlto + 4 * m, alto);
    if (!REXCVAR_GET(nfsmw_render_prueba_mantener_msaa)) {
      Escribir32(base, obj + kOffMsaa + 4 * m, 0);
    }
    // Only the first tile is read (sub_82458850 copies tiles*16 bytes).
    std::memcpy(base + obj + kOffRects + kBytesPorModoRects * m, base + rect2, 16);
  }

  if (Modo4().reducida) {
    nfsmw::nativo::g_ancho_escena.store(Modo4().ancho, std::memory_order_relaxed);
  }
  // And mode 4 becomes the 1080p one, with its own set. That way the resolution changes live by choosing
  // mode 2 or mode 4, without registering anything again.
  {
    const uint32_t rect4 = obj + kOffRects + kBytesPorModoRects * kModo1080p;
    Escribir32(base, obj + kOffTiras + 4 * kModo1080p, 1);
    Escribir32(base, obj + kOffAncho + 4 * kModo1080p, Modo4().ancho);
    Escribir32(base, obj + kOffAlto + 4 * kModo1080p, Modo4().alto);
    Escribir32(base, obj + kOffMsaa + 4 * kModo1080p, 0);
    Escribir32(base, rect4 + 0, 0);
    Escribir32(base, rect4 + 4, 0);
    Escribir32(base, rect4 + 8, Modo4().ancho);
    Escribir32(base, rect4 + 12, Modo4().visible);
    REXLOG_INFO("[resolucion] modo {} preparado a {}x{} (area visible {}x{}): {}", kModo1080p, Modo4().ancho,
                Modo4().alto, Modo4().ancho, Modo4().visible,
                Modo4().reducida ? "la escena reducida (nfsmw_resolucion_interna)" : "es el de sobremesa");
  }

  REXLOG_INFO("[render] modos 3-5 igualados al modo 2: 1 tira {}x{} sin MSAA "
              "(modo seleccionado ahora: {}). La escena ya no se repite por tira.",
              ancho, alto, modo_actual);
}

std::atomic<uint32_t> g_conjuntos_sin_msaa{0};

// Mode the game had chosen before it was forced to 2 (0 = never forced).
std::atomic<uint32_t> g_modo_pedido{0};
std::atomic<uint32_t> g_avisos_modo{0};

bool ModoSinAaActivo() {
  return REXCVAR_GET(nfsmw_render_sin_mosaico) && REXCVAR_GET(nfsmw_render_modo_sin_aa);
}

// After each mode choice by the game (sub_824402F0 and sub_82441990): if it chose 3, 4 or 5, it stays at
// 2. It is written after the game has done its part with that choice (its quality calls do not read the
// index).
/*
 * Effective docked mode: the real dock or the one Reverse-NX fakes.
 *
 * Looking only at the hardware would avoid a known cost: in handheld mode with Reverse-NX, 1080p leaves a
 * race at 9.3-9.9 FPS against 13.8-14.8 at 720p (the clocks do not go up: the profile shows 307.2 MHz in
 * all fourteen reports, and the clock sysmodule in use does not register "sys:clk").
 *
 * Reverse-NX still decides the resolution too, by design and knowing that price. The fixed options are
 * there for whoever prefers otherwise.
 */
bool EnSobremesa() {
#if REX_PLATFORM_SWITCH
  return rex::ui::switch_saltynx::ModoBase(appletGetOperationMode() == AppletOperationMode_Console);
#else
  return false;
#endif
}

/*
 * The output size latch. Decided only once, in ImponerTamanoDeSalida (below), and needed up here because
 * ModoQueToca has to follow it.
 */
std::atomic<int> g_salida_latch{-1};

/*
 * The mode to use: 2 (1280x720) or 4 (1920x1080, or the reduced scene of an escena_WxH choice).
 *
 * The output latch decides, not the current Reverse-NX mode.
 *
 * The game's front buffer is created once at startup and never changes (log: "salida del juego al
 * arrancar: 1920x1080, impuesta"). If the scene followed Reverse-NX live and the front buffer did not,
 * the scene ended up drawn at 1280x720 in the corner of a 1920x1080 front buffer, with the HUD spanning
 * the 1920 width and the rest uninitialized: the cropping and the white blotch seen on the console. It is
 * the same reasoning already written in EscribirTamanoVideo: the logical screen size and the front buffer
 * size must always agree.
 *
 * So Reverse-NX decides the resolution, but it is read at startup. Changing it with the game running
 * moves the window and the clocks, not the internal resolution; that requires restarting the game.
 */
uint32_t ModoQueToca() {
  const int pestillo = g_salida_latch.load(std::memory_order_acquire);
  if (pestillo >= 0) {
    const uint32_t quiero = pestillo > 0 ? kModo1080p : kModoSinAa;
    // Only once: if Reverse-NX asks for the opposite, say so. The game's output is already created and
    // cannot change on the fly, so neither can the internal resolution: a restart is needed.
    static std::atomic<bool> avisado{false};
    if (!avisado.load(std::memory_order_relaxed) &&
        REXCVAR_GET(nfsmw_resolucion_interna) == "automatico") {
      const bool sobremesa = EnSobremesa();
      if ((sobremesa ? kModo1080p : kModoSinAa) != quiero && !avisado.exchange(true)) {
        REXLOG_INFO("[resolucion] Reverse-NX dice {} pero la salida del juego ya se creo para {}: la "
                    "resolucion interna NO cambia en marcha, hay que reiniciar el juego",
                    sobremesa ? "sobremesa" : "portatil",
                    pestillo > 0 ? "la del modo 4" : "1280x720");
      }
    }
    return quiero;
  }
  const std::string r = REXCVAR_GET(nfsmw_resolucion_interna);
  if (r == "1920x1080" || Modo4().reducida) {
    return kModo1080p;
  }
  if (r == "automatico") {
    return EnSobremesa() ? kModo1080p : kModoSinAa;
  }
  return kModoSinAa;  // 1280x720 and 1024x576 (the latter goes through the video mode)
}

std::atomic<uint32_t> g_modo_puesto{0};

void ForzarModoSinAa(uint8_t* base) {
  if (!ModoSinAaActivo()) {
    return;
  }
  const uint32_t obj = Leer32(base, kRenderizadorGlobal);
  if (obj == 0) {
    return;
  }
  const uint32_t modo = Leer32(base, obj);
  const uint32_t quiero = ModoQueToca();
  if (modo == quiero) {
    return;
  }
  if (modo < kModos) {
    const uint32_t antes = g_modo_pedido.exchange(modo, std::memory_order_relaxed);
    if (antes != modo && g_avisos_modo.fetch_add(1, std::memory_order_relaxed) < 16) {
      REXLOG_INFO("[render] el juego eligio el modo {}: se usa el {}", modo, quiero);
    }
  }
  Escribir32(base, obj, quiero);
  if (g_modo_puesto.exchange(quiero, std::memory_order_relaxed) != quiero) {
    REXLOG_INFO("[resolucion] {}x{} ({}): modo {}", quiero == kModo1080p ? Modo4().ancho : 1280,
                quiero == kModo1080p ? Modo4().visible : 720, EnSobremesa() ? "sobremesa" : "portatil", quiero);
#if REX_PLATFORM_SWITCH
    RexSwitchPerfResolution(quiero == kModo1080p ? Modo4().ancho : 1280, quiero == kModo1080p ? Modo4().visible : 720);
#endif
  }
}

}  // namespace

// What the output table hook needs.
bool QuiereSalida1080p() { return ModoQueToca() == kModo1080p; }

/*
 * Where the game's front buffer really comes from.
 *
 * Read in the recompiled code:
 *
 *   sub_824402F0 (the one that calls XGetVideoMode) hard-codes 1280 and 720 into the four globals as soon
 *   as the video mode is HD with width >= 1280:
 *       stw 1280 -> 0x82A2CF70 and 0x82A2CF68      stw 720 -> 0x82A2CF74 and 0x82A2CF6C
 *   and then sub_82440420 builds the D3DPRESENT_PARAMETERS (at 0x828FBB70) reading exactly the first two:
 *       lwz 0x82A2CF68 -> [params+0] BackBufferWidth      lwz 0x82A2CF6C -> [params+4] BackBufferHeight
 *   before calling CreateDevice (sub_825A1658).
 *
 * That is why raising the video mode to 1920x1080 did nothing: the game overwrites it with its constant
 * and never looks at what the system says beyond "it is HD". Here we overwrite the game's value.
 *
 * The pair 0x82A2CF70/74 is the logical screen size (read in dozens of places: HUD, projection, UI) and
 * the pair 0x82A2CF68/6C is the buffer's. All four are raised, which is what the game itself does.
 *
 * One limitation: the front buffer is created once, so this is decided at startup and does not change
 * later. Docking or undocking still changes the scene's internal resolution, not the output resolution.
 */
bool SalidaDelJuegoEs1080p() { return g_salida_latch.load(std::memory_order_acquire) > 0; }

void ImponerTamanoDeSalida(uint8_t* base, const char* donde) {
  int quiero = g_salida_latch.load(std::memory_order_acquire);
  if (quiero < 0) {
    quiero = (ModoSinAaActivo() && QuiereSalida1080p()) ? 1 : 0;
    int esperado = -1;
    if (g_salida_latch.compare_exchange_strong(esperado, quiero, std::memory_order_acq_rel)) {
      // 0 does not mean "1280x720": nothing is touched and the game picks the size, which with 1024x576 is
      // not 720p.
      REXLOG_INFO("[resolucion] salida del juego al arrancar: {} ({})",
                  quiero ? fmt::format("{}x{}, impuesta", Modo4().ancho, Modo4().visible)
                         : std::string("la que elija el juego"),
                  donde);
    } else {
      quiero = esperado;
    }
  }
  if (quiero <= 0) {
    return;
  }
  Escribir32(base, kGlobalAncho0, Modo4().ancho);
  Escribir32(base, kGlobalAlto0, Modo4().visible);
  Escribir32(base, kGlobalAncho1, Modo4().ancho);
  Escribir32(base, kGlobalAlto1, Modo4().visible);
  Escribir32(base, kGlobalModoSalida, Modo4().modo_salida);
  static std::atomic<uint32_t> avisos{0};
  if (avisos.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO("[resolucion] bufer frontal del juego: {}x{} ({})", Modo4().ancho, Modo4().visible, donde);
  }
}

// The game's video size, which its front buffer should come from.
void EscribirTamanoVideo(uint8_t* base) {
  // The latch decides, not the current mode. The logical screen size and the front buffer size must always
  // agree: otherwise the game lays out a 1920x1080 HUD over a 1280x720 buffer.
  if (!SalidaDelJuegoEs1080p()) {
    return;
  }
  Escribir32(base, kGlobalAncho0, Modo4().ancho);
  Escribir32(base, kGlobalAlto0, Modo4().visible);
  Escribir32(base, kGlobalAncho1, Modo4().ancho);
  Escribir32(base, kGlobalAlto1, Modo4().visible);
  Escribir32(base, kGlobalModoSalida, Modo4().modo_salida);
  static std::atomic<uint32_t> avisos{0};
  if (avisos.fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_INFO("[resolucion] tamano de video del juego: {}x{}, modo de salida {}", Modo4().ancho, Modo4().visible,
                Modo4().modo_salida);
  }
}

void EscribirSalida1080p(uint8_t* base, uint32_t direccion_ancho, uint32_t direccion_alto) {
  Escribir32(base, direccion_ancho, Modo4().ancho);
  Escribir32(base, direccion_alto, Modo4().visible);
  static std::atomic<uint32_t> avisos{0};
  if (avisos.fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_INFO("[resolucion] salida del juego: {}x{}", Modo4().ancho, Modo4().visible);
  }
}

// Samples per pixel of the AA mode the game has chosen, from its original table. Xbox 360 occlusion
// queries count samples (sub_82225610 expects 2048 in mode 4 and 1024 in mode 2), and the native
// renderer's scene always uses 1.
uint32_t MuestrasOriginalesModoActual(const uint8_t* base) {
  const uint32_t obj = Leer32(base, kRenderizadorGlobal);
  if (obj == 0) {
    return 1;
  }
  uint32_t modo = Leer32(base, obj);
  // With mode 2 forced, the samples of the mode the game had requested (what the Xbox 360 would count).
  if (const uint32_t pedido = g_modo_pedido.load(std::memory_order_relaxed);
      modo == kModoSinAa && pedido != 0 && ModoSinAaActivo()) {
    modo = pedido;
  }
  if (modo >= kModos) {
    return 1;
  }
  // Without a saved table (hook off) the game's table is read as is. D3DMULTISAMPLE_TYPE: 0 = 1, 1 = 2,
  // 2 = 4 samples.
  const uint32_t msaa = g_tabla_igualada.load(std::memory_order_relaxed)
                            ? g_msaa_original[modo].load(std::memory_order_relaxed)
                            : Leer32(base, obj + kOffMsaa + 4 * modo);
  return msaa == 1 ? 2 : msaa == 2 ? 4 : 1;
}

}  // namespace nfsmw::render_targets

// Diagnostic nfsmw_diag_luminancia: sub_82223308 averages the 64x64 luminance texture the game has just
// resolved (locked with LockRect) and sub_822234F0 stores the result as the target luminance for
// brightness adaptation (g_fAdaptedLum). The address, the first bytes and the result are logged.
REXCVAR_DEFINE_BOOL(nfsmw_diag_luminancia, false, "NFSMW",
                    "Solo pruebas: anota cada segundo la medida de luminancia del brillo (sub_82223308)");
REX_EXTERN(__imp__sub_82223308);
REX_HOOK_RAW(sub_82223308) {
  const uint32_t direccion = ctx.r3.u32;
  const uint32_t texels = ctx.r4.u32;
  __imp__sub_82223308(ctx, base);
  if (!REXCVAR_GET(nfsmw_diag_luminancia)) {
    return;
  }
  static std::atomic<uint64_t> ultimo{0};
  const uint64_t ahora = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
  if (ahora - ultimo.load(std::memory_order_relaxed) < 1000) {
    return;
  }
  ultimo.store(ahora, std::memory_order_relaxed);
  std::string bytes;
  uint64_t suma_b3 = 0, suma_b0 = 0;
  for (uint32_t i = 0; i < texels && i < 4096; ++i) {
    suma_b0 += base[direccion + i * 4];
    suma_b3 += base[direccion + i * 4 + 3];
  }
  for (uint32_t i = 0; i < 16; ++i) {
    bytes += fmt::format("{:02X}{}", base[direccion + i], (i % 4) == 3 ? " " : "");
  }
  REXLOG_INFO("[luminancia] sub_82223308({:08X}, {}) = {:.4f}; primeros bytes {}; media byte 0 {:.1f}, byte 3 {:.1f}",
              direccion, texels, ctx.f1.f64, bytes, double(suma_b0) / std::max<uint32_t>(texels, 1),
              double(suma_b3) / std::max<uint32_t>(texels, 1));
}

// The two functions that choose the AA mode. After the original runs, 3, 4 or 5 becomes 2.
REX_EXTERN(__imp__sub_824402F0);
REX_HOOK_RAW(sub_824402F0) {
  __imp__sub_824402F0(ctx, base);
  // This is the one that hard-codes 1280x720 into the four screen size globals.
  nfsmw::render_targets::ImponerTamanoDeSalida(base, "XGetVideoMode");
  nfsmw::render_targets::ForzarModoSinAa(base);
}

// sub_82440420 builds the D3DPRESENT_PARAMETERS from those globals and creates the device. It is the
// last chance to change the front buffer size: after that it already exists.
REX_EXTERN(__imp__sub_82440420);
REX_HOOK_RAW(sub_82440420) {
  nfsmw::render_targets::ImponerTamanoDeSalida(base, "antes de CreateDevice");
  __imp__sub_82440420(ctx, base);
}
REX_EXTERN(__imp__sub_82441990);
REX_HOOK_RAW(sub_82441990) {
  __imp__sub_82441990(ctx, base);
  // This is the one that sets up video and hard-codes the size, so it is changed here.
  nfsmw::render_targets::EscribirTamanoVideo(base);
  nfsmw::render_targets::ForzarModoSinAa(base);
}
// Sample reference for the flare occlusion queries (sub_82225610): with mode 2 forced it is computed
// with the mode the game had requested, so the flare fades as on the Xbox 360 in that mode.
REX_EXTERN(__imp__sub_82225610);
REX_HOOK_RAW(sub_82225610) {
  using namespace nfsmw::render_targets;
  const uint32_t pedido = g_modo_pedido.load(std::memory_order_relaxed);
  const uint32_t obj = Leer32(base, kRenderizadorGlobal);
  if (pedido != 0 && obj != 0 && ModoSinAaActivo() && Leer32(base, obj) == kModoSinAa) {
    Escribir32(base, obj, pedido);
    __imp__sub_82225610(ctx, base);
    Escribir32(base, obj, kModoSinAa);
    return;
  }
  __imp__sub_82225610(ctx, base);
}

// Registration of the 6 main scene sets, one per AA mode.
// r3 = renderer. The table is fixed before the original reads it.
REX_EXTERN(__imp__sub_82458850);
REX_HOOK_RAW(sub_82458850) {
  if (REXCVAR_GET(nfsmw_render_sin_mosaico)) {
    nfsmw::render_targets::IgualarModosAlModoSinAa(base, ctx.r3.u32);
  }
  __imp__sub_82458850(ctx, base);
}

// The game's output resolution, which its front buffer comes from. The video mode does not decide it
// (tested: with video_mode 1920x1080 the front buffer stayed at 1280x720); this table of the game's does:
// it reads a global and returns 640x480, 1280x720 or 1920x1080 through its output pointers. The 1080p
// mode was already in the 2005 binary; nothing selects it, so it is imposed here.
REX_EXTERN(__imp__sub_82447F78);
REX_HOOK_RAW(sub_82447F78) {
  const uint32_t salida_ancho = ctx.r4.u32;
  const uint32_t salida_alto = ctx.r5.u32;
  __imp__sub_82447F78(ctx, base);
  if (!nfsmw::render_targets::QuiereSalida1080p() || !salida_ancho || !salida_alto) {
    return;
  }
  nfsmw::render_targets::EscribirSalida1080p(base, salida_ancho, salida_alto);
}

// Binding of a render target set. The game does it every frame, so it is where the resolution is checked
// for a change because the console has been docked or undocked.
REX_EXTERN(__imp__sub_8245D5F8);
REX_HOOK_RAW(sub_8245D5F8) {
  nfsmw::render_targets::ForzarModoSinAa(base);
  __imp__sub_8245D5F8(ctx, base);
}

// Registration of a set of render targets. r5 = the caller's temporary descriptor,
// which the original copies to the global table. MSAA is removed from it; the
// caller writes that field again before each registration.
REX_EXTERN(__imp__sub_8245D320);
REX_HOOK_RAW(sub_8245D320) {
  using namespace nfsmw::render_targets;
  const uint32_t desc = ctx.r5.u32;
  if (desc != 0 && REXCVAR_GET(nfsmw_resolucion_interna) == "1920x1080") {
    // With the test on, every set matters, with or without MSAA: if their sizes grow with the table, this
    // is the 1080p path; if they stay at 1280x720, they come from somewhere else.
    static std::atomic<uint32_t> avisos{0};
    if (avisos.fetch_add(1, std::memory_order_relaxed) < 32) {
      REXLOG_INFO("[1080p] conjunto registrado: {}x{}, MSAA {}, tiling {} con {} tira(s)",
                  Leer32(base, desc + kDescAncho), Leer32(base, desc + kDescAlto), Leer32(base, desc + kDescMsaa),
                  base[desc + kDescTiling], Leer32(base, desc + kDescTiras));
    }
  }
  if (REXCVAR_GET(nfsmw_render_sin_mosaico) && !REXCVAR_GET(nfsmw_render_prueba_mantener_msaa) && desc != 0) {
    const uint32_t msaa = Leer32(base, desc + kDescMsaa);
    if (msaa != 0) {
      Escribir32(base, desc + kDescMsaa, 0);
      const uint32_t n = g_conjuntos_sin_msaa.fetch_add(1, std::memory_order_relaxed) + 1;
      REXLOG_INFO("[render] conjunto {}x{} registrado sin MSAA (pedia {}), tiling {} con {} tira(s); "
                  "{} conjunto(s) corregidos",
                  Leer32(base, desc + kDescAncho), Leer32(base, desc + kDescAlto), msaa,
                  base[desc + kDescTiling], Leer32(base, desc + kDescTiras), n);
    }
  }
  __imp__sub_8245D320(ctx, base);
}

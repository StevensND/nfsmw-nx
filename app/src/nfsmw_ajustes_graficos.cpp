// nfsmw - graphics settings in the menu (F4) that work with the native renderer (see nfsmw_ajustes_graficos.h).

#include "nfsmw_ajustes_graficos.h"

#include "nfsmw_nativo_sistema.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/overlay/settings_overlay.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(nfsmw_resolucion_interna, "automatico", "Graficos",
                      "Resolucion a la que dibuja el juego. automatico: 1920x1080 en sobremesa y 1280x720 en "
                      "portatil, cambiando en marcha al meter y sacar la consola de la base Y siguiendo tambien a "
                      "Reverse-NX. Ojo: con Reverse-NX los relojes no suben, asi que ahi el 1080p cuesta un tercio "
                      "de los FPS. 1280x720 es la de la Xbox 360. 1920x1080 la fija siempre. 1024x576 es el modo de "
                      "menos resolucion del propio juego: 36 % menos pixeles (este pide reiniciar). "
                      "640x360 y 640x480 bajan mas, pero OJO: medido el 20/09, esto NO cambia la resolucion a la "
                      "que DIBUJA el juego -sigue en 1280x720- sino solo el tamano al que se encoge al resolver. "
                      "Lo que ahorra es el posproceso, las copias y el cubo; la escena no se mueve. De 1280x720 a "
                      "1024x576 son 2,14 ms reales y el posproceso ya baja a 0,03, asi que por debajo queda poco "
                      "que rascar y el escalado si se ve"
#if NFSMW_DEKO3D
                      ". escena_1120x630, escena_1024x576 y escena_960x540 SI bajan la resolucion a la que dibuja el "
                      "juego: la escena, el posproceso, el HUD y el bufer frontal (por el mismo camino que el "
                      "1080p), y el presentador lo escala a la pantalla; 77 %, 64 % y 56 % de los pixeles (piden "
                      "reiniciar). Solo en la build de deko3d: la de Vulkan crea las texturas resueltas con el ancho "
                      "de su pitch, y con anchos que no son multiplo de 32 el posproceso sale corrido a la izquierda"
#endif
                      )
    .allowed({"automatico", "1280x720", "1920x1080", "1024x576", "640x360", "640x480"
#if NFSMW_DEKO3D
              , "escena_1120x630", "escena_1024x576", "escena_960x540"
#endif
             });

/*
 * GPU MHz in handheld mode.
 *
 * The console starts games at 307.2 MHz. Nintendo later made a 460.8 MHz profile available for
 * the games that need it, and this one does: 25 FPS with the GPU at its limit. This is not an
 * overclock (no KIP or sysmodule is touched): it is requested through the official API (apm), the
 * same one any commercial game uses. Not every game requests it because the ones that do not
 * need it get longer battery life.
 *
 * 307.2 -> 460.8 is +50 % GPU. It costs battery and heat; with 0 the console stays
 * exactly as it was.
 *
 * How to check that it worked: the profiler reports the real frequency in every report
 * ("relojes: CPU 1020.0 MHz, GPU 307.2 MHz, ..."). If it still says 307.2, it did not take effect.
 */
REXCVAR_DEFINE_INT32(nfsmw_switch_gpu_mhz, 460, "Graficos",
                     "Switch: MHz de GPU que se piden al sistema en portatil (0 = no tocar nada, 384, "
                     "460). 460,8 MHz es un perfil oficial de Horizon, no un overclock; gasta mas "
                     "bateria y calienta mas")
    .allowed({"0", "384", "460"});

/*
 * The trade-off, just in case.
 *
 * The high GPU configurations come in two flavors: memory at 1331.2 (0x92220008) and memory at
 * 1600 (0x92220007). We want the first, because raising the RAM clock costs battery and heat
 * without giving anything back: our measurements put bandwidth at 27 %, and what saturates is the
 * fragment ALU. If this firmware lacks it, the default is to leave the GPU as it was rather than
 * raise the RAM clock. With this set to true the trade-off is accepted: high GPU with RAM at 1600.
 */
REXCVAR_DEFINE_BOOL(nfsmw_switch_ram_1600, false, "Graficos",
                    "Switch: aceptar el perfil de GPU alta aunque suba la memoria a 1600 MHz. Por "
                    "defecto NO: si no hay ninguno que deje la RAM donde estaba, la GPU se queda como "
                    "estaba. El log [apm] dice cual entro");

REXCVAR_DEFINE_BOOL(nfsmw_switch_saltynx, true, "Graficos",
                    "Switch: publica los FPS y la resolucion donde los leen los overlays de la consola "
                    "(SaltyNX) y sigue el modo portatil/sobremesa de Reverse-NX. Sin SaltyNX no hace nada");

REXCVAR_DEFINE_BOOL(nfsmw_switch_relojes_reverse, false, "Graficos",
                    "Switch: que los relojes sigan a Reverse-NX cuando no coincida con el hardware, en los DOS "
                    "sentidos, pidiendoselo al sysmodule (Horizon OC y demas forks de sys-clk). Si finge SOBREMESA con "
                    "la consola en la mano, se aplica TU columna de sobremesa (la del perfil del juego o la global): "
                    "eso es overclock y no sirve para medir. Si finge PORTATIL con la consola en la base, se baja a la "
                    "columna de portatil y, si la tienes vacia, a los relojes de serie (GPU 307,2 MHz y memoria "
                    "1331,2). Se suelta al coincidir otra vez y al salir");
/*
 * The rate it really ticks at, measured on the console (race, no overclock).
 *
 * nfsmw_limite_fps ends up in video_mode_refresh_rate, and BucleVblank (nfsmw_nativo_sistema.cpp) reads
 * it from there to know how often to fire the guest interrupt. With "60" the vblank thread ticks at
 * exactly 60.1 Hz: 601 vblanks per 10 s of log, spot on in every interval.
 *
 * It neither throttles nor drops work, and both are measured, not assumed:
 *   - It does not throttle: the game runs at 24.5 swaps/s, far below 60. The vblanks/swaps ratio is
 *     2.45, which is not an integer; if the game limited itself by counting vblanks it would be 2.00
 *     or 3.00. The pace is set by the work, not by the vblank.
 *   - Nothing is dropped: "presentados=6260 rechazos=0" in the log, and the profiler checks that the
 *     game presents 25.35/s and 25.35/s reach the window (nwindowQueueBuffer), deviation +0.0 %. Not
 *     a single frame is lost in the output mailbox.
 * Conclusion: at 60 there is nothing to gain or lose here. Setting "30" would only lower the guest
 * interrupt to 30 Hz (0.25 % of a core) and tell the game that the panel runs at 30; as long as a
 * frame takes 39 ms nothing changes. The time missing to reach 33.3 ms is not in this setting: it is
 * in the PM4 ring thread spending 6.4-7.5 ms per frame inside presentation without recording the next
 * frame.
 */
REXCVAR_DEFINE_STRING(nfsmw_limite_fps, "60", "Graficos",
                      "FPS maximos del juego. 60: sin limite propio (el juego va al ritmo de un vblank de 60 Hz). 30: "
                      "ritmo fijo de 30 FPS, sin altibajos, con el juego a su velocidad. Se aplica al reiniciar")
    .allowed({"60", "30"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

/*
 * Pace fixed by hardware, not by software.
 *
 * nfsmw_limite_fps does not limit presentation: it tells the native renderer's vblank thread at
 * what rate to fire the guest interrupt, and the game limits itself by counting vblanks. Those are two
 * different free-running 30 Hz clocks (the process timer and the panel), and they beat against each
 * other: most frames at 33.3 ms and every few seconds one at 50. Micro-stutters with the counter
 * showing 30.
 *
 * This is the other approach: it asks the compositor for one image every N vblanks. With 2 there is a
 * single clock, the panel's, and its cadence propagates back through backpressure to the PM4 ring
 * thread. The WSI sets it to 1 when the swapchain is created, so it is reapplied on every presentation
 * (switch_perf.cpp).
 */
/*
 * The default is 0, and this matters.
 *
 * The console swapchain is created in IMMEDIATE mode, which in the Horizon WSI means
 * nwindowSetSwapInterval(nw, 0): the queue stops blocking the producer. That is what closes the
 * 6.3-7.8 ms gap in which the GPU sits idle while the PM4 ring thread is inside presentation
 * (measured on the console: frame = GPU work + gap, exactly).
 *
 * But AnotarRitmo reapplies the interval on every presentation (on purpose, because the swapchain is
 * recreated when switching from docked to handheld mode). With the default at 2, or with a 1 in the
 * toml, the WSI's 0 was overwritten on the first frame and IMMEDIATE did nothing. With 0 it is left
 * alone and the swapchain mode applies, which is what we want.
 *
 * If the panel ever has to be locked again: 2 still works, but it only makes sense when the work
 * really fits in 33.3 ms. Because of the gap it does not fit, and with the gap closed interval 0
 * gives the same result without the risk of dropping to the next step.
 */
REXCVAR_DEFINE_UINT32(nfsmw_intervalo_swap, 0, "Graficos",
                      "Switch: vblanks entre imagenes. 0 = no tocarlo, manda el modo de la swapchain (lo normal "
                      "desde la compilacion 113: IMMEDIATE, o sea intervalo 0, que es lo que quita el hueco). "
                      "2 = 30 FPS clavados por el panel, solo util si el trabajo cabe en 33,3 ms. 1 = hasta 60");

// Optional antialiasing on the output, applied live. The Xbox 360 draws the scene with 4x MSAA; the Switch
// has none (it costs GPU time and hung NVK in another port), and FXAA smooths the edges with little work.
REXCVAR_DEFINE_STRING(nfsmw_antialiasing, "apagado", "Graficos",
                      "Antialiasing de la imagen. apagado: como hasta ahora. fxaa: suaviza los bordes dentados (la Xbox "
                      "360 usa MSAA 4x, que aqui no hay); cuesta algo de GPU")
    .allowed({"apagado", "fxaa"});

// Sky glow, applied live. The game's bright pass (PS n137) subtracts the threshold per channel: with an
// intense blue sky only the blue channel passes and leaves blue edges over the trees. natural: the same
// glow energy with the hue of the source color. suave: the threshold is applied to luminance, with much
// less halo.
// natural is the default: compared on the console, original still showed a noticeable blue halo.
REXCVAR_DEFINE_STRING(nfsmw_resplandor_cielo, "natural", "Graficos",
                      "Resplandor del cielo. natural (por defecto): el resplandor del juego sin el azul saturado que deja "
                      "bordes en arboles y tejados; las luces de colores igual. original: como la Xbox 360, con esos "
                      "bordes azules. suave: casi sin halo del cielo; las luces de un solo color (freno, policia) brillan "
                      "menos. Sin coste")
    .allowed({"original", "natural", "suave"});

// The game's color filter, applied live: its "visual treatment" (the yellow tint, the desaturation and the
// vignette of the final composite). Some players find the tint too strong in some areas of the city.
REXCVAR_DEFINE_STRING(nfsmw_tratamiento_visual, "original", "Graficos",
                      "Filtro de color del juego (el tono amarillo, la desaturacion y la vineta). original (por "
                      "defecto): como la Xbox 360. suave: a mitad de fuerza. apagado: sin filtro; el resplandor y los "
                      "fundidos siguen igual. Sin coste")
    .allowed({"original", "suave", "apagado"});

// Optional post-processing, applied live (visible when changed with the menu open). Option lists for the
// gamepad. The presets use the values from GoldenEye-Recomp (ge_postfx.cpp, public domain).
REXCVAR_DEFINE_STRING(nfsmw_posproceso, "apagado", "Graficos/Posproceso",
                      "Posproceso de la imagen. apagado: como la Xbox 360. Preajustes: cine, sepia, noir, frio, calido, "
                      "vivo, matrix y crt. personalizado: los valores de abajo. La gradacion de un solo canal no cuesta "
                      "nada; saturacion, vibracion, vineta y lineas hacen algo de trabajo por pixel")
    .allowed({"apagado", "cine", "sepia", "noir", "frio", "calido", "vivo", "matrix", "crt", "personalizado"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_brillo, "0.00", "Graficos/Posproceso",
                      "Personalizado: brillo que se suma (0.00 = sin cambio)")
    .allowed({"-0.20", "-0.15", "-0.10", "-0.05", "0.00", "+0.05", "+0.10", "+0.15", "+0.20"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_contraste, "1.00", "Graficos/Posproceso",
                      "Personalizado: contraste alrededor del gris medio (1.00 = sin cambio)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30", "1.40"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_saturacion, "1.00", "Graficos/Posproceso",
                      "Personalizado: saturacion (0.00 = blanco y negro, 1.00 = sin cambio)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00", "1.10", "1.20", "1.35", "1.50", "2.00"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_vibracion, "0.00", "Graficos/Posproceso",
                      "Personalizado: vibracion, sube mas los colores menos saturados (0.00 = sin cambio)")
    .allowed({"-0.50", "-0.25", "0.00", "+0.15", "+0.25", "+0.50", "+0.75", "+1.00"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_temperatura, "0.00", "Graficos/Posproceso",
                      "Personalizado: temperatura, calida (+) o fria (-) (0.00 = sin cambio)")
    .allowed({"-1.00", "-0.75", "-0.50", "-0.25", "0.00", "+0.25", "+0.50", "+0.75", "+1.00"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_gamma, "1.00", "Graficos/Posproceso",
                      "Personalizado: gamma, mas de 1 aclara los medios tonos (1.00 = sin cambio)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_vineta, "0.00", "Graficos/Posproceso",
                      "Personalizado: vineta, oscurece los bordes (0.00 = sin vineta)")
    .allowed({"0.00", "0.20", "0.30", "0.40", "0.55", "0.70", "1.00"});
REXCVAR_DEFINE_STRING(nfsmw_posproceso_lineas, "0.00", "Graficos/Posproceso",
                      "Personalizado: lineas de television antigua, una fila de cada tres (0.00 = sin lineas)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00"});

namespace nfsmw::ajustes {
namespace {

std::atomic<uint64_t> g_version_posproceso{1};
std::atomic<bool> g_fxaa{false};
std::atomic<int> g_resplandor_cielo{0};  // 0 original, 1 natural, 2 soft
std::atomic<int> g_tratamiento_visual{0};  // 0 original, 1 soft, 2 off

constexpr const char* kCvarsPosproceso[] = {
    "nfsmw_posproceso",           "nfsmw_posproceso_brillo",      "nfsmw_posproceso_contraste",
    "nfsmw_posproceso_saturacion", "nfsmw_posproceso_vibracion",  "nfsmw_posproceso_temperatura",
    "nfsmw_posproceso_gamma",     "nfsmw_posproceso_vineta",      "nfsmw_posproceso_lineas",
};

// nfsmw_resplandor_cielo: 0 original, 1 natural, 2 suave.
int ModoResplandor(std::string_view valor) {
  return valor == "natural" ? 1 : valor == "suave" ? 2 : 0;
}

// nfsmw_tratamiento_visual: 0 original, 1 suave, 2 apagado.
int ModoTratamiento(std::string_view valor) {
  return valor == "suave" ? 1 : valor == "apagado" ? 2 : 0;
}

float Numero(const char* nombre, float por_defecto) {
  const std::string texto = rex::cvar::GetFlagByName(nombre);
  char* fin = nullptr;
  const double valor = std::strtod(texto.c_str(), &fin);
  return fin && fin != texto.c_str() ? float(valor) : por_defecto;
}

// The command line takes precedence (PC tests with --video_mode_width=...). Whatever comes from the
// configuration file is replaced: SaveConfig saves the cvars that differ from their default value, and an
// old video mode must not win over the new settings.
void Poner(const char* nombre, const std::string& valor) {
  if (rex::cvar::GetFlagInfo(nombre) == nullptr) {
    REXLOG_WARN("[ajustes] el cvar {} no existe: no se aplica {}", nombre, valor);
    return;
  }
  if (rex::cvar::GetFlagSource(nombre) == rex::cvar::Source::kCommandLine) {
    REXLOG_INFO("[ajustes] {} viene de la linea de comandos ({}): no se toca", nombre,
                rex::cvar::GetFlagByName(nombre));
    return;
  }
  if (!rex::cvar::SetFlagByName(nombre, valor)) {
    REXLOG_WARN("[ajustes] no se pudo poner {} = {}", nombre, valor);
  }
}

}  // namespace

#if REX_PLATFORM_SWITCH
// Provided by the SaltyNX module so that the console overlays can show the resolution.
extern "C" void RexSwitchPerfResolution(unsigned ancho, unsigned alto);
extern "C" void RexSwitchPerfIntervaloSwap(unsigned vblanks);
extern "C" void RexSwitchSaltyNxHabilitar(int habilitado);
extern "C" void RexSwitchRelojesReverseHabilitar(int habilitado);
extern "C" void RexSwitchApmPedirGpuMhz(int mhz);
extern "C" void RexSwitchApmPermitirRam1600(int permitir);
extern "C" void RexSwitchApmAplicar(void);
#endif

void AplicarAjustesGraficos() {
  const std::string resolucion = REXCVAR_GET(nfsmw_resolucion_interna);
  // 1920x1080 is not requested through the video mode. The game has its own table and above 1280x720 it
  // stays at 1280x720, so asking for it does nothing. What does work is writing the size into the mode
  // table of its renderer: nfsmw_render_targets.cpp does that, reading this same cvar, and also switches
  // it on the fly when the console enters or leaves the dock (automatic mode). The video mode stays at
  // 720p, which is what is presented. Setting the video mode to 1920x1080 so that the game's front buffer
  // would grow with it was tried and does not work: the front buffer is registered earlier and stays at
  // 1280x720.
  // The escena_WxH choices go through the same path as 1080p (nfsmw_render_targets.cpp): the video mode stays 720p.
  const bool reducida = resolucion.rfind("escena_", 0) == 0;
  const bool interno_1080p = resolucion == "1920x1080" || resolucion == "automatico" || reducida;
  const std::string modo_video = interno_1080p ? std::string("1280x720") : resolucion;
  const size_t x = modo_video.find('x');
  if (x != std::string::npos) {
    Poner("video_mode_width", modo_video.substr(0, x));
    Poner("video_mode_height", modo_video.substr(x + 1));
  }
#if REX_PLATFORM_SWITCH
  // FPS and resolution for the console overlays, and honoring Reverse-NX. Can be turned off if it gets in the way.
  RexSwitchSaltyNxHabilitar(REXCVAR_GET(nfsmw_switch_saltynx) ? 1 : 0);
  RexSwitchRelojesReverseHabilitar(REXCVAR_GET(nfsmw_switch_relojes_reverse) ? 1 : 0);
  // GPU profile in handheld mode. See nfsmw_switch_gpu_mhz.
  RexSwitchApmPedirGpuMhz(REXCVAR_GET(nfsmw_switch_gpu_mhz));
  RexSwitchApmPermitirRam1600(REXCVAR_GET(nfsmw_switch_ram_1600) ? 1 : 0);
  RexSwitchApmAplicar();
  RexSwitchPerfIntervaloSwap(unsigned(REXCVAR_GET(nfsmw_intervalo_swap)));
#endif
  Poner("resolution", "");  // empty preset: the width and height above apply
  const std::string limite = REXCVAR_GET(nfsmw_limite_fps);
  Poner("video_mode_refresh_rate", limite == "30" ? "30" : "60");
  REXLOG_INFO("[ajustes] resolucion interna {} y limite de FPS {}: modo de video {}x{} a {} Hz", resolucion, limite,
              rex::cvar::GetFlagByName("video_mode_width"), rex::cvar::GetFlagByName("video_mode_height"),
              rex::cvar::GetFlagByName("video_mode_refresh_rate"));
#if REX_PLATFORM_SWITCH
  {
    // The overlay shows the resolution the game draws at, so with the 1080p internal mode it has to be
    // told 1920x1088, which is the real scene render target, not the video mode.
    unsigned ancho = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_width").c_str(), nullptr, 10));
    unsigned alto = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_height").c_str(), nullptr, 10));
    // nfsmw_render_targets.cpp publishes the real size every time the mode changes, because with
    // "automatico" it changes on the fly. Only the startup value is set here.
    if (resolucion == "1920x1080") {
      ancho = 1920;
      alto = 1080;
    } else if (reducida) {
      ancho = unsigned(std::strtoul(resolucion.c_str() + 7, nullptr, 10));
      alto = unsigned(std::strtoul(resolucion.c_str() + resolucion.find('x') + 1, nullptr, 10));
    }
    if (ancho && alto) {
      RexSwitchPerfResolution(ancho, alto);  // for the console overlay
    }
  }
#endif
}

void OcultarAjustesSinEfecto() {
  if (!nfsmw::nativo::Activo()) {
    return;  // with emulation, all of them do something
  }
  const std::vector<std::string> ocultos = {
      // From the emulated path: they do nothing with the native renderer.
      "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y", "vsync", "anisotropic_override",
      "swap_post_effect",
      // Replaced by nfsmw_resolucion_interna and nfsmw_limite_fps.
      "video_mode_width", "video_mode_height", "resolution", "video_mode_refresh_rate",
  };
  // On PC, the emulated path settings are registered by the GPU plugin, which is not loaded with the
  // native renderer: they do not exist there. On the Switch they are built into the executable and do show
  // up in the menu. They are hidden by name in both cases.
  std::string registrados;
  std::string sin_registrar;
  for (const std::string& nombre : ocultos) {
    std::string& lista = rex::cvar::GetFlagInfo(nombre) ? registrados : sin_registrar;
    lista += (lista.empty() ? "" : ", ") + nombre;
  }
  rex::ui::OcultarAjustesEnMenu(ocultos);
  REXLOG_INFO("[ajustes] menu: fuera, porque no hacen nada con el renderizador nativo o los sustituye la categoria "
              "Graficos: {}; no registrados en esta build: {}",
              registrados.empty() ? "ninguno" : registrados, sin_registrar.empty() ? "ninguno" : sin_registrar);
}

Posproceso LeerPosproceso() {
  struct Preajuste {
    const char* nombre;
    Posproceso valores;
  };
  // The GoldenEye-Recomp ones (ge_postfx.cpp): brightness, contrast, saturation, vibrance, temperature,
  // gamma, tint (red, green, blue and strength), vignette and scanlines.
  static const Preajuste kPreajustes[] = {
      {"cine", {true, -0.04f, 1.15f, 1.05f, 0.15f, -0.10f, 1.0f, 1.0f, 0.95f, 0.85f, 0.12f, 0.40f, 0.0f}},
      {"sepia", {true, -0.02f, 1.05f, 0.20f, 0.0f, 0.25f, 1.0f, 1.0f, 0.82f, 0.55f, 0.50f, 0.30f, 0.0f}},
      {"noir", {true, -0.03f, 1.35f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.55f, 0.0f}},
      {"frio", {true, 0.0f, 1.05f, 1.0f, 0.10f, -0.55f, 1.0f, 0.70f, 0.85f, 1.0f, 0.15f, 0.18f, 0.0f}},
      {"calido", {true, 0.02f, 1.05f, 1.05f, 0.15f, 0.55f, 1.0f, 1.0f, 0.85f, 0.60f, 0.12f, 0.18f, 0.0f}},
      {"vivo", {true, 0.0f, 1.10f, 1.20f, 0.50f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}},
      {"matrix", {true, -0.03f, 1.10f, 0.85f, 0.20f, 0.0f, 1.0f, 0.55f, 1.0f, 0.60f, 0.30f, 0.30f, 0.25f}},
      {"crt", {true, -0.02f, 1.05f, 1.10f, 0.10f, 0.0f, 1.0f, 0.90f, 1.0f, 0.90f, 0.10f, 0.20f, 0.50f}},
  };
  const std::string modo = rex::cvar::GetFlagByName("nfsmw_posproceso");
  for (const Preajuste& p : kPreajustes) {
    if (modo == p.nombre) {
      return p.valores;
    }
  }
  Posproceso resultado;
  if (modo != "personalizado") {
    return resultado;  // apagado
  }
  resultado.activo = true;
  resultado.brillo = Numero("nfsmw_posproceso_brillo", 0.0f);
  resultado.contraste = Numero("nfsmw_posproceso_contraste", 1.0f);
  resultado.saturacion = Numero("nfsmw_posproceso_saturacion", 1.0f);
  resultado.vibracion = Numero("nfsmw_posproceso_vibracion", 0.0f);
  resultado.temperatura = Numero("nfsmw_posproceso_temperatura", 0.0f);
  resultado.gamma = Numero("nfsmw_posproceso_gamma", 1.0f);
  resultado.vineta = Numero("nfsmw_posproceso_vineta", 0.0f);
  resultado.lineas = Numero("nfsmw_posproceso_lineas", 0.0f);
  return resultado;
}

uint64_t VersionPosproceso() {
  return g_version_posproceso.load(std::memory_order_relaxed);
}

bool AntialiasingFxaa() {
  return g_fxaa.load(std::memory_order_relaxed);
}

int ResplandorCielo() {
  return g_resplandor_cielo.load(std::memory_order_relaxed);
}

int TratamientoVisual() {
  return g_tratamiento_visual.load(std::memory_order_relaxed);
}

void VigilarAjustesEnVivo() {
  // The notification arrives with the registry lock held (SetFlagFromSource): only atomics are touched.
  for (const char* nombre : kCvarsPosproceso) {
    rex::cvar::RegisterChangeCallback(nombre, [](std::string_view, std::string_view) {
      g_version_posproceso.fetch_add(1, std::memory_order_relaxed);
    });
  }
  g_fxaa.store(rex::cvar::GetFlagByName("nfsmw_antialiasing") == "fxaa", std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_antialiasing", [](std::string_view, std::string_view valor) {
    g_fxaa.store(valor == "fxaa", std::memory_order_relaxed);
  });
  g_resplandor_cielo.store(ModoResplandor(rex::cvar::GetFlagByName("nfsmw_resplandor_cielo")),
                          std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_resplandor_cielo", [](std::string_view, std::string_view valor) {
    g_resplandor_cielo.store(ModoResplandor(valor), std::memory_order_relaxed);
  });
  g_tratamiento_visual.store(ModoTratamiento(rex::cvar::GetFlagByName("nfsmw_tratamiento_visual")),
                             std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_tratamiento_visual", [](std::string_view, std::string_view valor) {
    g_tratamiento_visual.store(ModoTratamiento(valor), std::memory_order_relaxed);
  });
}

}  // namespace nfsmw::ajustes

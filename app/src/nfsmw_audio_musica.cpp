// nfsmw - diagnostic: where the race music is lost (docs/deko3d-devlog.md, runs 7 and 8).
//
// The race music (SOUND\PFDATA\MW_MUSIC.MUS, a 4-channel EA-XA stream at 44.1 kHz) is read from the disc at the
// speed it plays, but nothing is heard. Ruled out so far: the XMP notification (the game never sends it), the native
// resampler, filter and gain sum (validated against the recompiled code in run 8: 0 differences, still silent), and
// sub_825E3230 (never called).
//
// sub_82618788 is the EA-XA stream decoder's "give me N samples" (r3 = decoder, r4 = the address of the output
// pointer, r5 = how many; returns how many it wrote, as floats in 16-bit sample units). This hook keeps, per decoder,
// the samples written and their peak, and every 10 s logs the busiest decoders: a decoder (or four, one per
// channel) producing ~44,100 samples per second is the music. Its peak says whether the decoded music is sound or
// silence; the WAV dump of the mix (audio_volcado_s) says whether it reaches the output.
//
// The function is called directly by the generated code: tools/llamadas_directas.py must run again after adding
// this hook (it finds the address in this file) so that the calls go through sub_82618788.
//
// nfsmw_audio_diag_musica: off by default. The sessions without music were probably the career's prologue (the user reports
// no music there until free roam is unlocked), so the bug is parked; turn it on to trace a silent race.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_audio_nativo.h"
#include "nfsmw_informe_diferido.h"

REXCVAR_DEFINE_BOOL(nfsmw_audio_diag_musica, false, "NFSMW",
                    "Diagnostico de la musica de las carreras: cada 10 s, los descodificadores EA-XA (sub_82618788) "
                    "con mas muestras, con su pico");

REX_EXTERN(__imp__sub_82618788);

namespace {

using namespace nfsmw::audio_nativo;

struct Decodificador {
  uint64_t muestras = 0;
  uint64_t llamadas = 0;
  float pico = 0.0f;
};

std::mutex g_candado;
std::unordered_map<uint32_t, Decodificador> g_decodificadores;
uint64_t g_ultimo_ms = 0;

uint64_t AhoraMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool Valida(uint32_t dir) {
  return (dir >= 0x40000000u && dir < 0x90000000u) || (dir >= 0xA0000000u && dir < 0xC0000000u);
}

}  // namespace

REX_HOOK_RAW(sub_82618788) {
  if (!REXCVAR_GET(nfsmw_audio_diag_musica)) {
    __imp__sub_82618788(ctx, base);
    return;
  }
  const uint32_t decodificador = ctx.r3.u32;
  const uint32_t puntero = ctx.r4.u32;
  const uint32_t salida = Valida(puntero) ? Leer32(base, puntero) : 0;
  __imp__sub_82618788(ctx, base);
  const uint32_t escritas = ctx.r3.u32;
  float pico = 0.0f;
  if (Valida(salida) && escritas && escritas < 65536) {
    for (uint32_t i = 0; i < escritas; ++i) {
      const float v = LeerFloat(base, salida + i * 4);
      if (std::isfinite(v)) {
        pico = std::max(pico, std::fabs(v));
      }
    }
  }
  const uint64_t ahora = AhoraMs();
  std::string linea;
  {
    std::lock_guard<std::mutex> cerrojo(g_candado);
    Decodificador& d = g_decodificadores[decodificador];
    d.muestras += escritas < 65536 ? escritas : 0;
    ++d.llamadas;
    d.pico = std::max(d.pico, pico);
    if (!g_ultimo_ms) {
      g_ultimo_ms = ahora;
    } else if (ahora - g_ultimo_ms >= 10000) {
      const double segundos = double(ahora - g_ultimo_ms) / 1000.0;
      std::vector<std::pair<uint32_t, Decodificador>> lista(g_decodificadores.begin(), g_decodificadores.end());
      std::sort(lista.begin(), lista.end(),
                [](const auto& a, const auto& b) { return a.second.muestras > b.second.muestras; });
      linea = fmt::format("[musica] ultimos {:.0f} s: {} descodificadores EA-XA activos; los de mas muestras:", segundos,
                          lista.size());
      for (size_t i = 0; i < std::min<size_t>(lista.size(), 8); ++i) {
        linea += fmt::format(" {:08X} {:.0f} muestras/s pico {:.0f} ({} llamadas);", lista[i].first,
                             double(lista[i].second.muestras) / segundos, lista[i].second.pico,
                             lista[i].second.llamadas);
      }
      g_decodificadores.clear();
      g_ultimo_ms = ahora;
    }
  }
  if (!linea.empty()) {
    NFSMW_INFORME_DIFERIDO("{}", linea);
  }
}

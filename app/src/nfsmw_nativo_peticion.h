// nfsmw - native renderer: what the graphics system hands a renderer backend for each draw, and the
// statistics and report helpers they share. No graphics API here: the Vulkan backend (nfsmw_nativo_dibujos.h,
// nfsmw_nativo_destinos.cpp) and the deko3d one (nfsmw_dk_destinos.cpp) both use it.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nfsmw::nativo {

struct EntradaShader;

// GPU time categories (ContextoDestinos::MarcarGpu and the C2 report).
inline constexpr uint32_t kGpuOtros = 0;
inline constexpr uint32_t kGpuSombras = 1;   // depth-only targets of 1600 or more
inline constexpr uint32_t kGpuEscena = 2;    // 1280
inline constexpr uint32_t kGpuReflejo = 3;   // 640
inline constexpr uint32_t kGpu320 = 4;       // 320: cubemap faces and blur
inline constexpr uint32_t kGpuMenores = 5;   // under 320
inline constexpr uint32_t kGpuCopias = 6;    // C2 copies to resolved textures
inline constexpr uint32_t kGpuBorrados = 7;  // C2 color and depth clears
// 1280 targets without depth: full-screen post-processing and HUD ("scene" is kept for the ones with
// depth, the geometry).
inline constexpr uint32_t kGpuEscenaSinProfundidad = 8;
// Not a pass type but the GPU gap between the end of one submission and the start of the next.
inline constexpr uint32_t kGpuHuecoEntreTrabajos = 9;
inline constexpr uint32_t kGpuCategorias = 10;
// Buckets of the histogram of intervals between Swaps (originally <15, 15-18, 18-25, 25-30, 30-36, 36-50,
// >=50 ms). Now 11 buckets: the top ones were all lumped into ">=50 ms", where 83 % of race frames fell,
// so a 55 ms frame could not be told from a 150 ms one, which is exactly the difference between "slow"
// and "stuttering".
inline constexpr uint32_t kCubetasSwap = 11;

// Stages of the C6 report. The last four split the pass change, which on the console is the most
// expensive and most variable stage.
inline constexpr uint32_t kEtapasDibujo = 12;

struct PeticionDibujo {
  const uint32_t* registros = nullptr;       // register mirror of the ring sink
  const EntradaShader* vs = nullptr;
  const EntradaShader* ps = nullptr;
  std::span<const uint32_t> vs_microcodigo;  // patched by D3D (last IM_LOAD)
  uint64_t generacion_vs = 0;                // changes with every VS IM_LOAD
  uint64_t generacion_constantes_vs = 0;     // changes when 0x4000-0x43FF are written
  uint64_t generacion_constantes_ps = 0;     // changes when 0x4400-0x47FF are written
  /*
   * The two most expensive stages of recording a draw on the console are "texturas" (4.1 ms per frame) and
   * the viewport/scissor that the C6 report includes in "pipeline". Both are pure functions of registers
   * that almost never change between consecutive draws, so the ring sink tracks when they really change
   * (it compares the value before writing it, as with the constants) and the count arrives here.
   *
   * A consumer can keep the last value seen next to its result (the 48+16 texture slots and the 32 1/size
   * words of the shared block; the VkViewport, the VkRect2D and ndc[4]) and skip the whole recomputation
   * while it does not change; the framing cache does this with generacion_encuadre. A generation that goes
   * up too often only causes extra work, never a wrong draw.
   */
  uint64_t generacion_fetch = 0;             // changes when 0x4800-0x48BF are written (fetch constants)
  uint64_t generacion_encuadre = 0;          // viewport, tijera, recorte, modo de rasterizado
  // kVeg* flags (nfsmw_nativo_ganchos.h) of the record the draw comes with; 0 = no verdict from the game
  // (nfsmw_d3d_vegetacion_juego).
  uint16_t vegetacion_juego = 0;
};

struct EstadisticasDibujos {
  uint64_t dibujados = 0;
  uint64_t rechazados = 0;
  uint64_t pipelines = 0;
  uint64_t texturas = 0;
  uint64_t subidas_textura = 0;
  uint64_t megas_subidos = 0;  // vertices, indices, constants and textures
  uint64_t megas_texturas = 0;  // texture images created (base level, no eviction)
  uint64_t ms_pipelines = 0;  // creando pipelines, acumulado
  // Accumulated for "C6 contadores" (the system prints the differences per report).
  uint64_t pases = 0;             // render passes empezados
  uint64_t envios_llenos = 0;     // submissions due to a full upload buffer
  uint64_t ns_envios_llenos = 0;  // inside those submissions, including the wait for the GPU
  uint64_t bytes_vertices = 0;
  // Deduplication of vertex uploads within the frame.
  uint64_t dedupe_aciertos = 0;
  uint64_t dedupe_bytes = 0;
  uint64_t dedupe_colisiones = 0;
  uint64_t bytes_indices = 0;
  uint64_t samplers = 0;          // samplers preparados
  uint64_t samplers_cache = 0;    // of those, resolved by the register cache
  uint64_t ns_pases = 0;          // inside TerminarPase + EmpezarPase on a pass change
  uint64_t ns_vertices = 0;       // copying vertices with byte swap
  uint64_t entradas_calculadas = 0;   // CalcularEntrada due to another VS generation
  uint64_t ns_entradas = 0;           // inside CalcularEntrada
  uint64_t entradas_reutilizadas = 0; // EntradaDe with the same generation and VS
  uint64_t pases_por_generacion = 0;  // pass change due to a new command buffer
  uint64_t pases_por_destino = 0;     // pass change due to other render targets
  uint64_t pases_reanudados = 0;      // the same pass, closed earlier by a copy or a clear
  uint64_t ns_render_pass = 0;        // en vkCmdBeginRenderPass + vkCmdEndRenderPass
  uint64_t texels_pases = 0;          // area opened, summed over every pass opening
  // The same area, split by render target type (indices kGpuSombras..kGpuMenores).
  std::array<uint64_t, kGpuCategorias> texels_por_categoria{};
  // Pass openings per render target type, to know the average area of each.
  std::array<uint64_t, kGpuCategorias> pases_por_categoria{};
  // Draws and triangles per render target type. With each one's GPU time (C2 report) it is possible to
  // tell whether an expensive pass is geometry-bound or pixel-bound: the counts do not depend on the
  // machine, so what is measured on PC holds for the console.
  std::array<uint64_t, kGpuCategorias> dibujos_por_categoria{};
  // Draws with no color to write, by whether their pixel shader is needed.
  uint64_t dibujos_ps_inutil = 0;
  uint64_t dibujos_ps_necesario = 0;
  // Shadow map draws by their constant c1 (g_bShadowMapAlphaEnabled).
  uint64_t sombras_alfa_activa = 0;
  uint64_t sombras_alfa_apagada = 0;
  std::array<uint64_t, kGpuCategorias> triangulos_por_categoria{};
  // Submissions (upload slot changes) with constants through UBOs and in total, to separate the modes of
  // nfsmw_nativo_constantes_ubo_alternar_s in the report.
  uint64_t envios_ubo = 0;
  uint64_t envios = 0;
  // How many draws really change the 488 bytes of shared constants. It decides whether the comparison or
  // the double memcpy is what needs to get cheaper.
  uint64_t compartidas_miradas = 0;
  uint64_t compartidas_cambiadas = 0;
  uint64_t bytes_repetidos_fotograma = 0;  // nfsmw_nativo_diag_vertices_repetidos
  uint64_t bytes_iguales_anterior = 0;
  uint64_t ns_hash_vertices = 0;
  // Rejections by cause (Rechazar in the .cpp), most frequent first.
  std::vector<std::pair<uint32_t, uint64_t>> causas;
  // Accumulated nanoseconds per stage of the draws that get recorded: state, indices, textures, pass,
  // uploads, pipeline and recording.
  std::array<uint64_t, kEtapasDibujo> etapas_ns{};
  // Scene draws that allow or prevent early depth rejection.
  uint64_t escena_con_descarte = 0;
  uint64_t escena_sin_descarte = 0;
  // Recorded draws with the stopwatch (1 in kCronometroCada): the divisor of etapas_ns and ns_vertices.
  uint64_t dibujados_cronometrados = 0;
  // Shadow map vegetation dropped right on entry, before paying for indices, textures, upload and pass. It
  // was counted but never printed anywhere, so the gain could not be verified at first. Now it is printed.
  uint64_t vegetacion_pronto = 0;
};

/*
 * Incremented every time the ring thread handles a guest wait for the GPU (WAIT_REG_MEM). That is the
 * only point at which the game may legally rewrite a vertex range it has already referenced in this
 * frame, so it is where upload deduplication must forget what it recorded. See
 * nfsmw_nativo_vertices_dedupe.h.
 */
extern std::atomic<uint32_t> g_sincronizaciones_anillo;

/*
 * The ring thread's reports are written on another thread (nfsmw_nativo_informes_diferidos).
 *
 * The ring's periodic dump (the system report, every 20 s) took 29-32 ms, and not because of formatting:
 * the log is synchronous (log_async = false) and every 2-4 lines the SD FILE is flushed to the card. In
 * one race there were 6 stutters of 60 ms or more: 3 ended 40-70 ms after a dump started, and 1 within
 * 0.12 s of the C6 subetapas line (0.5 would be expected by chance).
 * The line is formatted on the calling thread (it reads the ring's state, which is only consistent on its
 * own thread) and queued; the "NFSMW informes" thread (priority 0x3B) writes it. The queue never blocks
 * the producer: if it fills up, the line is dropped and counted. Defined in nfsmw_nativo_sistema.cpp.
 */
void InformeDiferido(std::string linea);

// Like REXLOG_INFO, but the report thread does the SD write (InformeDiferido).
#define NFSMW_INFORME_ANILLO(...) ::nfsmw::nativo::InformeDiferido(fmt::format(__VA_ARGS__))

}  // namespace nfsmw::nativo

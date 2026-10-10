// nfsmw - native renderer, part C2: render targets, copies (resolve and clear) and presentation of the
// game's image.
//
// Measured on PC: each Swap presents the destination of the last copy (RB_COPY_DEST_BASE), and during the
// videos and the title screen there are 2 copies per frame, both with a clear. This part does that without
// EDRAM: a render target is one Vulkan image per (base, format, pitch), and a resolved texture is another
// image per destination address, which is where the Swap looks it up through its fetch constant.

#pragma once

#include "nfsmw_nativo_peticion.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nfsmw::nativo {

// Registers a copy needs, taken from the ring sink's register mirror.
struct RegistrosCopia {
  uint32_t rb_surface_info = 0;
  uint32_t rb_color_info[4] = {};
  uint32_t rb_depth_info = 0;
  uint32_t rb_copy_control = 0;
  uint32_t rb_copy_dest_base = 0;
  uint32_t rb_copy_dest_pitch = 0;
  uint32_t rb_copy_dest_info = 0;
  uint32_t rb_color_clear = 0;
  uint32_t rb_color_clear_lo = 0;
  uint32_t rb_depth_clear = 0;
  uint32_t pa_sc_window_offset = 0;
  uint32_t pa_sc_window_scissor_tl = 0;
  uint32_t pa_sc_window_scissor_br = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t pa_su_vtx_cntl = 0;
  // Fetch constant 0 as vertices: the copy rectangle (D3D9 puts it there).
  uint32_t fetch_vertices[2] = {};
};

// Fetch constant 0 as a texture at the time of the Swap: VdSwap writes it.
struct TexturaSwap {
  uint32_t dword[6] = {};
};

class DestinosNativos {
 public:
  // nullptr if the device is missing or presentation could not be set up.
  static std::unique_ptr<DestinosNativos> Crear(const rex::ui::vulkan::VulkanDevice* dispositivo,
                                                rex::memory::Memory* memoria);
  virtual ~DestinosNativos() = default;

  // Resolve and/or clear. false if it could not be done (each cause is logged once).
  virtual bool Copiar(const RegistrosCopia& registros) = 0;

  // Submits pending work and draws the Swap's texture into the presenter's output. false if that Swap has
  // no resolved texture.
  virtual bool Presentar(rex::ui::Presenter* presentador, const TexturaSwap& swap, uint32_t ancho,
                         uint32_t alto) = 0;

  // 256-entry gamma ramp loaded by the game, 10 bits per channel (red, green, blue). Subsequent outputs
  // apply it the way the Xbox 360 display does. Called from the same thread as Presentar.
  virtual void RampaGamma(const std::array<std::array<uint16_t, 3>, 256>& rampa) = 0;

  // The game's occlusion queries (ConsultaOclusion in nfsmw_nativo_sistema.cpp). Empezar: whatever is drawn
  // until Terminar counts for the base structure. Terminar: returns the last complete count measured for
  // that structure, in host samples and from an earlier frame, or false if there is none yet.
  virtual void EmpezarOclusion(uint32_t base) = 0;
  virtual bool TerminarOclusion(uint32_t base, uint64_t& muestras) = 0;
  // Accumulated: spans counted on the host, spans without room, published queries, published samples and
  // the maximum of a single query.
  virtual void EstadisticasOclusion(uint64_t valores[5]) const = 0;

  // Parts C3-C6: records a ring draw with its shaders. false if it could not.
  virtual bool Dibujar(const PeticionDibujo& peticion) = 0;
  virtual EstadisticasDibujos EstadisticasDeDibujos() const = 0;
  // Deferred vertex copies finished: before returning the read pointer to the game.
  virtual void EsperarSubidas() = 0;
  // Vertex copies queued and not done yet (only to measure the fences).
  virtual size_t CopiasPendientes() const { return 0; }

  // Depth copies to a resolved texture: they force keeping the depth tile.
  virtual uint64_t CopiasProfundidad() const = 0;
  // Depth clears: color ones are already in Estadisticas.
  virtual uint64_t BorradosProfundidad() const = 0;
  virtual void Estadisticas(uint64_t& copias, uint64_t& borrados, uint64_t& presentados,
                            uint64_t& rechazos) const = 0;
  // GPU nanoseconds of the measured submissions, accumulated (0 without timestamps).
  virtual void TiempoGpu(uint64_t& nanosegundos, uint64_t& trabajos) const = 0;
  // End timestamp of the last measured submission, in nanoseconds per timestampPeriod (0 without
  // timestamps). Used to measure the real timestamp scale: on the console NVK declares 1 ns per unit and
  // it is ~1.628 ns.
  virtual uint64_t MarcaGpuFinalNs() const = 0;
  // Submissions measured with precise intermediate timestamps (BOTTOM_OF_PIPE), accumulated.
  virtual uint64_t TrabajosMarcasPrecisas() const = 0;
  // Ring thread waits for the GPU (vkWaitForFences in Completar), accumulated.
  virtual void EsperasGpu(uint64_t& veces, uint64_t& nanosegundos) const = 0;
  // Actual duration of each GPU submission (wall clock, from submit to fence), to compare with the sum of
  // the timestamps.
  virtual void DuracionTrabajosGpu(uint64_t& veces, uint64_t& nanosegundos) const = 0;
  // Cost of starting each command buffer (Grabar with no recording active) and of writing the read-backs
  // to guest memory, accumulated: {recordings, ns in Grabar, ns resetting pools, read-backs written,
  // texels written, ns writing them}.
  virtual void CosteGrabar(uint64_t coste[6]) const = 0;
  // GPU nanoseconds accumulated per category (kGpuOtros..., nfsmw_nativo_dibujos.h).
  virtual void TiempoGpuPorCategoria(
      std::array<uint64_t, kGpuCategorias>& nanosegundos) const = 0;
  // Shaded fragments, vertex invocations and clipped primitives, accumulated per pass category (only
  // with nfsmw_nativo_estadisticas_pipeline).
  virtual void EstadisticasPipeline(std::array<uint64_t, kGpuCategorias>& fragmentos,
                                    std::array<uint64_t, kGpuCategorias>& vertices,
                                    std::array<uint64_t, kGpuCategorias>& primitivas) const = 0;
  // Fragments and draws per pixel shader number (0 = no PS), accumulated over the diagnostic frames, and
  // how many of those frames there have been.
  // Image copies and megapixels per size bucket (<=64x64, <=320x320, <=1024x1024, larger), accumulated.
  virtual void CopiasPorTamano(std::array<uint64_t, 4>& copias,
                               std::array<uint64_t, 4>& pixeles) const = 0;
  virtual void EstadisticasPorShader(std::vector<uint64_t>& fragmentos,
                                     std::vector<uint64_t>& dibujos,
                                     uint64_t& fotogramas) const = 0;
  // Swaps by interval since the previous one (kCubetasSwap buckets) and submissions whose first timestamp
  // came before the previous submission's last one (the GPU chains them without a gap), accumulated.
  virtual void IntervalosEntreSwaps(std::array<uint64_t, kCubetasSwap>& cubetas, uint64_t& solapes,
                                    double& peor_ms) const = 0;
  // Breakdown of Presentar's time, accumulated in nanoseconds and counts:
  // {waits for the previous output, ns; work submissions, ns in the queue lock, ns in vkQueueSubmit;
  //  output submissions, ns in the lock, ns in vkQueueSubmit; RefreshGuestOutput, ns before the callback,
  //  ns inside it, ns after}.
  virtual void CostePresentar(uint64_t coste[12]) const = 0;
};

}  // namespace nfsmw::nativo

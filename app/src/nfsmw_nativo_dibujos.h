// nfsmw - native renderer, parts C3, C4, C5c and C6: drawing with the NFSSPV library shaders onto the
// C2 render targets.
//
// Started as the version that validated the whole chain on PC with the videos and the title screen: 2D
// textures (base level), guest vertices and indices, constants, pipelines per state and render passes.
// What is not covered is rejected with the cause logged once (details in the .cpp).

#pragma once

#include <rex/ui/vulkan/device.h>

#include "nfsmw_nativo_peticion.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace nfsmw::nativo {

struct EntradaShader;

// Image of a render target or a texture, in a host format.
struct ImagenNativa {
  VkImage imagen = VK_NULL_HANDLE;
  // Only if the image has its own dedicated allocation. Images from the texture pool leave this NULL on
  // purpose: their memory is a chunk of a shared slab and cannot be freed on its own.
  VkDeviceMemory memoria = VK_NULL_HANDLE;
  // Texture pool slab, or UINT32_MAX if the image does not come from the pool. The literal is used here
  // instead of kBloquePoolInvalido to keep that header out of this one.
  uint32_t pool_bloque = 0xFFFFFFFFu;
  VkImageView vista = VK_NULL_HANDLE;
  uint32_t ancho = 0;
  uint32_t alto = 0;
  // nfsmw_nativo_sombras_escala: the shadow map is drawn smaller than the guest requests and upscaled when
  // resolved. This holds the size the guest thinks it has; 0 = the same.
  uint32_t ancho_guest = 0;
  uint32_t alto_guest = 0;
  // ZCULL: the scene's depth images are created without TRANSFER_DST so the driver can assign them a ZCULL
  // plane. Without that usage they cannot be cleared with vkCmdClearDepthStencilImage or receive copies:
  // they have to be cleared by opening a pass with loadOp = CLEAR, and they cannot be swapped with their
  // resolved texture.
  bool admite_destino_de_copia = true;
  VkFormat formato = VK_FORMAT_UNDEFINED;
  bool preparada = false;  // already in GENERAL and initialized
  // Resolved texture with RB_COPY_DEST_INFO.copy_dest_swap: the guest sees it with red and blue swapped
  // relative to the render target it comes from.
  bool intercambio_rb = false;
  // nfsmw_nativo_resolver_sin_copia: when a whole render target is resolved, its image is swapped with the
  // resolved texture's instead of copied. The target keeps that texture's old content: if the game draws
  // on it again without clearing first, the content has to be brought back.
  bool contenido_invalido = false;
  uint32_t resuelta_base = 0;  // where its content is while contenido_invalido
};

// What the C2 render target code provides to the draws. All on the ring thread.
class ContextoDestinos {
 public:
  virtual ~ContextoDestinos() = default;
  // Command buffer of the frame's work, recording. nullptr on failure.
  virtual VkCommandBuffer ComandosTrabajo() = 0;
  // Upload command buffer, recording: submitted right before the work one.
  virtual VkCommandBuffer ComandosSubida() = 0;
  // Changes every time the work buffer starts recording again.
  virtual uint64_t GeneracionComandos() const = 0;
  // Render targets already in GENERAL. May record commands: call outside a pass.
  virtual ImagenNativa* DestinoColor(uint32_t base, uint32_t formato, uint32_t pitch) = 0;
  virtual ImagenNativa* DestinoProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) = 0;
  // Texture resolved by C2 at that physical address, or nullptr.
  virtual const ImagenNativa* TexturaResuelta(uint32_t direccion) = 0;
  // nfsmw_nativo_profundidad_perezosa. While set, the depth textures requested belong to a sample the
  // shader does not take (the final composite without blur) and do not force a copy.
  virtual void LecturasDeProfundidadMuertas(bool muertas) { (void)muertas; }
  // nfsmw_nativo_diag_borrados. What a pass loads and stores of that render target (its renderArea): the
  // part of what was cleared that is actually used. Called before opening the pass.
  virtual void AnotarAreaDePase(const ImagenNativa* imagen, uint32_t ancho, uint32_t alto) {
    (void)imagen;
    (void)ancho;
    (void)alto;
  }
  // nfsmw_nativo_sombra_minimo (see nfsmw_nativo_destinos.cpp). When a pass opens: true if it is the
  // shadow map car pass that C2 is observing or applying, and then each draw is validated with
  // DibujoDeCochesSombra. `solo_profundidad`: the pass has no color targets.
  virtual bool PaseDeCochesSombra(const ImagenNativa* profundidad, bool solo_profundidad) {
    (void)profundidad;
    (void)solo_profundidad;
    return false;
  }
  // A draw of that pass: `exacto` if it leaves in the depth buffer the minimum of what was there and of
  // its fragments.
  virtual void DibujoDeCochesSombra(bool exacto, uint32_t control_z) {
    (void)exacto;
    (void)control_z;
  }
  // Address of the shadow map texture with cars to watch for when preparing samplers; 0 = none.
  virtual uint32_t DireccionSombraCoches() const { return 0; }
  // A draw samples that texture. `capaz`: its pixel shader has tfetch2DSombraMin on that register; `ps`,
  // its number. Returns the pair to take the minimum with (its view goes in the register's 3D index word),
  // or nullptr.
  virtual const ImagenNativa* CompaneraSombraCoches(bool capaz, uint32_t ps) {
    (void)capaz;
    (void)ps;
    return nullptr;
  }
  // Submits what was recorded and continues in the other slot, with its upload buffer empty.
  virtual bool EnviarYEsperar() = 0;
  // Waits for the GPU to finish everything pending and starts recording again. It is expensive (a
  // one-frame stutter), so it is only used as a last resort when memory runs out: with the GPU idle,
  // textures can be released regardless of when they were last used, because none is in use.
  virtual bool EsperarGpuDelTodo() = 0;
  // GPU timestamp: whatever is recorded from here counts toward that category.
  virtual void MarcarGpu(uint32_t categoria) = 0;
  // Host occlusion query for the game's open one, inside the open pass. Returns its index, or UINT32_MAX
  // if no game query is open or there is no room left. End it before closing the pass.
  virtual uint32_t EmpezarConsultaOclusion() = 0;
  virtual void TerminarConsultaOclusion(uint32_t indice) = 0;

  // nfsmw_reflejo_visibilidad. Our own occlusion query around a single draw that samples the reflection
  // (or around the witness, the final composite), inside the open pass and with no game query open.
  // Returns its index, or UINT32_MAX if it cannot be measured (the draw counts as visible). End it right
  // after the draw.
  virtual uint32_t EmpezarConsultaVisibilidad(bool testigo) {
    (void)testigo;
    return UINT32_MAX;
  }
  virtual void TerminarConsultaVisibilidad(uint32_t indice) { (void)indice; }

  // Pipeline statistics of a whole pass (shaded fragments, vertex invocations and primitives reaching
  // clipping), accumulated per category. UINT32_MAX if not measured.
  virtual uint32_t EmpezarEstadisticas(uint32_t categoria) = 0;
  virtual void TerminarEstadisticas(uint32_t indice) = 0;

  // The same per draw, tagged with the pixel shader number, in an occasional diagnostic frame. Returns
  // UINT32_MAX if not measured or if there is no room left.
  virtual uint32_t EmpezarEstadisticasDibujo(uint32_t etiqueta, uint32_t categoria) = 0;
  virtual void TerminarEstadisticasDibujo(uint32_t indice) = 0;
};

class DibujosVulkan {
 public:
  // nullptr if the device lacks capabilities (the reason is logged).
  static std::unique_ptr<DibujosVulkan> Crear(const rex::ui::vulkan::VulkanDevice* dispositivo,
                                              rex::memory::Memory* memoria,
                                              ContextoDestinos* contexto);
  virtual ~DibujosVulkan() = default;

  virtual bool Dibujar(const PeticionDibujo& peticion) = 0;
  // Before copies, clears and any command outside a pass.
  virtual void TerminarPase() = 0;
  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR, instead of with
  // vkCmdClearDepthStencilImage. Needed for images created without TRANSFER_DST, the only ones the driver
  // can give a ZCULL plane. Returns false if it could not.
  virtual bool BorrarProfundidadEnPase(VkCommandBuffer comandos, const ImagenNativa& imagen,
                                       float profundidad, uint32_t stencil) = 0;
  // nfsmw_nativo_borrar_area_util. Clears only the `area` rectangle of a color image by opening a pass
  // with loadOp = CLEAR. Returns false if it could not.
  virtual bool BorrarColorEnPase(VkCommandBuffer comandos, const ImagenNativa& imagen, const VkClearColorValue& color,
                                 const VkRect2D& area) {
    (void)comandos;
    (void)imagen;
    (void)color;
    (void)area;
    return false;
  }
  // Right before submitting: closes the pass and publishes the upload buffer.
  virtual void AntesDeEnviar() = 0;
  // Starts work in that work slot. Its upload buffer starts over: the GPU has finished the last work
  // submitted with it.
  virtual void UsarRanura(uint32_t ranura) = 0;
  // A C2 image is about to be destroyed: it is removed from the descriptors.
  virtual void OlvidarImagen(VkImage imagen) = 0;
  // A C2 view is about to be destroyed (Destruir, in nfsmw_nativo_destinos.cpp, with the GPU idle for that
  // image): the FramebufferDe cache framebuffers that use it are destroyed. Without this, if Vulkan gave
  // the same handle to another view, the cache returned a framebuffer created on the dead view.
  virtual void OlvidarVista(VkImageView vista) { (void)vista; }
  // The GPU is out of memory. Stops the GPU, releases half the texture cache and reports whether anything
  // was freed. Called by both texture and render target allocations: whichever runs out of memory first
  // calls this before giving up.
  virtual bool SoltarTexturasPorFaltaDeMemoria() = 0;
  // Before every C2 copy: a resolved texture may change image or channels.
  virtual void InvalidarTexturas() = 0;

  // Invalidates in the texture caches only what points to these two images. Used by the image swap of
  // nfsmw_nativo_resolver_sin_copia: dropping the whole cache twice per frame costs more than the copy it
  // saves (measured on PC).
  virtual void InvalidarImagenes(VkImage a, VkImage b) = 0;
  virtual EstadisticasDibujos Estadisticas() const = 0;
  // Draws recorded since start-up; C2 counts those that fall between two copies.
  virtual uint64_t Dibujados() const = 0;
  // Waits for the copy thread to finish the pending vertex copies (nfsmw_nativo_subidas_hilo).
  virtual void EsperarSubidas() = 0;
  // Vertex copies queued and not done yet (only to measure the fences).
  virtual size_t CopiasPendientes() const { return 0; }
  // With a game occlusion query open, each span of draws inside a pass is counted with a host query
  // (ContextoDestinos::EmpezarConsultaOclusion), which closes with the pass or when the game query closes.
  virtual void OclusionAbierta(bool abierta) = 0;
};


}  // namespace nfsmw::nativo

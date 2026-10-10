// nfsmw - deko3d renderer: the SDK's presentation interfaces on deko3d (docs/HowToDeko3d.md, P1). Switch only.
//
// ReXApp only talks to rex::ui::GraphicsProvider, Presenter and ImmediateDrawer, so the deko3d build brings its own:
//   ProveedorDeko3d     owns the device and its queue (Dispositivo) and the shader compile worker (Compilador);
//   PresentadorDeko3d   the guest output mailbox images, the swapchain on the window's NWindow, and painting: the
//                       guest output scaled with letterboxing (GetGuestOutputPaintFlow), then the UI drawers;
//   DibujanteDeko3d     Dear ImGui's draw lists (the Debug Menu, the F3 overlay) on top.
//
// The game's renderer refreshes the guest output from the ring thread (Presenter::RefreshGuestOutput): its
// refresher gets a ContextoSalidaDeko3d with the image to draw into, records into its own Nucleo and submits
// before returning. The presenter paints from the UI thread with its own Nucleo; both submit to the one queue, so
// the refresh is on the GPU before the paint that samples it. Each side puts a fragment barrier before touching
// the image (the GPU overlaps consecutive submissions).

#pragma once

#include <rex/ui/graphics_provider.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>

#include <deko3d.h>

#include <array>
#include <memory>
#include <mutex>

#include "nfsmw_dk_compilador.h"
#include "nfsmw_dk_nucleo.h"

namespace nfsmw::deko {

// The guest output images: RGB10A2 like the Vulkan presenter's, so the gamma ramp and the post-processing keep
// their precision.
inline constexpr DkImageFormat kFormatoSalida = DkImageFormat_RGB10A2_Unorm;

class ContextoSalidaDeko3d final : public rex::ui::Presenter::GuestOutputRefreshContext {
 public:
  ContextoSalidaDeko3d(bool& is_8bpc_out_ref, DkImage& imagen, uint32_t ancho, uint32_t alto, uint64_t version)
      : GuestOutputRefreshContext(is_8bpc_out_ref), imagen_(imagen), ancho_(ancho), alto_(alto), version_(version) {}
  // A render target of kFormatoSalida. Write it with a fragment barrier before (the presenter may still be
  // sampling it from an earlier paint) and submit before the refresher returns.
  DkImage& imagen() const { return imagen_; }
  uint32_t ancho() const { return ancho_; }
  uint32_t alto() const { return alto_; }
  // Changes when the image is created again (another size): the renderer's views of it must be made again.
  uint64_t version() const { return version_; }

 private:
  DkImage& imagen_;
  uint32_t ancho_, alto_;
  uint64_t version_;
};

class ProveedorDeko3d final : public rex::ui::GraphicsProvider {
 public:
  // The device, the queue and the compile worker. A deko3d failure ends the process (nfsmw_dk_nucleo.h).
  static std::unique_ptr<ProveedorDeko3d> Crear();
  ~ProveedorDeko3d() override;

  Dispositivo& dispositivo() { return dispositivo_; }
  Compilador& compilador() { return *compilador_; }

  std::unique_ptr<rex::ui::Presenter> CreatePresenter(
      rex::ui::Presenter::HostGpuLossCallback host_gpu_loss_callback =
          rex::ui::Presenter::FatalErrorHostGpuLossCallback) override;
  std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  ProveedorDeko3d() = default;
  Dispositivo dispositivo_;
  std::unique_ptr<Compilador> compilador_;
};

}  // namespace nfsmw::deko

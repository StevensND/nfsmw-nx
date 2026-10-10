// nfsmw - deko3d renderer: the renderer backend of the graphics system (DestinosNativos) on deko3d. Switch only.
//
// nfsmw_nativo_sistema.cpp reads the game's command ring and hands every draw, copy (resolve and clear) and Swap to
// a DestinosNativos (nfsmw_nativo_destinos.h); the Vulkan one is nfsmw_nativo_destinos.cpp. This is the deko3d
// one, on the ring thread with its own Nucleo.
//
// The implementation is RenderDk (nfsmw_dk_render.h): render targets, copies and the output pass in
// nfsmw_dk_destinos.cpp, draws in nfsmw_dk_dibujos.cpp, textures in nfsmw_dk_texturas.cpp.

#pragma once

#include <memory>

#include "nfsmw_nativo_destinos.h"

namespace nfsmw::deko {
class ProveedorDeko3d;
}

namespace nfsmw::nativo {

std::unique_ptr<DestinosNativos> CrearDestinosDeko3d(nfsmw::deko::ProveedorDeko3d* proveedor,
                                                     rex::memory::Memory* memoria);

}  // namespace nfsmw::nativo

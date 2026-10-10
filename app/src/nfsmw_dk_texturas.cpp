// nfsmw - deko3d renderer: the game's textures, samplers and image descriptors. See nfsmw_dk_render.h.
//
// The texture cache is the Vulkan backend's (nfsmw_nativo_dibujos.cpp, PrepararTextura), with the same keys, mip
// rules, untiling and check intervals: a texture's guest bytes are fingerprinted, and only untiled and uploaded
// when the fingerprint changes; textures that do not change are checked less and less often, down to once every
// 32-39 frames. What deko3d removes: an image is a layout over the image heap (no allocation, no bind thread), and
// the upload is a copy recorded in the slot's pre-list.
//
// Not yet: the sampled re-check of stable textures, the per-frame check budget and the fingerprint thread (P4), and
// cubemaps made of resolved faces (P3).

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <xxhash.h>

#include <algorithm>
#include <cstring>

#include "nfsmw_dk_render.h"
#include "nfsmw_nativo_xenos.h"

REXCVAR_DEFINE_BOOL(nfsmw_nativo_mipmaps, true, "NFSMW",
                    "Renderizador nativo: sube los niveles de mip que trae el juego (como en la Xbox 360). false: solo "
                    "el nivel base, como antes de la build 136")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_anisotropico, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 179): filtrado anisotropico de las texturas del mundo "
                     "(samplers lineales con mips). 0 = como la Xbox 360 (trilineal, borroso de lado y a lo lejos); "
                     "2, 4, 8 o 16 = mas nitido de lado, algo mas de GPU")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_mb_max, 512, "NFSMW",
                     "Renderizador nativo: MB de la cache de texturas antes de soltar las que no se usan desde hace "
                     "120 fotogramas (hasta quedar en el 75 %)")
    .range(64, 2048);
// The Vulkan path's texture checks, same names and defaults (nfsmw_nativo_dibujos.cpp is not in the deko3d build).
REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_muestreo, 8, "NFSMW",
                     "Renderizador nativo: las texturas ESTABLES se recomprueban con la huella de una muestra (primer y "
                     "ultimo bloque de 4 KB y 1 de cada 8, base y mips) en vez de con todos sus bytes; si la muestra "
                     "cambia se sigue por el camino completo. N = 1 de cada N recomprobaciones de cada textura sigue "
                     "siendo completa, y las 3000 primeras hacen las dos huellas: con un solo desacuerdo se apaga solo. "
                     "0 = siempre la huella completa")
    .range(0, 64);
REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_kb_fotograma, 6144, "NFSMW",
                     "Renderizador nativo: KB de memoria del invitado que las comprobaciones de texturas estables "
                     "pueden leer en un fotograma; las que no caben esperan 1-2 fotogramas (8 veces seguidas como "
                     "mucho). Reparte las comprobaciones que coincidian en el mismo fotograma. 0 = sin limite");

namespace nfsmw::nativo {
namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);
constexpr uint64_t kFotogramasSinUsoParaSoltar = 120;
constexpr uint32_t kDescriptoresReservados = 8;  // 0-2 empty textures, the rest free for the renderer

// The sampler's type in the constant table (D3DXPARAMETER_TYPE): what the translator declared the texture as.
bool TipoVolumen(uint16_t tipo) { return tipo == 8 || tipo == 13; }  // Texture3D, Sampler3D
bool TipoCubo(uint16_t tipo) { return tipo == 9 || tipo == 14; }     // TextureCube, SamplerCube

uint32_t Log2Bytes(uint32_t bytes) { return bytes >= 16 ? 4 : bytes >= 8 ? 3 : bytes >= 4 ? 2 : bytes >= 2 ? 1 : 0; }

// The fetch constant's swizzle through the channels the host format has (GuestToHostSwizzle; MapeoComponentes in
// the Vulkan backend). DkImageSwizzle is VkComponentSwizzle minus one.
void Swizzle(uint32_t swizzle_guest, uint16_t swizzle_host, DkImageSwizzle salida[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t valor = (swizzle_guest >> (i * 3)) & 0x7;
    if (valor <= 3) {
      salida[i] = DkImageSwizzle(DkImageSwizzle_Red + ((swizzle_host >> (valor * 3)) & 0x7));
    } else if (valor == 4) {
      salida[i] = DkImageSwizzle_Zero;
    } else {
      salida[i] = DkImageSwizzle_One;
    }
  }
}

// The original untiling, block by block (when the block is not a power of two bytes, which no format here has).
void LeerNivelMosaicoDeSiempre(const uint8_t* origen, uint32_t pitch_bloques, uint32_t log2, uint32_t bytes, uint32_t ox,
                               uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host, uint8_t* destino) {
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      const int32_t desplazamiento = DesplazamientoMosaico2D(int32_t(ox + x), int32_t(oy + y), pitch_bloques, log2);
      std::memcpy(destino + (size_t(y) * bx_host + x) * bytes, origen + desplazamiento, bytes);
    }
  }
}

}  // namespace

// Same host formats as the Vulkan backend (FormatoTexturaDe), as deko3d formats.
bool RenderDk::FormatoDe(uint32_t formato, FormatoDk& f) {
  switch (formato) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {DkImageFormat_R8_Unorm, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {DkImageFormat_RG8_Unorm, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {DkImageFormat_RGBA8_Unorm, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {DkImageFormat_RGB10A2_Unorm, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {DkImageFormat_RGBA_BC1, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {DkImageFormat_RGBA_BC2, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {DkImageFormat_RGBA_BC3, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {DkImageFormat_RG_BC5_Unorm, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {DkImageFormat_R_BC4_Unorm, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:  // k_16
      f = {DkImageFormat_R16_Unorm, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {DkImageFormat_RG16_Unorm, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {DkImageFormat_RGBA16_Unorm, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {DkImageFormat_R16_Float, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {DkImageFormat_RG16_Float, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {DkImageFormat_RGBA16_Float, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {DkImageFormat_R32_Float, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {DkImageFormat_RG32_Float, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {DkImageFormat_RGBA32_Float, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

// ---- Descriptors

uint32_t RenderDk::ReservarDescriptorImagen() {
  if (!descriptores_libres_.empty()) {
    const uint32_t r = descriptores_libres_.back();
    descriptores_libres_.pop_back();
    return r;
  }
  if (siguiente_descriptor_ < nucleo_.config().descriptores_imagen) {
    return siguiente_descriptor_++;
  }
  return 0;
}

void RenderDk::LiberarDescriptoresDe(uint64_t serie) {
  const auto it = vistas_por_serie_.find(serie);
  if (it == vistas_por_serie_.end()) {
    return;
  }
  auto& por_liberar = descriptores_por_liberar_[trabajo_ % descriptores_por_liberar_.size()];
  for (uint64_t clave : it->second) {
    if (const auto v = vistas_.find(clave); v != vistas_.end()) {
      por_liberar.push_back(v->second);
      vistas_.erase(v);
    }
  }
  vistas_por_serie_.erase(it);
}

void RenderDk::EscribirDescriptorImagen(uint32_t ranura, const DkImageView& vista) {
  AsegurarTrabajo();
  DkImageDescriptor d;
  dkImageDescriptorInitialize(&d, &vista, false, false);
  // In the pre-list: ordered before every draw of this submission, never under a draw that reads it (a freed slot is
  // only reused after its fence).
  dkCmdBufPushData(nucleo_.ComandosPrevios(), nucleo_.DescriptoresImagen() + DkGpuAddr(ranura) * sizeof(d), &d,
                   sizeof(d));
}

uint32_t RenderDk::VistaDe(const ImagenDk& imagen, uint32_t swizzle_guest, uint16_t swizzle_host) {
  const uint64_t clave = (imagen.serie << 24) ^ (uint64_t(swizzle_guest & 0xFFF) << 12) ^ swizzle_host;
  if (const auto it = vistas_.find(clave); it != vistas_.end()) {
    return it->second;
  }
  const uint32_t ranura = ReservarDescriptorImagen();
  if (!ranura) {
    Avisar(35, "tabla de descriptores de imagenes llena");
    return 0;
  }
  DkImageView v;
  dkImageViewDefaults(&v, &imagen.imagen);
  Swizzle(swizzle_guest, swizzle_host, v.swizzle);
  if (imagen.profundidad) {
    v.dsSource = DkDsSource_Depth;
  }
  EscribirDescriptorImagen(ranura, v);
  vistas_.emplace(clave, ranura);
  vistas_por_serie_[imagen.serie].push_back(clave);
  return ranura;
}

uint32_t RenderDk::VaciaDe(uint16_t tipo_shader) const {
  return TipoVolumen(tipo_shader) ? 1 : TipoCubo(tipo_shader) ? 2 : 0;
}

bool RenderDk::CrearVacias() {
  static constexpr DkImageType kTipos[3] = {DkImageType_2D, DkImageType_3D, DkImageType_Cubemap};
  for (uint32_t i = 0; i < 3; ++i) {
    ImagenDk& v = vacias_[i];
    if (!CrearImagen(v, kTipos[i], DkImageFormat_RGBA8_Unorm, 1, 1, 1, 1, 0, "textura vacia")) {
      return false;
    }
    const deko::Subida s = Reservar(4 * 6, 256);
    if (!s) {
      return false;
    }
    std::memset(s.cpu, 0, 4 * 6);
    DkImageView vista;
    dkImageViewDefaults(&vista, &v.imagen);
    const DkCopyBuf origen = {s.gpu, 4, 4};
    const DkImageRect rect = {0, 0, 0, 1, 1, i == 2 ? 6u : 1u};
    dkCmdBufCopyBufferToImage(nucleo_.ComandosPrevios(), &origen, &vista, &rect, 0);
    EscribirDescriptorImagen(i, vista);
  }
  // Sampler 0: bilinear, clamped (the output pass).
  DkSampler s;
  dkSamplerDefaults(&s);
  s.minFilter = s.magFilter = DkFilter_Linear;
  s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  dkCmdBufPushData(nucleo_.ComandosPrevios(), nucleo_.DescriptoresSampler(), &d, sizeof(d));
  siguiente_descriptor_ = kDescriptoresReservados;
  return true;
}

// The sampler of a fetch constant, as the Vulkan backend builds it (RanuraSampler), without its measurement tests.
// `puntual`: no filtering (depth textures).
uint32_t RenderDk::SamplerDe(const uint32_t* f, bool puntual) {
  const uint32_t campo_filtros = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
  const uint32_t campo_mips = (f[4] >> 2) & 0xFF;       // minimum in 0-3, maximum in 4-7
  const uint32_t clave = ((f[0] >> 10) & 0x1FF) | (campo_filtros << 9) | (campo_mips << 21) | ((f[5] & 0x3) << 29) |
                         (puntual ? 0x80000000u : 0u);
  if (const auto it = samplers_.find(clave); it != samplers_.end()) {
    return it->second;
  }
  if (siguiente_sampler_ >= nucleo_.config().descriptores_sampler) {
    Avisar(37, "tabla de samplers llena");
    return 0;
  }
  // The Vulkan backend's address modes: the halfway clamps as edge clamps, the mirrored ones as mirror-to-edge.
  static constexpr DkWrapMode kModos[8] = {
      DkWrapMode_Repeat,      DkWrapMode_MirroredRepeat,    DkWrapMode_ClampToEdge,   DkWrapMode_MirrorClampToEdge,
      DkWrapMode_ClampToEdge, DkWrapMode_MirrorClampToEdge, DkWrapMode_ClampToBorder, DkWrapMode_MirrorClampToEdge};
  DkSampler s;
  dkSamplerDefaults(&s);
  s.magFilter = ((f[3] >> 19) & 0x3) == 1 && !puntual ? DkFilter_Linear : DkFilter_Nearest;
  s.minFilter = ((f[3] >> 21) & 0x3) == 1 && !puntual ? DkFilter_Linear : DkFilter_Nearest;
  const uint32_t mip = (campo_filtros >> 4) & 0x3;
  s.mipFilter = mip == 1 && !puntual ? DkMipFilter_Linear : DkMipFilter_Nearest;
  s.wrapMode[0] = kModos[(f[0] >> 10) & 0x7];
  s.wrapMode[1] = kModos[(f[0] >> 13) & 0x7];
  s.wrapMode[2] = kModos[(f[0] >> 16) & 0x7];
  s.lodClampMin = float(campo_mips & 0xF);
  // With real mips, the fetch constant's maximum level limits too; a basemap filter stays on the base level.
  s.lodClampMax = mip == 2 ? s.lodClampMin + 0.25f : std::max(s.lodClampMin, float((f[4] >> 6) & 0xF));
  if ((f[5] & 0x3) == 1) {  // opaque white; otherwise transparent black
    for (auto& c : s.borderColor) {
      c.value_f = 1.0f;
    }
  }
  static const uint32_t aniso = uint32_t(std::max(0, int32_t(REXCVAR_GET(nfsmw_nativo_anisotropico))));
  if (aniso > 1 && s.minFilter == DkFilter_Linear && s.mipFilter == DkMipFilter_Linear &&
      s.lodClampMax > s.lodClampMin) {
    s.maxAnisotropy = float(std::min(aniso, 16u));
  }
  const uint32_t ranura = siguiente_sampler_++;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  AsegurarTrabajo();
  dkCmdBufPushData(nucleo_.ComandosPrevios(), nucleo_.DescriptoresSampler() + DkGpuAddr(ranura) * sizeof(d), &d,
                   sizeof(d));
  samplers_.emplace(clave, ranura);
  return ranura;
}

// ---- Untiling (the Vulkan backend's LeerNivel and LeerVolumen, with the fast untiling always on: it was checked
// against the block-by-block one on the PC and the console)

bool RenderDk::LeerNivel(uint64_t direccion, bool mosaico, uint32_t pitch_bloques, uint64_t fila_bytes,
                         const FormatoDk& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                         uint8_t* destino) {
  if (!bx || !by) {
    return true;
  }
  const uint32_t log2 = Log2Bytes(tf.bytes);
  const uint64_t extremo =
      mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((ox + bx + 31) & ~31u),
                                                                   int32_t((oy + by + 31) & ~31u), pitch_bloques, log2),
                                           0))
              : fila_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
  if (direccion + extremo > kMemoriaFisica) {
    return false;
  }
  const uint8_t* origen = memoria_->TranslatePhysical(uint32_t(direccion));
  if (!mosaico) {
    for (uint32_t y = 0; y < by; ++y) {
      std::memcpy(destino + size_t(y) * bx_host * tf.bytes,
                  origen + uint64_t(oy + y) * fila_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
    }
    return true;
  }
  if (tf.bytes == (1u << log2)) {
    switch (log2) {
      case 0: DesenmosaicarNivel<0>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 1: DesenmosaicarNivel<1>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 2: DesenmosaicarNivel<2>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 3: DesenmosaicarNivel<3>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      default: DesenmosaicarNivel<4>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
    }
    return true;
  }
  LeerNivelMosaicoDeSiempre(origen, pitch_bloques, log2, tf.bytes, ox, oy, bx, by, bx_host, destino);
  return true;
}

bool RenderDk::LeerVolumen(const uint32_t* f, const FormatoDk& tf, uint32_t bloques_x, uint32_t bloques_y,
                           uint32_t fondo, uint32_t bloques_x_host, uint32_t bloques_y_host, uint32_t pitch_bloques,
                           bool mosaico, uint8_t* datos) {
  const uint64_t direccion = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
  const uint32_t log2 = Log2Bytes(tf.bytes);
  const uint64_t fila = uint64_t((pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes;
  const uint64_t corte = fila * ((bloques_y + 31) & ~uint32_t(31));
  const uint64_t extremo =
      mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico3D(int32_t((bloques_x + 31) & ~31u),
                                                                   int32_t((bloques_y + 31) & ~31u),
                                                                   int32_t((fondo + 3) & ~3u), pitch_bloques, bloques_y,
                                                                   log2),
                                           0))
              : corte * fondo;
  if (direccion + extremo > kMemoriaFisica) {
    return false;
  }
  const uint8_t* origen = memoria_->TranslatePhysical(uint32_t(direccion));
  const size_t bytes_corte = size_t(bloques_x_host) * bloques_y_host * tf.bytes;
  for (uint32_t z = 0; z < fondo; ++z) {
    for (uint32_t y = 0; y < bloques_y; ++y) {
      for (uint32_t x = 0; x < bloques_x; ++x) {
        const uint64_t desplazamiento =
            mosaico ? uint64_t(DesplazamientoMosaico3D(int32_t(x), int32_t(y), int32_t(z), pitch_bloques, bloques_y, log2))
                    : z * corte + y * fila + uint64_t(x) * tf.bytes;
        std::memcpy(datos + bytes_corte * z + (size_t(y) * bloques_x_host + x) * tf.bytes, origen + desplazamiento,
                    tf.bytes);
      }
    }
  }
  return true;
}

// ---- The texture of a fetch constant

uint32_t RenderDk::PrepararTextura(const uint32_t* f, uint16_t tipo_shader, uint32_t& ancho_out, uint32_t& alto_out,
                                   bool& puntual, uint64_t& valido_hasta) {
  ancho_out = 0;  // 0 = unknown (1/size is not written)
  alto_out = 0;
  puntual = false;
  valido_hasta = fotograma_;
  if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
    return VaciaDe(tipo_shader);
  }
  const uint32_t dimension = (f[5] >> 9) & 0x3;
  const bool cubo = dimension == uint32_t(xenos::DataDimension::kCube);
  const bool volumen = dimension == uint32_t(xenos::DataDimension::k3D);
  if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cubo && !volumen) {
    Avisar(30, "texturas 1D todavia no soportadas: se usa una vacia");
    return VaciaDe(tipo_shader);
  }
  if (TipoVolumen(tipo_shader) != volumen || TipoCubo(tipo_shader) != cubo) {
    Avisar(29, "la dimension de la textura no es la que declara el shader: se usa una vacia");
    return VaciaDe(tipo_shader);
  }
  if ((f[0] >> 2) & 0xFF) {
    Avisar(31, "texturas con signo o gamma: se leen sin signo");
  }
  const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
  const uint32_t base = (f[1] >> 12) << 12;
  if (!cubo && !volumen) {
    if (const ImagenDk* resuelta = TexturaResuelta(base & 0x1FFFFFFF)) {
      // Resolved textures report their size like any other (without it the shadow PCF taps collapse).
      ancho_out = resuelta->ancho;
      alto_out = resuelta->alto;
      if (resuelta->profundidad) {
        // A depth copy (k_24_8): depth in R, which the fetch swizzle distributes; read unfiltered.
        puntual = true;
        return VistaDe(*resuelta, swizzle, kSwizzleRRRR);
      }
      // The emulation's copy swaps channels in memory (copy_dest_swap) and the fetch swizzle undoes it; here the copy
      // is image to image, so the swap goes into the host channels.
      return VistaDe(*resuelta, swizzle, resuelta->intercambio_rb ? kSwizzleBGRA : kSwizzleRGBA);
    }
  }
  const uint32_t formato = f[1] & 0x3F;
  FormatoDk tf;
  if (!FormatoDe(formato, tf)) {
    Avisar(400 + formato, "formato de textura todavia no soportado: se usa una vacia");
    return VaciaDe(tipo_shader);
  }
  // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h).
  const uint32_t ancho = volumen ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
  const uint32_t alto = volumen ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t fondo = volumen ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
  if (volumen && tf.bloque > 1) {
    Avisar(38, "textura 3D comprimida: se usa una vacia");
    return VaciaDe(tipo_shader);
  }
  const uint32_t capas = cubo ? 6 : 1;
  const uint32_t bloques_x = (ancho + tf.bloque - 1) / tf.bloque;
  const uint32_t bloques_y = (alto + tf.bloque - 1) / tf.bloque;
  const uint32_t pitch_bloques = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.bloque, 1);
  // Cubemap faces are consecutive: each takes its base level with rows and columns aligned to 32 blocks and the
  // total to 4 KB (GetGuestTextureLayout).
  const uint64_t zancada_cara =
      (uint64_t((pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes * ((bloques_y + 31) & ~uint32_t(31)) + 4095) &
      ~uint64_t(4095);
  const uint32_t claves[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
  const uint64_t clave = XXH3_64bits(claves, sizeof(claves));

  // Mip levels with the rules of GetSubresourcesFromFetchConstant: no mip address, no mips; the maximum clamped to the
  // size; without the base or with a minimum above 0, the base is not read. 3D textures: base only.
  static const bool mipmaps = REXCVAR_GET(nfsmw_nativo_mipmaps);
  const bool mosaico = (f[0] >> 31) & 0x1;
  const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
  const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
  const uint32_t nivel_empaquetado =
      mipmaps && !volumen && ((f[5] >> 11) & 0x1) ? NivelEmpaquetado(ancho, alto) : UINT32_MAX;
  uint32_t nivel_max = 0;
  bool leer_base = true;
  if (mipmaps && !volumen && dir_mips != 0) {
    const uint32_t tam_max = Log2Suelo(std::max(ancho, alto));
    uint32_t nivel_min = std::min((f[4] >> 2) & 0xFu, tam_max);
    nivel_max = std::max(std::min((f[4] >> 6) & 0xFu, tam_max), nivel_min);
    if (nivel_max != 0) {
      if (dir_base == 0) {
        nivel_min = std::max(nivel_min, 1u);
      }
      leer_base = nivel_min == 0;
    }
  }
  // Mip regions from dir_mips (GetGuestTextureLayout): rows of 32 blocks from the size rounded up to a power of 2
  // (and 256-byte rows if linear), layers aligned to 4 KB; the packed tail's levels share its first region.
  struct RegionMip {
    uint64_t desplazamiento = 0;
    uint64_t fila_bytes = 0;
    uint64_t zancada = 0;
    uint32_t pitch_bloques = 0;
  };
  std::array<RegionMip, 16> regiones{};
  uint64_t extension_mips = 0;
  if (nivel_max != 0) {
    const uint32_t ultima = nivel_empaquetado == 0 ? 0 : std::min(nivel_max, nivel_empaquetado);
    for (uint32_t s = nivel_empaquetado == 0 ? 0 : 1; s <= ultima; ++s) {
      RegionMip& region = regiones[s];
      const uint32_t fila_texels = std::max(std::bit_ceil(ancho) >> s, 1u);
      const uint32_t filas_texels = std::max(std::bit_ceil(alto) >> s, 1u);
      region.pitch_bloques = ((fila_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u;
      region.fila_bytes = uint64_t(region.pitch_bloques) * tf.bytes;
      if (!mosaico) {
        region.fila_bytes = (region.fila_bytes + 255) & ~uint64_t(255);
      }
      const uint64_t filas_bloques = uint64_t(((filas_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u);
      region.zancada = (region.fila_bytes * filas_bloques + 4095) & ~uint64_t(4095);
      region.desplazamiento = extension_mips;
      extension_mips += region.zancada * capas;
    }
    if (dir_mips + extension_mips > kMemoriaFisica) {
      nivel_max = 0;  // the mips would go past memory: base only
      extension_mips = 0;
      leer_base = true;
    }
  }
  // With the packed tail from level 0 (short side of 16 texels or less), the base does not start at its address
  // either.
  uint32_t base_ox = 0;
  uint32_t base_oy = 0;
  if (nivel_empaquetado == 0) {
    DesplazamientoEmpaquetado(ancho, alto, tf.bloque, 0, base_ox, base_oy);
  }

  // The game's dynamic cubemaps (the car's environment, the rear-view mirror's face): their faces are resolves to
  // consecutive addresses, so the guest memory never holds them. Each face is copied from its resolved texture into
  // the cube's layer once per frame, at the first draw that samples it (the Vulkan path does the same in its upload
  // command buffer, a frame later).
  if (cubo) {
    std::array<const ImagenDk*, 6> caras{};
    bool resueltas = true;
    for (uint32_t c = 0; c < 6 && resueltas; ++c) {
      caras[c] = TexturaResuelta(uint32_t((dir_base + c * zancada_cara) & 0x1FFFFFFF));
      resueltas = caras[c] && !caras[c]->profundidad && caras[c]->formato == DkImageFormat_RGBA8_Unorm &&
                  caras[c]->ancho >= ancho && caras[c]->alto >= alto;
    }
    if (resueltas) {
      TexturaDk& cubo_resuelto = texturas_[clave ^ 0x9E3779B97F4A7C15ull];
      if (!cubo_resuelto.imagen.viva) {
        if (!CrearImagen(cubo_resuelto.imagen, DkImageType_Cubemap, DkImageFormat_RGBA8_Unorm, ancho, alto, 1, 1, 0,
                         "cubo de caras resueltas")) {
          texturas_.erase(clave ^ 0x9E3779B97F4A7C15ull);
          Avisar(32, "no se pudo crear una textura");
          return VaciaDe(tipo_shader);
        }
        cubo_resuelto.bytes = uint64_t(ancho) * alto * 4 * 6;
        bytes_texturas_ += cubo_resuelto.bytes;
        ++texturas_creadas_;
        REXLOG_INFO("[deko3d] cubo {:08X} {}x{} con sus caras resueltas", base, ancho, alto);
      }
      if (cubo_resuelto.fotograma != fotograma_) {
        cubo_resuelto.fotograma = fotograma_;
        // Only the faces resolved since the last refresh (the game redraws one face per frame or fewer). Transfers
        // on the copy engine: no 3D state changes, so the pass and a deferred sky stay as they are.
        bool alguna = false;
        DkImageView destino;
        dkImageViewDefaults(&destino, &cubo_resuelto.imagen.imagen);
        for (uint32_t c = 0; c < 6; ++c) {
          if (cubo_resuelto.caras_version[c] == caras[c]->version + 1) {
            continue;  // stored as version + 1, so a new cube (all 0) copies every face
          }
          cubo_resuelto.caras_version[c] = caras[c]->version + 1;
          if (!alguna) {
            alguna = true;
            MarcarGpu(kGpuCopias);
            AntesDeTransferir();
          }
          DkImageView origen;
          dkImageViewDefaults(&origen, &caras[c]->imagen);
          const DkImageRect a = {0, 0, 0, ancho, alto, 1};
          const DkImageRect b = {0, 0, c, ancho, alto, 1};
          dkCmdBufCopyImage(nucleo_.comandos(), &origen, &a, &destino, &b, 0);
        }
        if (alguna) {
          ++cubos_refrescados_;
          if (g_.pase_valido) {
            MarcarGpu(categoria_pase_);
          }
        }
      }
      ancho_out = cubo_resuelto.imagen.ancho;
      alto_out = cubo_resuelto.imagen.alto;
      return VistaDe(cubo_resuelto.imagen, swizzle, caras[0]->intercambio_rb ? kSwizzleBGRA : kSwizzleRGBA);
    }
  }

  TexturaDk& textura = texturas_[clave];
  if (!textura.imagen.viva) {
    // BC: the size in whole blocks, as the Vulkan backend (the shaders' 1/size is the host size).
    const uint32_t ancho_host = tf.bloque > 1 ? (ancho + 3) & ~uint32_t(3) : ancho;
    const uint32_t alto_host = tf.bloque > 1 ? (alto + 3) & ~uint32_t(3) : alto;
    const uint32_t niveles = std::min(nivel_max + 1, Log2Suelo(std::max(ancho_host, alto_host)) + 1);
    if (!CrearImagen(textura.imagen, cubo ? DkImageType_Cubemap : volumen ? DkImageType_3D : DkImageType_2D,
                     tf.formato, ancho_host, alto_host, volumen ? fondo : 1, niveles, 0, "textura")) {
      texturas_.erase(clave);
      Avisar(32, "no se pudo crear una textura");
      return VaciaDe(tipo_shader);
    }
    for (uint32_t n = 0; n < niveles; ++n) {
      textura.bytes += uint64_t((std::max(ancho_host >> n, 1u) + tf.bloque - 1) / tf.bloque) *
                       ((std::max(alto_host >> n, 1u) + tf.bloque - 1) / tf.bloque) * tf.bytes * capas *
                       (fondo ? fondo : 1);
    }
    bytes_texturas_ += textura.bytes;
    ++texturas_creadas_;
    if (texturas_creadas_ <= 48) {
      REXLOG_INFO("[deko3d] textura {:08X} {}x{} formato {} {} orden {} pitch {} swizzle {:03X}{}; niveles {}", base,
                  ancho, alto, formato, mosaico ? "en mosaico" : "lineal", (f[1] >> 6) & 0x3,
                  ((f[0] >> 22) & 0x1FF) << 5, swizzle, cubo ? " (cubo)" : volumen ? " (3D)" : "", niveles);
    }
  }
  const uint32_t ranura = VistaDe(textura.imagen, swizzle, tf.swizzle_host);
  // Until its next check nothing about it changes: the draws' per-fetch cache keeps the answer that long.
  auto listo = [&]() {
    valido_hasta = textura.siguiente > fotograma_ ? textura.siguiente - 1 : fotograma_;
    return ranura;
  };
  ancho_out = textura.imagen.ancho;
  alto_out = textura.imagen.alto;
  if (textura.fotograma == fotograma_) {
    return listo();  // already checked this frame
  }
  if (textura.con_contenido && fotograma_ < textura.siguiente) {
    return listo();
  }
  textura.fotograma = fotograma_;
  // 2D and cubemaps: the guest bytes are fingerprinted before untiling; unchanged, nothing to do.
  if (!volumen) {
    const uint32_t log2_bloque = Log2Bytes(tf.bytes);
    const uint32_t bloques_x_leidos = bloques_x + base_ox;
    const uint32_t bloques_y_leidos = bloques_y + base_oy;
    const uint64_t extension_capa =
        mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((bloques_x_leidos - 1) & ~31u),
                                                                     int32_t((bloques_y_leidos - 1) & ~31u),
                                                                     pitch_bloques, log2_bloque),
                                             0)) +
                      (log2_bloque == 0 ? 0xA00u : log2_bloque == 1 ? 0xC00u : (0x400u << log2_bloque))
                : uint64_t(std::max(pitch_bloques, bloques_x_leidos)) * tf.bytes * (bloques_y_leidos - 1) +
                      uint64_t(bloques_x_leidos) * tf.bytes;
    const uint64_t extension = (capas - 1) * zancada_cara + extension_capa;
    if (dir_base + extension <= kMemoriaFisica) {
      // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
      auto sin_cambios = [&] {
        textura.intervalo = std::min<uint32_t>(textura.intervalo * 2, 32);
        textura.siguiente = fotograma_ + textura.intervalo + (textura.intervalo == 32 ? (clave >> 7) & 7 : 0);
        return listo();
      };
      if (muestreo_huellas_ < 0) {
        const int32_t cada = REXCVAR_GET(nfsmw_nativo_huellas_muestreo);
        muestreo_huellas_ = cada >= 2 ? std::min<int32_t>(cada, 64) : 0;
        presupuesto_huellas_ = uint64_t(std::max(REXCVAR_GET(nfsmw_nativo_huellas_kb_fotograma), 0)) * 1024;
        REXLOG_INFO("[deko3d] texturas estables: recomprobacion por muestreo {}; presupuesto {} KB por fotograma",
                    muestreo_huellas_ ? fmt::format("SI, 1 de cada {} completa, las {} primeras con las dos huellas",
                                                    muestreo_huellas_, kMuestrasAComprobar)
                                      : std::string("no"),
                    presupuesto_huellas_ >> 10);
      }
      // The sampled recheck (the Vulkan path's C3, nfsmw_nativo_huellas_muestreo): a stable texture (interval 8 or
      // more) whose sample matches is taken as unchanged; 1 in N of its rechecks is still whole, and the first
      // kMuestrasAComprobar compute both hashes (one disagreement switches sampling off). Not for textures whose
      // sample would read more than half of their bytes.
      const uint64_t bytes_completa = extension + extension_mips;
      const uint64_t bytes_de_muestra = BytesMuestra(extension) + BytesMuestra(extension_mips);
      const bool con_muestra = muestreo_huellas_ > 0 && textura.con_contenido && textura.intervalo >= 4 &&
                               bytes_de_muestra * 2 <= bytes_completa &&
                               (bases_sin_muestra_.empty() || !bases_sin_muestra_.count(uint32_t(base)));
      const bool muestra_util = con_muestra && textura.intervalo >= 8 && textura.muestra_valida;
      const bool solo_muestra = muestra_util && muestras_comprobadas_ >= kMuestrasAComprobar &&
                                textura.muestras_seguidas + 1u < uint32_t(muestreo_huellas_);
      const uint64_t bytes_huella = solo_muestra ? bytes_de_muestra : bytes_completa + (con_muestra ? bytes_de_muestra : 0);
      // The per-frame budget: textures that doubled their interval together (one zone) are rechecked in the same
      // frame; a stable one that does not fit waits 1-2 frames (kAplazamientoMax times in a row at most). New and
      // changing textures never wait.
      if (fotograma_huellas_ != fotograma_) {
        fotograma_huellas_ = fotograma_;
        bytes_huella_fotograma_ = 0;
      }
      if (presupuesto_huellas_ && textura.con_contenido && textura.intervalo >= 8 &&
          textura.aplazamientos < kAplazamientoMax && bytes_huella_fotograma_ > 0 &&
          bytes_huella_fotograma_ + bytes_huella > presupuesto_huellas_) {
        ++textura.aplazamientos;
        ++huellas_aplazadas_;
        textura.siguiente = fotograma_ + 1 + (clave & 1);
        return listo();
      }
      textura.aplazamientos = 0;
      bytes_huella_fotograma_ += bytes_huella;
      bytes_huella_ += bytes_huella;
      const uint8_t* const crudo_base = memoria_->TranslatePhysical(uint32_t(dir_base));
      const uint8_t* const crudo_mips = extension_mips ? memoria_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
      uint64_t muestra = 0;
      if (con_muestra) {
        muestra = HuellaMuestra(crudo_base, extension, 0);
        if (extension_mips) {
          muestra = HuellaMuestra(crudo_mips, extension_mips, muestra);
        }
      }
      const bool muestra_igual = muestra_util && muestra == textura.huella_muestra;
      if (solo_muestra && muestra_igual) {
        ++textura.muestras_seguidas;
        ++muestras_aciertos_;
        bytes_ahorrados_muestra_ += bytes_completa - bytes_de_muestra;
        return sin_cambios();
      }
      if (solo_muestra) {
        ++muestras_distintas_;  // the sample changed: now the whole hash
        bytes_huella_fotograma_ += bytes_completa;
        bytes_huella_ += bytes_completa;
      }
      uint64_t huella = XXH3_64bits(crudo_base, size_t(extension));
      if (extension_mips) {
        huella = XXH3_64bits_withSeed(crudo_mips, size_t(extension_mips), huella);
      }
      const bool completa_igual = textura.con_contenido && huella == textura.huella_cruda;
      if (muestra_util && !solo_muestra) {
        // The guard: this stable recheck computed both hashes.
        ++muestras_comprobadas_;
        if (muestra_igual && !completa_igual) {
          // A texture the game changes where the sample does not look (runs 9 and 10: the same 1024x1024 DXT1 at
          // 0CB7D000). That address is rechecked whole from now on and the guard starts again for the rest; a
          // fifth such texture switches sampling off for all.
          const bool apagar = bases_sin_muestra_.size() >= kBasesSinMuestraMax;
          REXLOG_WARN("[deko3d] la MUESTRA de la textura {:08X} {}x{} formato {} ({} KB de base y {} KB de mips) no "
                      "vio un cambio que la huella completa si (tras {} recomprobaciones con las dos): {}",
                      base, ancho, alto, formato, extension >> 10, extension_mips >> 10, muestras_comprobadas_,
                      apagar ? "se APAGA el muestreo de huellas"
                             : "esa direccion se comprueba siempre entera y la guardia vuelve a empezar");
          if (apagar) {
            muestreo_huellas_ = 0;
          } else {
            bases_sin_muestra_.insert(uint32_t(base));
            muestras_comprobadas_ = 0;
          }
          textura.muestra_valida = false;
        } else if (muestras_comprobadas_ == kMuestrasAComprobar) {
          REXLOG_INFO("[deko3d] muestreo de huellas: {} recomprobaciones con las dos huellas, ningun desacuerdo; sigue "
                      "con 1 de cada {} completa",
                      muestras_comprobadas_, muestreo_huellas_);
        }
      }
      if (completa_igual) {
        if (con_muestra && muestreo_huellas_ > 0) {
          textura.huella_muestra = muestra;
          textura.muestra_valida = true;
        }
        textura.muestras_seguidas = 0;
        return sin_cambios();
      }
      textura.muestra_valida = false;  // new content: its sample is taken at the next whole check
      textura.muestras_seguidas = 0;
      textura.huella_cruda = huella;
    }
  }
  // Every level, layer after layer, untiled and in host byte order.
  const ImagenDk& img = textura.imagen;
  std::array<size_t, 16> desplazamiento_nivel{};
  std::array<size_t, 16> bytes_capa_nivel{};
  size_t bytes_datos = 0;
  for (uint32_t n = 0; n < img.niveles; ++n) {
    const uint32_t bx_host = (std::max(img.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
    const uint32_t by_host = (std::max(img.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
    desplazamiento_nivel[n] = bytes_datos;
    bytes_capa_nivel[n] = size_t(bx_host) * by_host * tf.bytes;
    bytes_datos += bytes_capa_nivel[n] * capas * (fondo ? fondo : 1);
  }
  temporal_.assign(bytes_datos, 0);
  const uint32_t bloques_x_host = (img.ancho + tf.bloque - 1) / tf.bloque;
  const uint32_t bloques_y_host = (img.alto + tf.bloque - 1) / tf.bloque;
  if (volumen) {
    if (!LeerVolumen(f, tf, bloques_x, bloques_y, fondo, bloques_x_host, bloques_y_host, pitch_bloques, mosaico,
                     temporal_.data())) {
      Avisar(33, "textura fuera de la memoria");
      return textura.con_contenido ? ranura : VaciaDe(tipo_shader);
    }
    // 3D textures have no raw fingerprint: the untiled data decides.
    const uint64_t huella = XXH3_64bits(temporal_.data(), temporal_.size());
    if (textura.con_contenido && huella == textura.huella_cruda) {
      textura.intervalo = std::min<uint32_t>(textura.intervalo * 2, 32);
      textura.siguiente = fotograma_ + textura.intervalo;
      return listo();
    }
    textura.huella_cruda = huella;
  } else {
    for (uint32_t c = 0; c < (leer_base ? capas : 0u); ++c) {
      if (!LeerNivel(dir_base + c * zancada_cara, mosaico, pitch_bloques,
                     uint64_t(std::max(pitch_bloques, bloques_x)) * tf.bytes, tf, base_ox, base_oy, bloques_x, bloques_y,
                     bloques_x_host, temporal_.data() + bytes_capa_nivel[0] * c)) {
        Avisar(33, "textura fuera de la memoria");
        return textura.con_contenido ? ranura : VaciaDe(tipo_shader);
      }
    }
    for (uint32_t n = 1; n < img.niveles; ++n) {
      const uint32_t s = nivel_empaquetado == 0 ? 0 : std::min(n, nivel_empaquetado);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= nivel_empaquetado) {
        DesplazamientoEmpaquetado(ancho, alto, tf.bloque, n, ox, oy);
      }
      const uint32_t bx_invitado = (std::max(ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_invitado = (std::max(alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t bx_host = (std::max(img.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_host = (std::max(img.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      bool fuera = false;
      for (uint32_t c = 0; c < capas && !fuera; ++c) {
        fuera = !LeerNivel(dir_mips + regiones[s].desplazamiento + c * regiones[s].zancada, mosaico,
                           regiones[s].pitch_bloques, regiones[s].fila_bytes, tf, ox, oy, std::min(bx_invitado, bx_host),
                           std::min(by_invitado, by_host), bx_host,
                           temporal_.data() + desplazamiento_nivel[n] + bytes_capa_nivel[n] * c);
      }
      if (fuera) {
        Avisar(39, "nivel de mip fuera de la memoria: se queda a cero");
        break;
      }
    }
  }
  const uint32_t orden = (f[1] >> 6) & 0x3;
  if (orden && (tf.unidad_orden == 2 || tf.unidad_orden == 4)) {
    CambiarOrdenBytes(temporal_.data(), temporal_.size(), tf.unidad_orden, orden);
  }
  // To the upload ring (it may move on to the next slot: the copies and the descriptors go with the slot they were
  // recorded in, and this texture's draws come after).
  const deko::Subida subida = Reservar(uint32_t(bytes_datos), 256);
  if (!subida) {
    Avisar(13, "textura mayor que el anillo de subida");
    return textura.con_contenido ? ranura : VaciaDe(tipo_shader);
  }
  std::memcpy(subida.cpu, temporal_.data(), bytes_datos);
  DkCmdBuf previos = nucleo_.ComandosPrevios();
  for (uint32_t n = 0; n < img.niveles; ++n) {
    const uint32_t bx_host = (std::max(img.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
    const uint32_t by_host = (std::max(img.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
    DkImageView vista;
    dkImageViewDefaults(&vista, &img.imagen);
    vista.mipLevelOffset = uint8_t(n);
    // Layers (cube faces, 3D slices) by the rectangle's z; rows and layers in bytes of the upload.
    const uint32_t fila = bx_host * tf.bytes;
    const DkCopyBuf origen = {subida.gpu + desplazamiento_nivel[n], fila, fila * by_host};
    const DkImageRect rect = {0, 0, 0, std::max(img.ancho >> n, 1u), std::max(img.alto >> n, 1u),
                              volumen ? fondo : capas};
    dkCmdBufCopyBufferToImage(previos, &origen, &vista, &rect, 0);
  }
  textura.con_contenido = true;
  textura.intervalo = 1;
  textura.siguiente = fotograma_ + 1;
  ++texturas_subidas_;
  bytes_subidos_ += bytes_datos;
  return listo();
}

// Above the limit, textures not checked for kFotogramasSinUsoParaSoltar frames are released until the cache is
// back under 75 % (a texture in use is checked at least every 39 frames). The game streams the city into new
// addresses: without a limit the cache grew ~20 MB every 40 s of racing (docs/native-renderer.md).
void RenderDk::SoltarTexturasSiHaceFalta() {
  const uint64_t limite = uint64_t(REXCVAR_GET(nfsmw_nativo_texturas_mb_max)) << 20;
  if (bytes_texturas_ <= limite) {
    return;
  }
  const uint64_t objetivo = limite / 4 * 3;
  uint64_t soltadas = 0, bytes = 0;
  for (auto it = texturas_.begin(); it != texturas_.end() && bytes_texturas_ > objetivo;) {
    TexturaDk& t = it->second;
    if (t.fotograma != UINT64_MAX && t.fotograma + kFotogramasSinUsoParaSoltar < fotograma_) {
      bytes_texturas_ -= t.bytes;
      bytes += t.bytes;
      ++soltadas;
      DestruirImagen(t.imagen);
      it = texturas_.erase(it);
    } else {
      ++it;
    }
  }
  if (soltadas) {
    ++generacion_texturas_;  // the per-fetch cache may name them
    REXLOG_INFO("[deko3d] texturas: {} soltadas ({} MB); quedan {} ({} MB)", soltadas, bytes >> 20, texturas_.size(),
                bytes_texturas_ >> 20);
  }
}

}  // namespace nfsmw::nativo

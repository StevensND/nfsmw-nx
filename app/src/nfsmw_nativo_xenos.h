// nfsmw - native renderer: the Xbox 360 GPU's arithmetic that does not depend on the graphics API, shared by the
// Vulkan backend (nfsmw_nativo_dibujos.cpp) and the deko3d one (nfsmw_dk_*.cpp): register offsets, the tiled
// address of a texel (2D and 3D), the mip level layout with its packed tail, untiling and byte swaps, the
// vertex declaration usages and the fetch swizzle remap, and a pass's GPU time category. Moved out of
// nfsmw_nativo_dibujos.cpp as it was; each function is a copy of the emulation's code it names.

#pragma once

#include <rex/graphics/xenos.h>

#include <xxhash.h>

#include "nfsmw_nativo_peticion.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace nfsmw::nativo {

/*
 * A pass's category from its render target, in one place.
 *
 * It used to be decided by width: "1600 or more" meant shadows. With the scene at 1920
 * (nfsmw_1080p_prueba) that no longer tells them apart and the scene was counted as shadows, so its
 * category vanished from the report. The shadow map is recognized for what it is: a depth-only target, with
 * no color at all.
 */
// The scene's width: 1280, or the smaller one of nfsmw_resolucion_interna = escena_WxH (set once at startup by
// nfsmw_render_targets.cpp, before any pass). The sky deferral recognizes the scene by it.
inline std::atomic<uint32_t> g_ancho_escena{1280};

inline uint32_t CategoriaDeDestino(uint32_t pitch, const uint64_t* claves) {
  const bool color = claves[0] || claves[1] || claves[2] || claves[3];
  if (!color && claves[4] && pitch >= 1600) {
    return kGpuSombras;
  }
  if (pitch >= std::min<uint32_t>(1280, g_ancho_escena.load(std::memory_order_relaxed))) {
    return claves[4] ? kGpuEscena : kGpuEscenaSinProfundidad;
  }
  if (pitch >= 640) {
    return kGpuReflejo;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuMenores;
}

inline constexpr uint32_t kRegConstantesVs = 0x4000;
inline constexpr uint32_t kRegConstantesPs = 0x4400;
inline constexpr uint32_t kRegFetch = 0x4800;
inline constexpr uint32_t kRegBooleanos = 0x4900;
inline constexpr uint32_t kRegistrosConstantes = 0x400;  // 256 constantes x 4

inline constexpr uint32_t kRemapeoIdentidad = 0xFFF;

inline constexpr uint32_t kMemoriaFisica = 0x20000000;

// One guest vertex binding into the upload buffer, as host words with the fetch constant's byte order.
// Done by the ring thread or by the copy thread (nfsmw_nativo_subidas_hilo).
struct TrabajoCopia {
  const uint8_t* origen = nullptr;
  uint8_t* destino = nullptr;
  uint32_t palabras = 0;
  rex::graphics::xenos::Endian orden = rex::graphics::xenos::Endian::kNone;
};

inline void CopiarVertices(const TrabajoCopia& t) {
  // The fields go into local variables: the destination is a byte pointer and, as far as the compiler
  // knows, writing through it could change the structure itself. With t.* inside the loop it was not
  // vectorized (935 ms per 5.3 GB of vertices in a race, against 490 ms with the loop inside Dibujar).
  const uint8_t* const origen = t.origen;
  uint8_t* const destino = t.destino;
  const uint32_t palabras = t.palabras;
  const rex::graphics::xenos::Endian orden = t.orden;
  if (orden == rex::graphics::xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < palabras; ++i) {
      uint32_t v;
      std::memcpy(&v, origen + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(destino + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < palabras; ++i) {
      uint32_t v;
      std::memcpy(&v, origen + size_t(i) * 4, 4);
      v = rex::graphics::xenos::GpuSwap(v, orden);
      std::memcpy(destino + size_t(i) * 4, &v, 4);
    }
  }
}

inline float Flotante(uint32_t valor) {
  return std::bit_cast<float>(valor);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
inline uint32_t Log2Techo(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

inline uint32_t Log2Suelo(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

/*
 * The sample used to recheck a stable texture (nfsmw_nativo_huellas_muestreo). From a region of guest
 * memory, its first 4 KB block, its last one and one in every kMuestraCada are read, counted from the
 * start of the region. The base and the mips of a texture start at 4 KB-aligned addresses, so each block
 * is a whole page. That reads ~13-19 % of the bytes of textures of 64 KB or more; for textures of few
 * blocks the sample is almost everything, and PrepararTextura does not use it if it exceeds half. Tested
 * on PC: BytesMuestra matches what is read for every size from 1 byte to 300 KB, and a one-byte change is
 * seen if and only if it falls in a sampled block.
 */
inline constexpr uint64_t kBloqueMuestra = 4096;
inline constexpr uint64_t kMuestraCada = 8;

// Bytes HuellaMuestra reads in a region of that size.
inline uint64_t BytesMuestra(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t ultimo = (bytes - 1) / kBloqueMuestra;
  return (ultimo + kMuestraCada - 1) / kMuestraCada * kBloqueMuestra + (bytes - ultimo * kBloqueMuestra);
}

// Chained XXH3 of blocks 0, 8, 16... below the last one, and of the last one (which may be partial).
inline uint64_t HuellaMuestra(const uint8_t* datos, uint64_t bytes, uint64_t semilla) {
  if (!bytes) {
    return semilla;
  }
  const uint64_t ultimo = (bytes - 1) / kBloqueMuestra;
  uint64_t huella = semilla;
  for (uint64_t b = 0; b < ultimo; b += kMuestraCada) {
    huella = XXH3_64bits_withSeed(datos + b * kBloqueMuestra, size_t(kBloqueMuestra), huella);
  }
  return XXH3_64bits_withSeed(datos + ultimo * kBloqueMuestra, size_t(bytes - ultimo * kBloqueMuestra), huella);
}

// First level of the packed tail: once the short side is 16 texels or less.
inline uint32_t NivelEmpaquetado(uint32_t ancho, uint32_t alto) {
  const uint32_t l = Log2Techo(std::min(ancho, alto));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
inline void DesplazamientoEmpaquetado(uint32_t ancho, uint32_t alto, uint32_t bloque, uint32_t nivel, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_ancho = Log2Techo(ancho);
  const uint32_t l2_alto = Log2Techo(alto);
  const uint32_t l2 = std::min(l2_ancho, l2_alto);
  x = 0;
  y = 0;
  if (l2 > 4 + nivel) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = nivel - base;
  if (m < 3) {
    if (l2_ancho > l2_alto) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_ancho > l2_alto) {
    x = (1u << (l2_ancho - base)) >> (m - 2);
  } else {
    y = (1u << (l2_alto - base)) >> (m - 2);
  }
  x /= bloque;
  y /= bloque;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
inline int32_t DesplazamientoMosaico2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During race stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): LeerNivel called DesplazamientoMosaico2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as DesplazamientoMosaico2D (checked block by block on PC).
template <uint32_t kLog2>
inline void DesenmosaicarNivel(const uint8_t* origen, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* destino) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGrupo = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_fila = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t fila = 0; fila < by; ++fila) {
    const int32_t y = int32_t(oy + fila);
    const int32_t macro_y = (y >> 5) * macros_fila;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* salida = destino + size_t(fila) * bx_host * kBytes;
    uint32_t columna = 0;
    while (columna < bx) {
      const int32_t x = int32_t(ox + columna);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t desplazamiento = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGrupo > 1 && (uint32_t(x) % kGrupo) == 0 && columna + kGrupo <= bx) {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kGrupo * kBytes);
        columna += kGrupo;
      } else {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kBytes);
        ++columna;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void CambiarOrdenBytes(uint8_t* datos, size_t bytes, uint32_t unidad, uint32_t orden) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // rex::graphics::xenos::Endian
  size_t n = unidad == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unidad == 2 && orden != k8in16) || (unidad != 2 && unidad != 4) || orden == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(datos + i);
    uint8x16_t r;
    if (unidad == 2 || orden == k8in16) {
      r = vrev16q_u8(v);
    } else if (orden == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(datos + i, r);
  }
#endif
  if (unidad == 2 || orden == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(datos[i], datos[i + 1]);
    }
  } else if (orden == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 3]);
      std::swap(datos[i + 1], datos[i + 2]);
    }
  } else if (orden == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 2]);
      std::swap(datos[i + 1], datos[i + 3]);
    }
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// (graphics/pipeline/texture/util.cpp:438-459). pitch and height in blocks.
inline int32_t DesplazamientoMosaico3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t alto,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  alto = (alto + 31) & ~uint32_t(31);
  const int32_t macro_exterior = ((y >> 4) + (z >> 2) * int32_t(alto >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_exterior) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t exterior = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = exterior + ((((x >> 3) + (exterior << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t direccion = (offset1 & 1) << 3;
  direccion += (offset2 >> 6) & 7;
  direccion <<= 3;
  direccion += offset1 & ~1;
  direccion <<= 2;
  direccion += offset2 & ~511;
  direccion <<= 3;
  direccion += offset2 & 63;
  return direccion;
}

// USAGE_LOCATIONS de XenosRecomp (shader_recompiler.cpp).
inline int32_t UbicacionDeUso(uint8_t uso, uint8_t indice) {
  switch (uso) {
    case 0:  // posicion
      return indice == 0 ? 0 : (indice == 1 ? 15 : -1);
    case 3:  // normal
      return indice == 0 ? 1 : -1;
    case 6:  // tangente
      return indice == 0 ? 2 : -1;
    case 7:  // binormal
      return indice == 0 ? 3 : -1;
    case 5:  // texcoord
      return indice < 4 ? 4 + indice : (indice < 8 ? 12 + (indice - 4) : -1);
    case 10:  // color
      return indice == 0 ? 8 : (indice == 1 ? 11 : -1);
    case 2:  // indices de mezcla
      return indice == 0 ? 9 : -1;
    case 1:  // pesos de mezcla
      return indice == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
inline bool EntradaEntera(uint8_t uso) {
  // Only BLENDINDICES. Since the nfsmw_validado_normales library, normals, tangents and binormals are
  // float4: NFSMW stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen in the rear-view mirror).
  return uso == 2;
}

inline uint32_t ComponentesVertice(uint32_t formato) {
  switch (formato) {
    case 33:
    case 36:
      return 1;
    case 25:
    case 31:
    case 34:
    case 37:
      return 2;
    case 16:
    case 17:
    case 57:
      return 3;
    default:
      return 4;
  }
}

// Components written by a fetch swizzle (the ones that are not 7), as a 4-bit mask.
inline uint32_t MascaraEscrita(uint32_t swizzle) {
  uint32_t mascara = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mascara |= 1u << i;
    }
  }
  return mascara;
}

// g_InputRemap code: the SPIR-V writes r[i] = entrada[orig[i]] and the fetch patched by D3D writes
// r[i] = dato[parcheado[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from parcheado[i]. 7 = the same component.
inline uint32_t CodigoRemapeo(uint32_t original, uint32_t parcheado) {
  uint32_t codigo = kRemapeoIdentidad;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (parcheado >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      codigo = (codigo & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return codigo;
}

}  // namespace nfsmw::nativo

// nfsmw - deko3d renderer: the DKSH cache file. See nfsmw_dk_cache_dksh.h.

#include "nfsmw_dk_cache_dksh.h"

#include "nfsmw_dk_glsl.h"

#include <cstring>
// The same xxHash setup as nfsmw_shader_library.cpp (reads through memcpy: see the note there).
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nfsmw::deko {
namespace {

constexpr uint32_t kMagia = 0x4B44464Eu;  // "NFDK"
constexpr uint32_t kVersion = 2;
constexpr size_t kCabecera = 32;
constexpr size_t kEntrada = 24;
constexpr uint32_t kMaxEntradas = 1u << 16;
constexpr uint32_t kMaxDksh = 1u << 20;

uint32_t U32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t U64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
void Poner(std::vector<uint8_t>& d, size_t at, const void* v, size_t n) { std::memcpy(d.data() + at, v, n); }

uint64_t HuellaId() {
  const std::string id = IdCompilador();
  return XXH3_64bits(id.data(), id.size());
}

}  // namespace

std::string IdCompilador() {
  return "uam 1.1.0 + SwitchWakerHD 2d147aa (patches 1-7); SPIRV-Cross vulkan-sdk-1.4.363.0; GLSL revision " +
         std::to_string(kRevisionGlsl);
}

uint64_t HuellaSpirv(std::span<const uint32_t> espirv) { return XXH3_64bits(espirv.data(), espirv.size_bytes()); }

bool CacheDksh::Cargar(std::span<const uint8_t> archivo, std::string& motivo) {
  entradas_.clear();
  if (archivo.size() < kCabecera || U32(archivo.data()) != kMagia) {
    motivo = "no es una cache de DKSH";
    return false;
  }
  if (U32(archivo.data() + 4) != kVersion) {
    motivo = "otra version del formato";
    return false;
  }
  if (U64(archivo.data() + 8) != HuellaId()) {
    motivo = "hecha por otro compilador (se espera: " + IdCompilador() + ")";
    return false;
  }
  const uint32_t n = U32(archivo.data() + 16);
  if (n > kMaxEntradas || archivo.size() < kCabecera + size_t(n) * kEntrada ||
      XXH3_64bits(archivo.data() + kCabecera, archivo.size() - kCabecera) != U64(archivo.data() + 24)) {
    motivo = "danada";
    return false;
  }
  size_t datos = kCabecera + size_t(n) * kEntrada;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = archivo.data() + kCabecera + size_t(i) * kEntrada;
    const uint32_t bytes = U32(e + 16);
    if (bytes > kMaxDksh || datos + bytes > archivo.size()) {
      entradas_.clear();
      motivo = "danada";
      return false;
    }
    entradas_[{U64(e), U32(e + 8), U32(e + 12)}].assign(archivo.data() + datos, archivo.data() + datos + bytes);
    datos = (datos + bytes + 3) & ~size_t(3);
  }
  return true;
}

std::vector<uint8_t> CacheDksh::Serializar() const {
  size_t total = kCabecera + entradas_.size() * kEntrada;
  for (const auto& [clave, dksh] : entradas_) {
    total = (total + dksh.size() + 3) & ~size_t(3);
  }
  std::vector<uint8_t> d(total, 0);
  const uint32_t cabecera[2] = {kMagia, kVersion};
  Poner(d, 0, cabecera, 8);
  const uint64_t id = HuellaId();
  Poner(d, 8, &id, 8);
  const uint32_t n = uint32_t(entradas_.size());
  Poner(d, 16, &n, 4);
  size_t e = kCabecera;
  size_t datos = kCabecera + entradas_.size() * kEntrada;
  for (const auto& [clave, dksh] : entradas_) {
    const uint32_t bytes = uint32_t(dksh.size());
    Poner(d, e, &clave.huella, 8);
    Poner(d, e + 8, &clave.especializacion, 4);
    Poner(d, e + 12, &clave.variante, 4);
    Poner(d, e + 16, &bytes, 4);
    e += kEntrada;
    if (bytes) {
      Poner(d, datos, dksh.data(), bytes);
    }
    datos = (datos + bytes + 3) & ~size_t(3);
  }
  const uint64_t integridad = XXH3_64bits(d.data() + kCabecera, d.size() - kCabecera);
  Poner(d, 24, &integridad, 8);
  return d;
}

const std::vector<uint8_t>* CacheDksh::Buscar(const Clave& clave) const {
  const auto it = entradas_.find(clave);
  return it == entradas_.end() ? nullptr : &it->second;
}

void CacheDksh::Anadir(const Clave& clave, std::vector<uint8_t> dksh) {
  entradas_[clave] = std::move(dksh);
}

}  // namespace nfsmw::deko

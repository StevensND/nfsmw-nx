// nfsmw - deko3d renderer: vertex and index data kept across frames. See nfsmw_dk_geometria.h.

#include "nfsmw_dk_geometria.h"

#include <xxhash.h>

#include <algorithm>

#include "nfsmw_dk_nucleo.h"

namespace nfsmw::deko {

namespace {

// Blocks in 64-byte steps: fewer, larger free pieces, and every block aligned for vertex and index fetches.
constexpr uint32_t kPaso = 64;

constexpr uint32_t Redondear(uint32_t v) { return (v + kPaso - 1) & ~(kPaso - 1); }

// splitmix64 of the identity: the keys are aligned host pointers, which a plain xor would cluster.
uint64_t Clave(uint64_t datos, uint32_t bytes, uint64_t modo) {
  uint64_t x = datos ^ (uint64_t(bytes) * 0x9E3779B97F4A7C15ull) ^ (modo * 0xD1B54A32D192ED03ull);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// The content hashes of a range, in 64-byte lines: the whole range (every line, in order) and each phase p (the lines
// i with i % kFases == p, then the first and the last). A line is hashed with XXH3 and folded in with a multiply-xor
// step; the phase hashes and the whole one come from the same line hashes, so one pass gives all of them.
constexpr uint32_t kLinea = 64;

uint64_t Plegar(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}

uint64_t HuellaLinea(const uint8_t* datos, uint32_t bytes, uint32_t i) {
  const uint32_t inicio = i * kLinea;
  return XXH3_64bits(datos + inicio, std::min(kLinea, bytes - inicio));
}

}  // namespace

uint64_t AlmacenGeometria::Huellas(const uint8_t* datos, uint32_t bytes, uint64_t* fases) {
  const uint32_t lineas = (bytes + kLinea - 1) / kLinea;
  if (lineas <= kFases) {
    return XXH3_64bits(datos, bytes);  // too small for a sample to save anything
  }
  uint64_t completa = 0x243F6A8885A308D3ull;
  for (uint32_t p = 0; p < kFases; ++p) {
    fases[p] = 0x13198A2E03707344ull + p;
  }
  uint64_t primera = 0, ultima = 0;
  for (uint32_t i = 0; i < lineas; ++i) {
    const uint64_t h = HuellaLinea(datos, bytes, i);
    completa = Plegar(completa, h);
    fases[i % kFases] = Plegar(fases[i % kFases], h);
    if (i == 0) {
      primera = h;
    }
    ultima = h;
  }
  for (uint32_t p = 0; p < kFases; ++p) {
    fases[p] = Plegar(Plegar(fases[p], primera), ultima);
  }
  return completa;
}

uint64_t AlmacenGeometria::HuellaFase(const uint8_t* datos, uint32_t bytes, uint32_t fase, uint64_t& leidos) {
  const uint32_t lineas = (bytes + kLinea - 1) / kLinea;
  uint64_t h = 0x13198A2E03707344ull + fase;
  for (uint32_t i = fase; i < lineas; i += kFases) {
    h = Plegar(h, HuellaLinea(datos, bytes, i));
    leidos += kLinea;
  }
  leidos += 2 * kLinea;
  return Plegar(Plegar(h, HuellaLinea(datos, bytes, 0)), HuellaLinea(datos, bytes, lineas - 1));
}

void AlmacenGeometria::Crear(Nucleo& nucleo, uint32_t mb, bool muestreo) {
  nucleo_ = &nucleo;
  muestreo_ = muestreo;
  if (!mb) {
    return;
  }
  bytes_total_ = uint64_t(mb) << 20;
  bloque_ = nucleo.disp().CrearBloque(uint32_t(bytes_total_), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                      "geometria entre fotogramas");
  cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(bloque_));
  gpu_ = dkMemBlockGetGpuAddr(bloque_);
  libres_.assign(1, {0u, uint32_t(bytes_total_)});
  tabla_.reserve(1u << 15);
}

void AlmacenGeometria::Destruir() {
  if (bloque_ && nucleo_) {
    nucleo_->disp().DestruirBloque(bloque_);
  }
  bloque_ = nullptr;
  cpu_ = nullptr;
  gpu_ = 0;
  tabla_.clear();
  rapida_.fill({});
  libres_.clear();
  pendientes_.clear();
  bytes_usados_ = 0;
}

bool AlmacenGeometria::Terminado(uint64_t trabajo) {
  // Never the work being recorded: its slot's fence is still the one of that slot's previous work.
  if (trabajo == 0 || trabajo <= terminado_hasta_) {
    return true;
  }
  if (trabajo >= trabajo_abierto_) {
    return false;
  }
  // Opening work A waited for the fence of A - ranuras, the previous work of its slot: everything up to there is
  // done. Each of the ranuras - 1 works after it is its slot's latest, so its fence is its own. Works finish in
  // order: advance the mark while they are done.
  const uint64_t ranuras = nucleo_->config().ranuras;
  if (trabajo_abierto_ > ranuras) {
    terminado_hasta_ = std::max(terminado_hasta_, trabajo_abierto_ - ranuras);
  }
  while (terminado_hasta_ + 1 < trabajo_abierto_ && nucleo_->TrabajoTerminado(terminado_hasta_ + 1)) {
    ++terminado_hasta_;
  }
  return trabajo <= terminado_hasta_;
}

void AlmacenGeometria::NuevoTrabajo(uint64_t trabajo_abierto) {
  trabajo_abierto_ = trabajo_abierto;
  ++epoca_;
  if (!activo()) {
    return;
  }
  while (!pendientes_.empty() && Terminado(pendientes_.front().trabajo)) {
    Devolver(pendientes_.front().offset, pendientes_.front().tam);
    pendientes_.pop_front();
  }
  // The rest may be out of order (stamped with older works): a whole pass, only when something is left.
  for (size_t i = 0; i < pendientes_.size();) {
    if (Terminado(pendientes_[i].trabajo)) {
      Devolver(pendientes_[i].offset, pendientes_[i].tam);
      pendientes_.erase(pendientes_.begin() + std::ptrdiff_t(i));
    } else {
      ++i;
    }
  }
}

void AlmacenGeometria::NuevoFotograma(uint64_t fotograma) {
  fotograma_ = fotograma;
  ++epoca_;
  if (activo() && fotograma_ - barrido_ >= 128) {
    barrido_ = fotograma_;
    Barrer();
  }
}

AlmacenGeometria::Resultado AlmacenGeometria::Consultar(const uint8_t* datos, uint32_t bytes, uint64_t modo,
                                                        uint32_t bytes_host, uint8_t*& destino) {
  destino = nullptr;
  if (!activo() || !bytes || bytes_host > bytes_total_ / 8) {
    return {};
  }
  ++estadisticas_.consultas;
  const uint64_t direccion = uint64_t(reinterpret_cast<uintptr_t>(datos));
  const uint64_t clave = Clave(direccion, bytes, modo);
  // A direct-mapped cache in front of the table: the same ranges come back every frame, and one array slot is
  // cheaper than the map's chain of nodes. The map's entries never move (node-based); Barrer, the only erase,
  // empties this cache.
  Rapida& rapida = rapida_[clave & (kRapidas - 1)];
  Entrada* encontrada = nullptr;
  bool nueva = false;
  if (rapida.entrada && rapida.clave == clave) {
    encontrada = rapida.entrada;
  } else {
    auto [it, insertada] = tabla_.try_emplace(clave);
    encontrada = &it->second;
    nueva = insertada;
    rapida = {clave, encontrada};
  }
  Entrada& e = *encontrada;
  if (!nueva && (e.datos != direccion || e.bytes != bytes || e.modo != modo)) {
    ++estadisticas_.colisiones;
    SoltarBloque(e);
    e = Entrada{};
    nueva = true;
  }
  e.fotograma = fotograma_;
  if (nueva) {
    e.datos = direccion;
    e.bytes = bytes;
    e.modo = modo;
  } else if (e.epoca == epoca_) {
    // Already checked in this epoch: its answer holds.
    if (e.tam) {
      ++estadisticas_.aciertos;
      estadisticas_.bytes_aciertos += bytes;
      return {&e, gpu_ + e.offset, e.vmin, e.vmax};
    }
    return {};
  } else if (e.volatil_hasta > fotograma_) {
    ++estadisticas_.volatiles;
    return {};
  }
  e.epoca = epoca_;
  // A stored range with its phase hashes: one eighth of its lines (rotating, so every line is read at least once
  // every kFases frames) plus the first and the last. The guard: the first kMuestrasGuardia sampled checks also hash
  // everything, and one disagreement switches sampling off.
  if (e.tam && e.fases_validas && muestreo_) {
    const uint32_t fase = uint32_t((fotograma_ + (e.datos >> 6)) % kFases);
    uint64_t leidos = 0;
    const bool muestra_igual = HuellaFase(datos, bytes, fase, leidos) == e.fases[fase];
    estadisticas_.bytes_huella += leidos;
    ++estadisticas_.muestreos;
    if (muestra_igual && estadisticas_.muestreos > kMuestrasGuardia) {
      e.cambios = 0;
      ++estadisticas_.aciertos;
      estadisticas_.bytes_aciertos += bytes;
      return {&e, gpu_ + e.offset, e.vmin, e.vmax};
    }
    if (muestra_igual) {
      uint64_t fases[kFases];
      const uint64_t completa = Huellas(datos, bytes, fases);
      estadisticas_.bytes_huella += bytes;
      if (completa == e.huella) {
        e.cambios = 0;
        ++estadisticas_.aciertos;
        estadisticas_.bytes_aciertos += bytes;
        return {&e, gpu_ + e.offset, e.vmin, e.vmax};
      }
      ++estadisticas_.desacuerdos;
      muestreo_ = false;
      Registro("[dk] geometria: la MUESTRA de un rango de %u bytes no vio un cambio que la huella completa si: se APAGA "
               "el muestreo (tras %llu comprobaciones por muestra)",
               bytes, static_cast<unsigned long long>(estadisticas_.muestreos));
    }
    // The sample changed (or disagreed): the whole path below decides.
  }
  uint64_t fases[kFases];
  const uint64_t huella = Huellas(datos, bytes, fases);
  estadisticas_.bytes_huella += bytes;
  const bool con_fases = (bytes + kLinea - 1) / kLinea > kFases;
  if (nueva) {
    e.huella = huella;  // first sighting: the ring this time
    e.fases_validas = con_fases;
    std::copy(fases, fases + kFases, e.fases);
    return {};
  }
  if (huella == e.huella) {
    if (con_fases && !e.fases_validas) {
      e.fases_validas = true;
      std::copy(fases, fases + kFases, e.fases);
    }
    e.cambios = 0;
    if (e.tam) {
      ++estadisticas_.aciertos;
      estadisticas_.bytes_aciertos += bytes;
      return {&e, gpu_ + e.offset, e.vmin, e.vmax};
    }
    // The second sighting with the same content: into the store.
    if (!Reservar(bytes_host, e)) {
      return {};
    }
    destino = cpu_ + e.offset;
    ++estadisticas_.admitidas;
    estadisticas_.bytes_admitidos += bytes_host;
    return {&e, gpu_ + e.offset, 0, 0};
  }
  // Changed: a new block next time it is stable; changing again, to the ring for a while.
  ++estadisticas_.cambios;
  e.huella = huella;
  e.fases_validas = con_fases;
  std::copy(fases, fases + kFases, e.fases);
  SoltarBloque(e);
  if (++e.cambios >= 2) {
    e.volatil_hasta = fotograma_ + kFotogramasVolatil;
  }
  return {};
}

void AlmacenGeometria::Admitida(const Resultado& r) {
  r.entrada->vmin = r.vmin;
  r.entrada->vmax = r.vmax;
}

void AlmacenGeometria::Usar(Entrada* e, uint64_t trabajo) {
  if (e) {
    e->trabajo = trabajo;
  }
}

bool AlmacenGeometria::Reservar(uint32_t bytes, Entrada& e) {
  const uint32_t tam = Redondear(bytes);
  for (int pasada = 0; pasada < 2; ++pasada) {
    for (size_t k = 0; k < libres_.size(); ++k) {
      auto& [inicio, largo] = libres_[k];
      if (largo < tam) {
        continue;
      }
      e.offset = inicio;
      e.tam = tam;
      inicio += tam;
      largo -= tam;
      if (!largo) {
        libres_.erase(libres_.begin() + std::ptrdiff_t(k));
      }
      bytes_usados_ += tam;
      return true;
    }
    if (pasada == 0) {
      Desalojar(tam);
    }
  }
  ++estadisticas_.sin_sitio;
  return false;
}

void AlmacenGeometria::Devolver(uint32_t offset, uint32_t tam) {
  auto it = std::lower_bound(libres_.begin(), libres_.end(), std::make_pair(offset, 0u));
  it = libres_.insert(it, {offset, tam});
  if (it + 1 != libres_.end() && it->first + it->second == (it + 1)->first) {
    it->second += (it + 1)->second;
    libres_.erase(it + 1);
  }
  if (it != libres_.begin() && (it - 1)->first + (it - 1)->second == it->first) {
    (it - 1)->second += it->second;
    libres_.erase(it);
  }
}

void AlmacenGeometria::SoltarBloque(Entrada& e) {
  if (!e.tam) {
    return;
  }
  bytes_usados_ -= e.tam;
  if (Terminado(e.trabajo)) {
    Devolver(e.offset, e.tam);
  } else {
    pendientes_.push_back({e.offset, e.tam, e.trabajo});
  }
  e.tam = 0;
  e.offset = 0;
  e.trabajo = 0;
}

void AlmacenGeometria::Desalojar(uint32_t necesarios) {
  // The blocks not used in this frame or the last, oldest first, until an eighth of the store (or what is needed)
  // is free again: one sort for many admissions.
  std::vector<std::pair<uint64_t, Entrada*>> candidatas;
  for (auto& [clave, e] : tabla_) {
    if (e.tam && e.fotograma + 2 <= fotograma_) {
      candidatas.emplace_back(e.fotograma, &e);
    }
  }
  std::sort(candidatas.begin(), candidatas.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  const uint64_t objetivo = std::max<uint64_t>(necesarios, bytes_total_ / 8);
  uint64_t liberados = 0;
  for (const auto& [fotograma, e] : candidatas) {
    if (liberados >= objetivo) {
      break;
    }
    liberados += e->tam;
    ++estadisticas_.desalojos;
    SoltarBloque(*e);
    e->epoca = 0;  // the next lookup checks it again (and may admit it again)
  }
  // What was freed earlier and is done by now.
  for (size_t i = 0; i < pendientes_.size();) {
    if (Terminado(pendientes_[i].trabajo)) {
      Devolver(pendientes_[i].offset, pendientes_[i].tam);
      pendientes_.erase(pendientes_.begin() + std::ptrdiff_t(i));
    } else {
      ++i;
    }
  }
}

void AlmacenGeometria::Barrer() {
  rapida_.fill({});
  // Entries without a block nobody asked for in a while (the first sightings of one-off ranges).
  for (auto it = tabla_.begin(); it != tabla_.end();) {
    if (!it->second.tam && it->second.fotograma + 256 < fotograma_) {
      it = tabla_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace nfsmw::deko

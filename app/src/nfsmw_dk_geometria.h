// nfsmw - deko3d renderer: vertex and index data kept across frames (the geometry store).
//
// What it fixes
//   Every draw copied the [vmin..vmax] range of each vertex binding, byte-swapped, into the upload ring, and
//   converted its indices into it too: about 10 MB per frame in run 5 (1,550 draws per frame), most of the
//   5.7-6.1 us per draw of "uploads and draw" (docs/deko3d-devlog.md). The Vulkan path measured that 99 % of those
//   bytes repeat: 41-49 % within the frame, which DedupeVertices already reuses, and 51-59 % equal to what an
//   earlier frame copied (nfsmw_nativo_vertices_dedupe.h). This is the second half.
//
// What it does
//   A persistent block (nfsmw_dk_geometria_mb, CPU-uncached as the ring) holds the host form of guest ranges seen
//   in earlier frames. A range is identified by its host pointer, its guest bytes and its mode (the byte order for
//   vertices; the index format, byte order, restart and offset for indices), and its content by the XXH3 of its
//   guest bytes. A range found with the same hash is bound from the store: no copy, no byte swap, no index
//   conversion, no ring space; for indices the stored vmin/vmax also skip the scan.
//
// Why it is exact
//   - The hash is of the guest bytes at the moment the draw is recorded, which is what the ring copy reads too.
//   - Within one epoch (one work slot, one frame, no guest wait for the GPU in between) a range is hashed once and
//     trusted after that: the same argument as DedupeVertices (a title cannot rewrite a range it already referenced
//     in a frame without synchronizing). A new work, a new frame or a guest wait starts a new epoch.
//   - A stored block is never written again while the GPU may read it. A range whose content changed gets a new
//     block; the old one goes back to the free list only once the last work that bound it is done
//     (Nucleo::TrabajoTerminado, and never the work still being recorded, whose slot fence is the previous one's).
//     The block is stamped when the draw binds it (Usar), after any change of work slot the draw may cause.
//
// What it does not keep
//   - Ranges seen once: a range enters the store the second time it is seen with the same hash, so one-off data
//     (a range at a new address every frame) never fills it.
//   - Changing data: a range that changed twice in a row goes to the ring unhashed for kFotogramasVolatil frames
//     (particles, the HUD); then it is hashed again.
//   When the store is full, the ranges not used for longest go first, never one used in this frame or the last.
//
// Single thread: the ring thread.

#pragma once

#include <deko3d.h>

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nfsmw::deko {

class Nucleo;

class AlmacenGeometria {
 public:
  struct Entrada;

  struct Resultado {
    Entrada* entrada = nullptr;
    DkGpuAddr gpu = 0;  // 0: not in the store, the caller uses the ring as before
    uint32_t vmin = 0, vmax = 0;  // indices: what the conversion found when it was stored
  };

  struct Estadisticas {
    uint64_t consultas = 0;
    uint64_t aciertos = 0;          // bound from the store
    uint64_t bytes_aciertos = 0;    // guest bytes not copied thanks to it
    uint64_t bytes_huella = 0;      // guest bytes hashed
    uint64_t admitidas = 0;         // ranges written into the store
    uint64_t bytes_admitidos = 0;
    uint64_t cambios = 0;           // ranges whose content changed
    uint64_t volatiles = 0;         // lookups sent to the ring unhashed (changing data)
    uint64_t sin_sitio = 0;         // admissions that did not fit even after evicting
    uint64_t desalojos = 0;         // blocks evicted to make room
    uint64_t colisiones = 0;        // two ranges on one key (the old one is dropped)
    uint64_t muestreos = 0;         // checks by sample (one phase of the lines)
    uint64_t desacuerdos = 0;       // a sample that missed a change the whole hash saw (switches sampling off)
  };

  AlmacenGeometria() = default;
  AlmacenGeometria(const AlmacenGeometria&) = delete;
  AlmacenGeometria& operator=(const AlmacenGeometria&) = delete;

  // The block, `mb` MiB in the nucleo's device. 0 = off (Buscar always answers "not in the store"). `muestreo`:
  // stored ranges are rechecked by a rotating sample of their lines (see Consultar).
  void Crear(Nucleo& nucleo, uint32_t mb, bool muestreo);
  bool muestreo() const { return muestreo_; }
  // After the GPU is idle.
  void Destruir();
  bool activo() const { return cpu_ != nullptr; }

  // A work was opened (its number, the Nucleo's EmpezarTrabajo argument): the blocks freed earlier go back once
  // their works are done, and what was checked must be checked again.
  void NuevoTrabajo(uint64_t trabajo_abierto);
  // A new frame (the LRU clock), also a new epoch.
  void NuevoFotograma(uint64_t fotograma);
  // A guest wait for the GPU: what was checked must be checked again.
  void Olvidar() { ++epoca_; }

  // Looks the range up (`bytes` guest bytes at `datos`, its host form is `bytes_host` bytes). When it is admitted
  // now, escribir(uint8_t* destino, uint32_t& vmin, uint32_t& vmax) writes its host form into the store (in order,
  // once: the memory is CPU-uncached) and gives vmin/vmax for indices.
  template <class Escribir>
  Resultado Buscar(const uint8_t* datos, uint32_t bytes, uint64_t modo, uint32_t bytes_host, Escribir&& escribir) {
    uint8_t* destino = nullptr;
    Resultado r = Consultar(datos, bytes, modo, bytes_host, destino);
    if (destino) {
      escribir(destino, r.vmin, r.vmax);
      Admitida(r);
    }
    return r;
  }
  // The draw binds it in work `trabajo` (the one being recorded).
  void Usar(Entrada* e, uint64_t trabajo);

  const Estadisticas& estadisticas() const { return estadisticas_; }
  uint64_t bytes_usados() const { return bytes_usados_; }
  uint64_t bytes_total() const { return bytes_total_; }
  size_t entradas() const { return tabla_.size(); }

  struct Entrada {
    uint64_t datos = 0;
    uint64_t modo = 0;
    uint64_t huella = 0;
    uint64_t epoca = 0;           // the last epoch it was checked in
    uint64_t fotograma = 0;       // the last frame it was looked up in
    uint64_t trabajo = 0;         // the last work that bound its block (0: never bound)
    uint64_t volatil_hasta = 0;   // up to this frame it goes to the ring unhashed
    uint32_t bytes = 0;
    uint32_t offset = 0, tam = 0;  // its block (tam 0: none)
    uint32_t vmin = 0, vmax = 0;
    uint32_t cambios = 0;          // content changes in a row
    bool fases_validas = false;    // fases[] describe the stored content (ranges of more than kFases lines)
    uint64_t fases[8] = {};        // the hash of each phase of its 64-byte lines (Huellas)
  };

 private:
  static constexpr uint64_t kFotogramasVolatil = 64;
  static constexpr uint32_t kFases = 8;
  static constexpr uint64_t kMuestrasGuardia = 20000;

  // The whole hash of a range and, when it has more than kFases lines, the hash of each phase into fases[].
  static uint64_t Huellas(const uint8_t* datos, uint32_t bytes, uint64_t* fases);
  // The hash of one phase; adds the bytes it read to `leidos`.
  static uint64_t HuellaFase(const uint8_t* datos, uint32_t bytes, uint32_t fase, uint64_t& leidos);

  Resultado Consultar(const uint8_t* datos, uint32_t bytes, uint64_t modo, uint32_t bytes_host, uint8_t*& destino);
  void Admitida(const Resultado& r);
  bool Reservar(uint32_t bytes, Entrada& e);
  // Gives its block back: at once if no work that bound it can be running, later otherwise.
  void SoltarBloque(Entrada& e);
  void Devolver(uint32_t offset, uint32_t tam);
  bool Terminado(uint64_t trabajo);
  void Desalojar(uint32_t necesarios);
  void Barrer();

  Nucleo* nucleo_ = nullptr;
  DkMemBlock bloque_ = nullptr;
  uint8_t* cpu_ = nullptr;
  DkGpuAddr gpu_ = 0;
  uint64_t bytes_total_ = 0, bytes_usados_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> libres_;  // offset, bytes; sorted by offset
  struct Pendiente {
    uint32_t offset, tam;
    uint64_t trabajo;
  };
  std::deque<Pendiente> pendientes_;
  std::unordered_map<uint64_t, Entrada> tabla_;
  struct Rapida {
    uint64_t clave = 0;
    Entrada* entrada = nullptr;
  };
  static constexpr size_t kRapidas = 8192;
  std::array<Rapida, kRapidas> rapida_{};
  uint64_t epoca_ = 1;
  uint64_t fotograma_ = 1;
  uint64_t trabajo_abierto_ = 0;
  uint64_t terminado_hasta_ = 0;  // every work up to this one is done
  uint64_t barrido_ = 0;          // frame of the last sweep
  bool muestreo_ = false;
  Estadisticas estadisticas_;
};

}  // namespace nfsmw::deko

// nfsmw - deko3d renderer, the core: device, queue, memory, work slots, shader code and swapchain
// (docs/HowToDeko3d.md, sections 2, 5.8-5.10 and 6). Switch only.
//
// What the rest of the renderer records its GPU work with. It knows nothing about the game: the test NRO
// (tools/switch/dk_prueba) uses it as it is, so each part is checked on the console before the game uses it.
//
// Device conventions: DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne. Row 0 of an image and of the
// window is the top and clip-space z goes from 0 to 1, as in Direct3D. Clip-space y points UP in deko3d 0.5.0
// whatever the origin flag (HowToDeko3d.md, section 6, item 2), as in Direct3D too: the deko3d shader library is
// compiled without -fvk-invert-y, and no viewport is ever given a negative height (deko3d computes it unsigned).
//
// Errors: deko3d's release library ends the process on any failed creation (result 2359-xxxx) without calling
// anything, so the log is flushed before each creation; the debug library (libdeko3dd, NFSMW_DEKO3D_DEPURAR)
// calls ManejarMensaje first. A queue in an error state (a GPU fault) aborts every later submit, flush, acquire
// and present, so those check it first and stop with the reason in the log.
//
// Threads: one Dispositivo (device and queue) is shared by everything that submits, through its methods, which
// take the queue lock (the game's ring thread records the frames, the presenter paints from the UI thread). Each
// of those threads records with its own Nucleo: command buffer, work slots, upload ring, image heap, code memory
// and descriptors, used by that thread only. One queue keeps the GPU work in submission order, as the Vulkan
// path's single queue does: what the ring thread submitted before a refresh of the guest output is done before
// the presenter's paint that samples it.

#pragma once

#include <deko3d.h>

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

struct NWindow;

namespace nfsmw::deko {

// ---- Log
// The app sends the lines to its log (REXLOG); the test NRO to a file on the SD card. Without a function they go
// to stdout.
using FuncionRegistro = void (*)(const char* linea);
void FijarRegistro(FuncionRegistro escribir, void (*vaciar)());
void Registro(const char* formato, ...) __attribute__((format(printf, 1, 2)));
// Before anything that may end the process (a creation, a submit): the lines already written must reach the SD.
void VaciarRegistro();
// Logs, flushes and ends the process.
[[noreturn]] void Fatal(const char* formato, ...) __attribute__((format(printf, 1, 2)));

// ---- Time
// The CPU counter (19.2 MHz) in nanoseconds: cheap enough for per-draw timers.
inline uint64_t AhoraNs() {
  uint64_t tics;
  asm volatile("mrs %0, cntpct_el0" : "=r"(tics));
  return tics * 625 / 12;
}
// GPU timestamps (dkCmdBufReportCounter, DkCounter_Timestamp: the second word of the 16-byte report) count
// 1.627 ns per unit on the Switch, as NVK's (docs/measuring.md). Raw differences times this are nanoseconds; check
// them against the wall clock anyway.
inline constexpr double kNsPorUnidadGpu = 1.627;

// The device and its one queue (HowToDeko3d.md, section 2). Every queue operation takes the lock.
class Dispositivo {
 public:
  Dispositivo() = default;
  Dispositivo(const Dispositivo&) = delete;
  Dispositivo& operator=(const Dispositivo&) = delete;
  ~Dispositivo();

  // `zcull`: DkQueueFlags_EnableZcull (HowToDeko3d.md, section 6, item 6). A failure ends the process (above).
  void Crear(bool zcull);
  // Waits for the GPU and destroys the queue and the device. Safe to call twice.
  void Destruir();

  DkDevice dispositivo() const { return dispositivo_; }

  // Memory blocks and swapchains of every thread are created and destroyed here, under their own lock (deko3d
  // does not say its device is safe to share for that). The log is flushed first: a failure ends the process.
  DkMemBlock CrearBloque(uint32_t bytes, uint32_t flags, const char* nombre);
  void DestruirBloque(DkMemBlock bloque);
  DkSwapchain CrearCadena(NWindow* ventana, const DkImage* const imagenes[], uint32_t cuantas);
  void DestruirCadena(DkSwapchain cadena);

  // Submits a command list and flushes the queue. `que` names it for the error message.
  void Enviar(DkCmdList lista, const char* que);
  // The same with two lists, in this order, and one flush.
  void Enviar(DkCmdList primera, DkCmdList segunda, const char* que);
  // Waits until the GPU has finished everything submitted.
  void EsperarInactiva();
  int Adquirir(DkSwapchain cadena);
  void Presentar(DkSwapchain cadena, int imagen);
  // Stops the process if a GPU fault left the queue in an error state; `antes_de` names the call that was next.
  // The caller holds the lock (the methods above check it themselves).
  void ComprobarCola(const char* antes_de) const;

 private:
  DkDevice dispositivo_ = nullptr;
  DkQueue cola_ = nullptr;
  std::mutex candado_;
  std::mutex creaciones_;
};

struct ConfigNucleo {
  uint32_t ranuras = 3;              // work slots: the CPU records one while the GPU runs the others
  uint32_t mb_subida = 64;           // per slot: vertices, indices, constants, texture uploads
  uint32_t kb_comandos = 4096;       // per slot: command memory (it grows past this with extra blocks)
  // per slot: command memory of the pre-list (ComandosPrevios); 0 = no pre-list
  uint32_t kb_previos = 0;
  uint32_t mb_codigo = 32;           // shader code (DKSH), bump allocated
  uint32_t mb_trozo_imagenes = 16;   // image heap chunks (as the Vulkan texture pool's slabs)
  uint32_t descriptores_imagen = 8192;
  uint32_t descriptores_sampler = 1024;
  uint32_t kb_consultas = 64;        // GPU reports: timestamps, occlusion, statistics
};

// A suballocation of the upload ring of the current slot.
struct Subida {
  uint8_t* cpu = nullptr;
  DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
  explicit operator bool() const { return cpu != nullptr; }
};

// A suballocation of the image heap.
struct BloqueImagen {
  DkMemBlock bloque = nullptr;
  uint32_t offset = 0;
  uint32_t bytes = 0;
  int32_t trozo = -1;
  explicit operator bool() const { return bloque != nullptr; }
};

struct EstadisticasNucleo {
  uint64_t trabajos = 0;             // slots closed since the last call
  uint64_t bytes_subida = 0;         // upload ring bytes used, summed over those slots
  uint64_t subida_llena = 0;         // reservations that did not fit
  uint64_t bytes_comandos_max = 0;   // the most command memory one slot needed
  uint64_t desbordes_comandos = 0;   // slots that needed more than kb_comandos
  uint64_t bytes_imagenes = 0;       // image heap in use (totals, not reset)
  uint64_t trozos_imagenes = 0;
  uint64_t bytes_codigo = 0;
};

class Nucleo {
 public:
  Nucleo() = default;
  Nucleo(const Nucleo&) = delete;
  Nucleo& operator=(const Nucleo&) = delete;
  ~Nucleo();

  // Command buffer and memory on that device. Any failure inside deko3d ends the process (see above).
  void Crear(Dispositivo& dispositivo, const ConfigNucleo& config);
  // Waits for the GPU and frees everything. Safe to call twice.
  void Destruir();

  DkDevice dispositivo() const { return disp_ ? disp_->dispositivo() : nullptr; }
  Dispositivo& disp() const { return *disp_; }
  DkCmdBuf comandos() const { return comandos_; }
  // The slot's pre-list: what is recorded here runs before everything in comandos() of the same submission, and
  // its end waits for every engine and invalidates the image, descriptor and shader caches. Texture uploads and
  // descriptor writes go here, so the draws recorded before and after them in comandos() all see them (the
  // Vulkan path's upload command buffer). Only with ConfigNucleo::kb_previos.
  DkCmdBuf ComandosPrevios() {
    previos_usados_ = true;
    return previos_;
  }
  const ConfigNucleo& config() const { return config_; }

  // ---- Work slots (HowToDeko3d.md, section 5.8)
  // Starts recording work number `trabajo` in slot trabajo % ranuras: waits for that slot's previous fence, frees
  // the image memory released while it was recorded, rewinds its upload ring and its command memory.
  void EmpezarTrabajo(uint64_t trabajo);
  // Signals the slot's fence after everything recorded, submits it and flushes the queue.
  void TerminarTrabajo();
  bool TrabajoAbierto() const { return abierto_; }
  // The GPU has finished work number `trabajo` (no wait).
  bool TrabajoTerminado(uint64_t trabajo);
  // Nanoseconds spent waiting for slot fences in EmpezarTrabajo since start (the GPU being behind, not recording).
  uint64_t NsEsperandoValla() const { return ns_esperando_valla_; }
  // Submits what was recorded so far in the middle of a slot (without its fence) and keeps recording.
  void EnviarParcial();
  // Waits until the GPU is idle (at the end, or before freeing things whose last use is unknown).
  void EsperarGpu() { disp_->EsperarInactiva(); }

  // ---- Memory
  // `bytes` of the current slot's upload ring (CPU-uncached: write it once, in order, never read it back).
  // Empty when the slot's ring is full (counted; the caller skips what needed it).
  Subida Reservar(uint32_t bytes, uint32_t alineacion);
  // Bytes left in the open slot's upload ring (0 without an open slot), before any alignment.
  uint32_t SubidaLibre() const;
  BloqueImagen ReservarImagen(uint32_t bytes, uint32_t alineacion);
  // Returns it to the heap once the GPU has finished the slot being recorded now.
  void LiberarImagenLuego(const BloqueImagen& bloque);
  // GPU memory the CPU reads back (CPU-cached, GPU-uncached): flush its CPU cache (dkMemBlockFlushCpuCache)
  // before the GPU writes it and again before reading. Its own block: few and small (HowToDeko3d.md, 5.7).
  DkMemBlock CrearBloqueLectura(uint32_t bytes, const char* nombre);

  // Copies a DKSH's code into code memory and initializes `shader` from it. false (logged) if it does not fit or
  // is not a DKSH. Sets codigo_nuevo(): code copied in the middle of a frame is only seen by the GPU after the
  // shader caches are invalidated (deko3d does it at dkQueueFlush only).
  bool CargarShader(DkShader& shader, std::span<const uint8_t> dksh, const char* nombre);
  bool codigo_nuevo() const { return codigo_nuevo_; }
  // Records the invalidation if code was loaded since the last one.
  void InvalidarCodigoSiHaceFalta();

  // Image and sampler descriptor tables. The renderer writes them with dkCmdBufPushData only, ordered with the
  // commands (HowToDeko3d.md, section 5.4); BindDescriptores binds both (once per command buffer start).
  DkGpuAddr DescriptoresImagen() const;
  DkGpuAddr DescriptoresSampler() const;
  void BindDescriptores();

  // kb_consultas of CPU-uncached memory for dkCmdBufReportCounter (16 bytes per report).
  uint8_t* ConsultasCpu() const { return consultas_cpu_; }
  DkGpuAddr ConsultasGpu() const { return consultas_gpu_; }

  EstadisticasNucleo TomarEstadisticas();

  // Called by deko3d's command buffers when they run out of memory (dkCmdBufMaker::cbAddMem).
  void MasMemoriaDeComandos(DkCmdBuf cual, size_t minimo);

 private:
  // The command memory one command buffer has used in one slot.
  struct MemoriaRanura {
    uint32_t usados = 0;
    // Blocks created when the slice ran out, kept for the next time this slot runs out (after its fence).
    std::vector<std::pair<DkMemBlock, uint32_t>> desborde;
    size_t desborde_usado = 0;
  };
  struct Ranura {
    DkFence valla{};
    bool vallada = false;
    uint32_t subida_usada = 0;
    MemoriaRanura comandos, previos;
    std::vector<BloqueImagen> por_liberar;
  };
  struct Trozo {
    DkMemBlock bloque = nullptr;
    uint32_t bytes = 0;
    std::vector<std::pair<uint32_t, uint32_t>> libres;  // offset, bytes; sorted by offset
  };

  DkMemBlock CrearBloque(uint32_t bytes, uint32_t flags, const char* nombre);
  // The pre-list recorded so far, ended with its barrier; 0 if nothing was recorded in it.
  DkCmdList CerrarPrevios();
  void AnadirTrozo(uint32_t bytes);
  static void DevolverAlTrozo(Trozo& t, uint32_t offset, uint32_t bytes);

  ConfigNucleo config_;
  Dispositivo* disp_ = nullptr;
  DkCmdBuf comandos_ = nullptr;
  DkCmdBuf previos_ = nullptr;
  bool previos_usados_ = false;
  uint64_t ns_esperando_valla_ = 0;
  DkMemBlock memoria_previos_ = nullptr;
  DkMemBlock subida_ = nullptr, memoria_comandos_ = nullptr, codigo_ = nullptr, descriptores_ = nullptr,
             consultas_ = nullptr;
  std::vector<DkMemBlock> bloques_lectura_;
  uint8_t* subida_cpu_ = nullptr;
  DkGpuAddr subida_gpu_ = 0;
  uint8_t* consultas_cpu_ = nullptr;
  DkGpuAddr consultas_gpu_ = 0;
  std::vector<Ranura> ranuras_;
  uint32_t ranura_ = 0;
  bool abierto_ = false;
  uint32_t codigo_usado_ = 0;
  bool codigo_nuevo_ = false;
  std::vector<Trozo> trozos_;
  uint64_t comandos_este_trabajo_ = 0;
  EstadisticasNucleo estadisticas_;
};

// ---- Images
// Before dkImageLayoutInitialize of a block-linear 2D (array, cube, multisampled) image whose level 0 is `filas`
// rows tall (compressed formats: rows of blocks): deko3d 0.5.0 gives images of 6 to 8 rows of 8-row blocks a tile
// height that its layout and its descriptor disagree on, and the second column of blocks of level 0 lands on
// mip 1 (HowToDeko3d.md, section 6, item 3). This sets the tile its level 0 really uses.
void ArreglarTamanoDeTesela(DkImageLayoutMaker& m, uint32_t filas);

// ---- Presentation (HowToDeko3d.md, section 5.9)
class Presentacion {
 public:
  static constexpr uint32_t kImagenes = 3;
  Presentacion() = default;
  Presentacion(const Presentacion&) = delete;
  Presentacion& operator=(const Presentacion&) = delete;
  ~Presentacion();

  // A swapchain of kImagenes RGBA8 images of that size on `ventana` (nullptr: the default window), in its own
  // memory block. `intervalo` 0 presents at once (the Vulkan path's IMMEDIATE), 1 waits for vsync.
  void Crear(Dispositivo& disp, uint32_t ancho, uint32_t alto, uint32_t intervalo, NWindow* ventana = nullptr);
  // Waits for the GPU and frees it. Safe to call twice.
  void Destruir();
  // Index of the image to draw into; the acquire is ordered on the queue before what is submitted next.
  int Adquirir();
  void Presentar(int imagen);
  DkImage& Imagen(int i) { return imagenes_[i]; }
  uint32_t ancho() const { return ancho_; }
  uint32_t alto() const { return alto_; }
  bool creada() const { return cadena_ != nullptr; }

 private:
  Dispositivo* disp_ = nullptr;
  DkSwapchain cadena_ = nullptr;
  DkMemBlock memoria_ = nullptr;
  DkImage imagenes_[kImagenes];
  uint32_t ancho_ = 0, alto_ = 0;
};

// 1920x1080 docked, 1280x720 handheld (appletGetOperationMode). The game recreates the swapchain when it changes.
void TamanoDePantalla(uint32_t& ancho, uint32_t& alto);

}  // namespace nfsmw::deko

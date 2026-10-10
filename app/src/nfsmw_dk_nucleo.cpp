// nfsmw - deko3d renderer, the core. See nfsmw_dk_nucleo.h.

#include "nfsmw_dk_nucleo.h"

#include <switch.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nfsmw::deko {
namespace {

FuncionRegistro g_escribir = nullptr;
void (*g_vaciar)() = nullptr;

constexpr uint32_t Alinear(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// Command memory is given to the command buffer in pieces of this size: the first at the start of each work,
// the next ones as it asks (MasMemoriaDeComandos). The report shows the use per slot in these steps.
constexpr uint32_t kTrozoComandos = 64u << 10;
// The last bytes of each piece are not given to deko3d. A command buffer made with dkCmdBufCreate has no reserved
// words, so an entry point that reserves fewer words than it writes (dkCmdBufBindVtxBuffers in 0.5.0) writes past
// the end of the piece; when the next piece starts right there, the next command overwrites those words while the
// GPU still reads them as part of the previous piece. With this gap, such words land in memory nobody reuses.
constexpr uint32_t kHolguraComandos = 256;

// deko3d's messages. Only the debug library (libdeko3dd) calls this, for every check it makes: warnings with
// DkResult_Success, and errors, after which it traps. The error is logged readably and ends the process here.
void ManejarMensaje(void*, const char* contexto, DkResult resultado, const char* mensaje) {
  if (resultado != DkResult_Success) {
    Fatal("[dk] error de deko3d en %s: resultado %d: %s", contexto ? contexto : "?", int(resultado),
          mensaje ? mensaje : "");
  }
  static int mensajes = 0;
  if (mensajes < 200) {
    Registro("[dk] deko3d %s: %s", contexto ? contexto : "?", mensaje ? mensaje : "");
  } else if (mensajes == 200) {
    Registro("[dk] deko3d: hay mas mensajes; no se registran");
  }
  ++mensajes;
}

void AnadirMemoriaDeComandos(void* nucleo, DkCmdBuf cual, size_t minimo) {
  static_cast<Nucleo*>(nucleo)->MasMemoriaDeComandos(cual, minimo);
}

}  // namespace

// ---- Log

void FijarRegistro(FuncionRegistro escribir, void (*vaciar)()) {
  g_escribir = escribir;
  g_vaciar = vaciar;
}

void Registro(const char* formato, ...) {
  char linea[1024];
  va_list args;
  va_start(args, formato);
  std::vsnprintf(linea, sizeof(linea), formato, args);
  va_end(args);
  if (g_escribir) {
    g_escribir(linea);
  } else {
    std::puts(linea);
  }
}

void VaciarRegistro() {
  if (g_vaciar) {
    g_vaciar();
  } else {
    std::fflush(stdout);
  }
}

void Fatal(const char* formato, ...) {
  char linea[1024];
  va_list args;
  va_start(args, formato);
  std::vsnprintf(linea, sizeof(linea), formato, args);
  va_end(args);
  Registro("%s", linea);
  VaciarRegistro();
  std::abort();
}

// ---- Device and queue

Dispositivo::~Dispositivo() { Destruir(); }

void Dispositivo::Crear(bool zcull) {
  Registro("[dk] creando el dispositivo de deko3d (origen arriba a la izquierda, profundidad 0..1)");
  VaciarRegistro();
  DkDeviceMaker dm;
  dkDeviceMakerDefaults(&dm);
  dm.cbDebug = ManejarMensaje;
  dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
  dispositivo_ = dkDeviceCreate(&dm);

  DkQueueMaker qm;
  dkQueueMakerDefaults(&qm, dispositivo_);
  qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio |
             (zcull ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull);
  qm.commandMemorySize = 1u << 20;
  qm.flushThreshold = qm.commandMemorySize / 8;
  Registro("[dk] creando la cola: graficos, ZCULL %s", zcull ? "activo" : "apagado");
  VaciarRegistro();
  cola_ = dkQueueCreate(&qm);
}

void Dispositivo::Destruir() {
  if (!dispositivo_) {
    return;
  }
  if (cola_) {
    std::lock_guard<std::mutex> cerrojo(candado_);
    if (!dkQueueIsInErrorState(cola_)) {
      dkQueueWaitIdle(cola_);
    }
    dkQueueDestroy(cola_);
    cola_ = nullptr;
  }
  dkDeviceDestroy(dispositivo_);
  dispositivo_ = nullptr;
}

DkMemBlock Dispositivo::CrearBloque(uint32_t bytes, uint32_t flags, const char* nombre) {
  bytes = Alinear(bytes, DK_MEMBLOCK_ALIGNMENT);
  std::lock_guard<std::mutex> cerrojo(creaciones_);
  Registro("[dk] creando un bloque de memoria: %s, %u KiB (flags 0x%X)", nombre, bytes >> 10, flags);
  VaciarRegistro();  // a failed creation ends the process inside deko3d
  DkMemBlockMaker m;
  dkMemBlockMakerDefaults(&m, dispositivo_, bytes);
  m.flags = flags;
  return dkMemBlockCreate(&m);
}

void Dispositivo::DestruirBloque(DkMemBlock bloque) {
  if (bloque) {
    std::lock_guard<std::mutex> cerrojo(creaciones_);
    dkMemBlockDestroy(bloque);
  }
}

DkSwapchain Dispositivo::CrearCadena(NWindow* ventana, const DkImage* const imagenes[], uint32_t cuantas) {
  std::lock_guard<std::mutex> cerrojo(creaciones_);
  VaciarRegistro();
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, dispositivo_, ventana ? ventana : nwindowGetDefault(), imagenes, cuantas);
  return dkSwapchainCreate(&sm);
}

void Dispositivo::DestruirCadena(DkSwapchain cadena) {
  if (cadena) {
    std::lock_guard<std::mutex> cerrojo(creaciones_);
    dkSwapchainDestroy(cadena);
  }
}

void Dispositivo::ComprobarCola(const char* antes_de) const {
  if (dkQueueIsInErrorState(cola_)) {
    Fatal("[dk] la cola de deko3d esta en error (un fallo de la GPU) antes de %s", antes_de);
  }
}

void Dispositivo::Enviar(DkCmdList lista, const char* que) {
  std::lock_guard<std::mutex> cerrojo(candado_);
  ComprobarCola(que);
  dkQueueSubmitCommands(cola_, lista);
  dkQueueFlush(cola_);
}

void Dispositivo::Enviar(DkCmdList primera, DkCmdList segunda, const char* que) {
  std::lock_guard<std::mutex> cerrojo(candado_);
  ComprobarCola(que);
  dkQueueSubmitCommands(cola_, primera);
  dkQueueSubmitCommands(cola_, segunda);
  dkQueueFlush(cola_);
}

void Dispositivo::EsperarInactiva() {
  std::lock_guard<std::mutex> cerrojo(candado_);
  ComprobarCola("esperar a la GPU");
  dkQueueWaitIdle(cola_);
}

int Dispositivo::Adquirir(DkSwapchain cadena) {
  // The acquire may wait for the window to give an image back: without the queue lock, so the ring thread can keep
  // submitting; only the GPU wait for that image goes into the queue.
  int imagen = 0;
  DkFence valla;
  dkSwapchainAcquireImage(cadena, &imagen, &valla);
  std::lock_guard<std::mutex> cerrojo(candado_);
  ComprobarCola("esperar una imagen de la cadena");
  dkQueueWaitFence(cola_, &valla);
  return imagen;
}

void Dispositivo::Presentar(DkSwapchain cadena, int imagen) {
  std::lock_guard<std::mutex> cerrojo(candado_);
  ComprobarCola("presentar");
  dkQueuePresentImage(cola_, cadena, imagen);
}

// ---- One recording thread's command buffer and memory

Nucleo::~Nucleo() { Destruir(); }

DkMemBlock Nucleo::CrearBloque(uint32_t bytes, uint32_t flags, const char* nombre) {
  return disp_->CrearBloque(bytes, flags, nombre);
}

void Nucleo::Crear(Dispositivo& disp, const ConfigNucleo& config) {
  disp_ = &disp;
  config_ = config;
  config_.ranuras = std::max<uint32_t>(config_.ranuras, 1);
  config_.kb_comandos = std::max<uint32_t>(Alinear(config_.kb_comandos, kTrozoComandos >> 10), kTrozoComandos >> 10);

  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, dispositivo());
  cm.userData = this;
  cm.cbAddMem = AnadirMemoriaDeComandos;
  comandos_ = dkCmdBufCreate(&cm);
  if (config_.kb_previos) {
    config_.kb_previos = std::max<uint32_t>(Alinear(config_.kb_previos, kTrozoComandos >> 10), kTrozoComandos >> 10);
    previos_ = dkCmdBufCreate(&cm);
  }

  constexpr uint32_t kSinCacheCpu = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  const uint32_t n = config_.ranuras;
  memoria_comandos_ = CrearBloque(n * (config_.kb_comandos << 10), kSinCacheCpu, "memoria de comandos (por ranura)");
  if (previos_) {
    memoria_previos_ =
        CrearBloque(n * (config_.kb_previos << 10), kSinCacheCpu, "memoria de la lista previa (por ranura)");
  }
  subida_ = CrearBloque(n * (config_.mb_subida << 20), kSinCacheCpu, "anillo de subida (por ranura)");
  subida_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(subida_));
  subida_gpu_ = dkMemBlockGetGpuAddr(subida_);
  codigo_ = CrearBloque(config_.mb_codigo << 20, kSinCacheCpu | DkMemBlockFlags_Code, "codigo de shaders");
  descriptores_ = CrearBloque((config_.descriptores_imagen + config_.descriptores_sampler) * 32, kSinCacheCpu,
                              "descriptores de imagenes y samplers");
  consultas_ = CrearBloque(config_.kb_consultas << 10, kSinCacheCpu, "consultas de la GPU");
  consultas_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(consultas_));
  consultas_gpu_ = dkMemBlockGetGpuAddr(consultas_);
  std::memset(consultas_cpu_, 0, config_.kb_consultas << 10);
  ranuras_.assign(n, Ranura{});
  AnadirTrozo(config_.mb_trozo_imagenes << 20);
  Registro("[dk] nucleo listo: %u ranuras, %u MiB de subida, %u KiB de comandos y %u KiB de lista previa por ranura, "
           "%u MiB de codigo",
           n, config_.mb_subida, config_.kb_comandos, previos_ ? config_.kb_previos : 0, config_.mb_codigo);
}

void Nucleo::Destruir() {
  if (!disp_) {
    return;
  }
  disp_->EsperarInactiva();
  if (comandos_) {
    dkCmdBufDestroy(comandos_);
  }
  if (previos_) {
    dkCmdBufDestroy(previos_);
  }
  for (Ranura& r : ranuras_) {
    for (MemoriaRanura* m : {&r.comandos, &r.previos}) {
      for (auto& [bloque, bytes] : m->desborde) {
        disp_->DestruirBloque(bloque);
      }
    }
  }
  ranuras_.clear();
  for (Trozo& t : trozos_) {
    disp_->DestruirBloque(t.bloque);
  }
  trozos_.clear();
  for (DkMemBlock b : bloques_lectura_) {
    disp_->DestruirBloque(b);
  }
  bloques_lectura_.clear();
  for (DkMemBlock b : {memoria_comandos_, memoria_previos_, subida_, codigo_, descriptores_, consultas_}) {
    if (b) {
      disp_->DestruirBloque(b);
    }
  }
  disp_ = nullptr;
  comandos_ = previos_ = nullptr;
  memoria_comandos_ = memoria_previos_ = subida_ = codigo_ = descriptores_ = consultas_ = nullptr;
}

// ---- Work slots

void Nucleo::EmpezarTrabajo(uint64_t trabajo) {
  ranura_ = uint32_t(trabajo % config_.ranuras);
  Ranura& r = ranuras_[ranura_];
  if (r.vallada) {
    const uint64_t antes = AhoraNs();
    for (int espera = 1;; ++espera) {
      if (dkFenceWait(&r.valla, 2'000'000'000ll) == DkResult_Success) {
        break;
      }
      disp_->ComprobarCola("esperar la valla de una ranura");
      Registro("[dk] trabajo %llu: la GPU no ha terminado el de la ranura %u tras %d s",
               static_cast<unsigned long long>(trabajo), ranura_, espera * 2);
      VaciarRegistro();
    }
    ns_esperando_valla_ += AhoraNs() - antes;
  }
  for (const BloqueImagen& b : r.por_liberar) {
    DevolverAlTrozo(trozos_[size_t(b.trozo)], b.offset, b.bytes);
  }
  r.por_liberar.clear();
  r.subida_usada = 0;
  // dkCmdBufClear rewinds to the start of the last piece of memory given (deko3d 0.5.0), which belongs to another
  // slot: the slot's first piece is given again explicitly, as deko_examples' CCmdMemRing does.
  dkCmdBufClear(comandos_);
  dkCmdBufAddMemory(comandos_, memoria_comandos_, ranura_ * (config_.kb_comandos << 10),
                    kTrozoComandos - kHolguraComandos);
  r.comandos.usados = kTrozoComandos;
  r.comandos.desborde_usado = 0;
  if (previos_) {
    dkCmdBufClear(previos_);
    dkCmdBufAddMemory(previos_, memoria_previos_, ranura_ * (config_.kb_previos << 10),
                      kTrozoComandos - kHolguraComandos);
    r.previos.usados = kTrozoComandos;
    r.previos.desborde_usado = 0;
  }
  previos_usados_ = false;
  comandos_este_trabajo_ = kTrozoComandos;
  abierto_ = true;
}

void Nucleo::MasMemoriaDeComandos(DkCmdBuf cual, size_t minimo) {
  Ranura& r = ranuras_[ranura_];
  const bool previa = cual == previos_ && previos_;
  MemoriaRanura& m = previa ? r.previos : r.comandos;
  DkMemBlock memoria = previa ? memoria_previos_ : memoria_comandos_;
  // Every piece keeps kHolguraComandos at its end that deko3d is not told about.
  const uint32_t quiero =
      Alinear(uint32_t(std::max<size_t>(minimo + kHolguraComandos, kTrozoComandos)), DK_MEMBLOCK_ALIGNMENT);
  const uint32_t rebanada = (previa ? config_.kb_previos : config_.kb_comandos) << 10;
  if (m.usados + quiero <= rebanada) {
    dkCmdBufAddMemory(cual, memoria, ranura_ * rebanada + m.usados, quiero - kHolguraComandos);
    m.usados += quiero;
    comandos_este_trabajo_ += quiero;
    return;
  }
  if (m.desborde_usado == 0) {
    ++estadisticas_.desbordes_comandos;
  }
  while (m.desborde_usado < m.desborde.size()) {
    const auto [bloque, bytes] = m.desborde[m.desborde_usado++];
    if (bytes >= quiero) {
      dkCmdBufAddMemory(cual, bloque, 0, bytes - kHolguraComandos);
      comandos_este_trabajo_ += bytes;
      return;
    }
  }
  const uint32_t bytes = std::max<uint32_t>(quiero, 1u << 20);
  DkMemBlock bloque = CrearBloque(bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                  previa ? "lista previa de desborde" : "memoria de comandos de desborde");
  m.desborde.push_back({bloque, bytes});
  m.desborde_usado = m.desborde.size();
  dkCmdBufAddMemory(cual, bloque, 0, bytes - kHolguraComandos);
  comandos_este_trabajo_ += bytes;
}

DkCmdList Nucleo::CerrarPrevios() {
  if (!previos_ || !previos_usados_) {
    return 0;
  }
  // Uploads (copy engine), descriptor writes and code are done and visible before comandos() runs
  dkCmdBufBarrier(previos_, DkBarrier_Full,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
  previos_usados_ = false;
  return dkCmdBufFinishList(previos_);
}

void Nucleo::TerminarTrabajo() {
  Ranura& r = ranuras_[ranura_];
  const DkCmdList previa = CerrarPrevios();
  dkCmdBufSignalFence(comandos_, &r.valla, true);
  r.vallada = true;
  if (previa) {
    disp_->Enviar(previa, dkCmdBufFinishList(comandos_), "enviar un trabajo");
  } else {
    disp_->Enviar(dkCmdBufFinishList(comandos_), "enviar un trabajo");
  }
  abierto_ = false;
  ++estadisticas_.trabajos;
  estadisticas_.bytes_subida += r.subida_usada;
  estadisticas_.bytes_comandos_max = std::max(estadisticas_.bytes_comandos_max, comandos_este_trabajo_);
}

void Nucleo::EnviarParcial() {
  const DkCmdList previa = CerrarPrevios();
  if (previa) {
    disp_->Enviar(previa, dkCmdBufFinishList(comandos_), "un envio parcial");
  } else {
    disp_->Enviar(dkCmdBufFinishList(comandos_), "un envio parcial");
  }
}

bool Nucleo::TrabajoTerminado(uint64_t trabajo) {
  Ranura& r = ranuras_[trabajo % config_.ranuras];
  return !r.vallada || dkFenceWait(&r.valla, 0) == DkResult_Success;
}

Subida Nucleo::Reservar(uint32_t bytes, uint32_t alineacion) {
  Ranura& r = ranuras_[ranura_];
  const uint32_t rebanada = config_.mb_subida << 20;
  const uint32_t offset = Alinear(r.subida_usada, std::max<uint32_t>(alineacion, 4));
  if (!abierto_ || uint64_t(offset) + bytes > rebanada) {
    if (estadisticas_.subida_llena++ < 20) {
      Registro("[dk] subida: %u bytes no caben en la ranura %u (%u de %u MiB usados)%s", bytes, ranura_,
               r.subida_usada >> 20, config_.mb_subida, abierto_ ? "" : " (fuera de un trabajo)");
    }
    return {};
  }
  r.subida_usada = offset + bytes;
  const uint32_t at = ranura_ * rebanada + offset;
  return {subida_cpu_ + at, subida_gpu_ + at};
}

uint32_t Nucleo::SubidaLibre() const {
  if (!abierto_) {
    return 0;
  }
  const uint32_t rebanada = config_.mb_subida << 20;
  const uint32_t usada = ranuras_[ranura_].subida_usada;
  return usada < rebanada ? rebanada - usada : 0;
}

DkMemBlock Nucleo::CrearBloqueLectura(uint32_t bytes, const char* nombre) {
  DkMemBlock b = CrearBloque(bytes, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached, nombre);
  bloques_lectura_.push_back(b);
  return b;
}

// ---- Image heap: first fit in chunks, frees delayed until the slot's fence

void Nucleo::AnadirTrozo(uint32_t bytes) {
  char nombre[48];
  std::snprintf(nombre, sizeof(nombre), "trozo %zu de imagenes", trozos_.size());
  bytes = Alinear(bytes, DK_MEMBLOCK_ALIGNMENT);
  Trozo t;
  t.bloque = CrearBloque(bytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, nombre);
  t.bytes = bytes;
  t.libres.push_back({0, bytes});
  trozos_.push_back(std::move(t));
  estadisticas_.trozos_imagenes = trozos_.size();
}

void Nucleo::DevolverAlTrozo(Trozo& t, uint32_t offset, uint32_t bytes) {
  auto it = std::lower_bound(t.libres.begin(), t.libres.end(), std::make_pair(offset, 0u));
  it = t.libres.insert(it, {offset, bytes});
  if (it + 1 != t.libres.end() && it->first + it->second == (it + 1)->first) {
    it->second += (it + 1)->second;
    t.libres.erase(it + 1);
  }
  if (it != t.libres.begin() && (it - 1)->first + (it - 1)->second == it->first) {
    (it - 1)->second += it->second;
    t.libres.erase(it);
  }
}

BloqueImagen Nucleo::ReservarImagen(uint32_t bytes, uint32_t alineacion) {
  alineacion = std::max<uint32_t>(alineacion, 256);
  bytes = Alinear(bytes, 256);
  for (int pasada = 0; pasada < 2; ++pasada) {
    for (size_t i = 0; i < trozos_.size(); ++i) {
      Trozo& t = trozos_[i];
      for (size_t k = 0; k < t.libres.size(); ++k) {
        const auto [inicio, largo] = t.libres[k];
        const uint32_t at = Alinear(inicio, alineacion);
        if (uint64_t(at - inicio) + bytes > largo) {
          continue;
        }
        t.libres.erase(t.libres.begin() + std::ptrdiff_t(k));
        if (at > inicio) {
          DevolverAlTrozo(t, inicio, at - inicio);
        }
        if (at + bytes < inicio + largo) {
          DevolverAlTrozo(t, at + bytes, inicio + largo - at - bytes);
        }
        estadisticas_.bytes_imagenes += bytes;
        return {t.bloque, at, bytes, int32_t(i)};
      }
    }
    AnadirTrozo(std::max(config_.mb_trozo_imagenes << 20, Alinear(bytes + alineacion, DK_MEMBLOCK_ALIGNMENT)));
  }
  Fatal("[dk] el monton de imagenes no puede colocar %u bytes", bytes);
}

void Nucleo::LiberarImagenLuego(const BloqueImagen& bloque) {
  if (bloque.trozo < 0) {
    return;
  }
  estadisticas_.bytes_imagenes -= bloque.bytes;
  ranuras_[ranura_].por_liberar.push_back(bloque);
}

// ---- Shader code

bool Nucleo::CargarShader(DkShader& shader, std::span<const uint8_t> dksh, const char* nombre) {
  // The DKSH header (uam's dksh.h): the control section, which starts with this header and stays in CPU memory,
  // is followed by the code, which goes to code memory.
  struct Cabecera {
    uint32_t magia, bytes_cabecera, bytes_control, bytes_codigo, programas_offset, programas;
  } c;
  if (dksh.size() < sizeof(c)) {
    Registro("[dk] shader %s: %zu bytes, no es un DKSH", nombre, dksh.size());
    return false;
  }
  std::memcpy(&c, dksh.data(), sizeof(c));
  if (c.magia != 0x48534B44u || c.programas != 1 || uint64_t(c.bytes_control) + c.bytes_codigo > dksh.size()) {
    Registro("[dk] shader %s: no es un DKSH de un programa (magia 0x%08X, %u programas)", nombre, c.magia,
             c.programas);
    return false;
  }
  const uint32_t at = Alinear(codigo_usado_, DK_SHADER_CODE_ALIGNMENT);
  if (uint64_t(at) + c.bytes_codigo > (config_.mb_codigo << 20) - DK_SHADER_CODE_UNUSABLE_SIZE) {
    Registro("[dk] memoria de codigo llena: %s (%u bytes) no se carga", nombre, c.bytes_codigo);
    return false;
  }
  std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(codigo_)) + at, dksh.data() + c.bytes_control,
              c.bytes_codigo);
  DkShaderMaker m;
  dkShaderMakerDefaults(&m, codigo_, at);
  m.control = dksh.data();
  m.programId = 0;
  dkShaderInitialize(&shader, &m);
  if (!dkShaderIsValid(&shader)) {
    Registro("[dk] shader %s: deko3d no lo acepta", nombre);
    return false;
  }
  codigo_usado_ = at + c.bytes_codigo;
  estadisticas_.bytes_codigo = codigo_usado_;
  codigo_nuevo_ = true;
  return true;
}

void Nucleo::InvalidarCodigoSiHaceFalta() {
  if (codigo_nuevo_) {
    dkCmdBufBarrier(comandos_, DkBarrier_None, DkInvalidateFlags_Shader);
    codigo_nuevo_ = false;
  }
}

DkGpuAddr Nucleo::DescriptoresImagen() const { return dkMemBlockGetGpuAddr(descriptores_); }
DkGpuAddr Nucleo::DescriptoresSampler() const {
  return dkMemBlockGetGpuAddr(descriptores_) + DkGpuAddr(config_.descriptores_imagen) * 32;
}
void Nucleo::BindDescriptores() {
  dkCmdBufBindImageDescriptorSet(comandos_, DescriptoresImagen(), config_.descriptores_imagen);
  dkCmdBufBindSamplerDescriptorSet(comandos_, DescriptoresSampler(), config_.descriptores_sampler);
}

EstadisticasNucleo Nucleo::TomarEstadisticas() {
  const EstadisticasNucleo e = estadisticas_;
  estadisticas_.trabajos = estadisticas_.bytes_subida = estadisticas_.subida_llena = 0;
  estadisticas_.bytes_comandos_max = estadisticas_.desbordes_comandos = 0;
  return e;
}

// ---- Images

void ArreglarTamanoDeTesela(DkImageLayoutMaker& m, uint32_t filas) {
  if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) {
    return;
  }
  if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D ||
      m.type == DkImageType_Buffer) {
    return;  // 3D: deko3d never shrinks the depth tile at level 0
  }
  if ((m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) &&
      (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x)) {
    filas *= 2;  // the multisampled height
  }
  // deko3d's choice (pickTileSize of 1.5 times the height in 8-row GOBs) ...
  const uint32_t gobs = (filas + filas / 2 + 7) / 8;
  const uint32_t tesela = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
  // ... and the shrink its layout applies to level 0 (calcLevelOffset), which the descriptor and the engines do not
  uint32_t reducida = tesela;
  while (reducida && (8u << (reducida - 1)) >= filas) {
    --reducida;
  }
  if (reducida == tesela) {
    return;
  }
  m.flags |= DkImageFlags_CustomTileSize;
  m.tileSize = DkTileSize(reducida);
}

// ---- Presentation

Presentacion::~Presentacion() { Destruir(); }

void Presentacion::Crear(Dispositivo& disp, uint32_t ancho, uint32_t alto, uint32_t intervalo, NWindow* ventana) {
  Destruir();
  disp_ = &disp;
  ancho_ = ancho;
  alto_ = alto;
  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, disp.dispositivo());
  lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = ancho;
  lm.dimensions[1] = alto;
  DkImageLayout disposicion;
  dkImageLayoutInitialize(&disposicion, &lm);
  const uint32_t alineacion = dkImageLayoutGetAlignment(&disposicion);
  const uint32_t bytes = Alinear(uint32_t(dkImageLayoutGetSize(&disposicion)), alineacion);
  Registro("[dk] creando la cadena de presentacion: %u imagenes RGBA8 de %ux%u", kImagenes, ancho, alto);
  memoria_ = disp.CrearBloque(kImagenes * bytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                              "imagenes de la cadena de presentacion");
  const DkImage* imagenes[kImagenes];
  for (uint32_t i = 0; i < kImagenes; ++i) {
    dkImageInitialize(&imagenes_[i], &disposicion, memoria_, i * bytes);
    imagenes[i] = &imagenes_[i];
  }
  cadena_ = disp.CrearCadena(ventana, imagenes, kImagenes);
  dkSwapchainSetSwapInterval(cadena_, intervalo);
}

void Presentacion::Destruir() {
  if (!cadena_) {
    return;
  }
  disp_->EsperarInactiva();
  disp_->DestruirCadena(cadena_);
  disp_->DestruirBloque(memoria_);
  cadena_ = nullptr;
  memoria_ = nullptr;
}

int Presentacion::Adquirir() { return disp_->Adquirir(cadena_); }

void Presentacion::Presentar(int imagen) { disp_->Presentar(cadena_, imagen); }

void TamanoDePantalla(uint32_t& ancho, uint32_t& alto) {
  const bool sobremesa = appletGetOperationMode() == AppletOperationMode_Console;
  ancho = sobremesa ? 1920 : 1280;
  alto = sobremesa ? 1080 : 720;
}

}  // namespace nfsmw::deko

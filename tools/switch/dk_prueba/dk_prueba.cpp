/**
 * @file    dk_prueba.cpp
 * @brief   deko3d test NRO: the renderer's core and the game's shaders on the console (docs/HowToDeko3d.md, P0 and P1)
 *
 * What it checks, in order, with everything written to sdmc:/switch/nfsmw-nx/logs/dk_prueba.log (the folder of the
 * game's own logs):
 *
 *  1. uam on the console: the test pattern's own shaders are compiled at start by the same uam library (with
 *     SwitchWakerHD's patches) the game will use, on a worker thread as the game's will be (8 MB stack, priority
 *     0x3B). "uam listo" or "AUTOPRUEBA FALLIDA".
 *  2. The core (app/src/nfsmw_dk_nucleo.cpp): device, queue, work slots, upload ring, image heap, descriptors,
 *     swapchain at the console's mode (1280x720 handheld, 1920x1080 docked).
 *  3. The test pattern, which must look like this (Direct3D's conventions, the ones the game's shaders use):
 *       - four quadrants: red top left, green top right, blue bottom left, white bottom right;
 *       - in the middle, three overlapping squares with a depth test: magenta (far), yellow (near) and cyan (the
 *         farthest, drawn last): yellow covers magenta, and cyan only shows where it overlaps nothing;
 *       - a textured square below the middle: a white mark in its TOP LEFT corner, red growing to the right, green
 *         growing downwards (row 0 of a texture is its top, as in Direct3D);
 *       - a bar at the bottom: the progress of the shader checks (yellow), then green (all good) or red.
 *     Anything mirrored or upside down means the y convention or the texture upload is wrong.
 *  4. With nfsmw_shaders_dk.nfsp (the deko3d shader library) next to it: every variant in shadercache_dksh.bin
 *     (made on the PC by shaders/dksh/nfsmw_dksh) is compiled again here and compared byte for byte (P0, step 5).
 *     Without that cache, each shader is compiled once with the usual value. The console's results are written
 *     to shadercache_dksh_consola.bin, with the compile times.
 *  5. The GPU cost of every pixel shader of the library: full-screen passes at the screen's size with dummy
 *     textures and constants, timed with GPU timestamps (x1.627, docs/measuring.md) and checked against the
 *     wall clock (P0, step 4). Sorted, most expensive first.
 *
 * + exits. Build: tools/switch/dk_prueba/build.sh (devkitpro/devkita64 container).
 */

#include "nfsmw_dk_cache_dksh.h"
#include "nfsmw_dk_glsl.h"
#include "nfsmw_dk_nucleo.h"
#include "nfsmw_shader_library.h"
#include "uam_api.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace deko = nfsmw::deko;
namespace fs = std::filesystem;

namespace {

const char* const kCarpeta = "sdmc:/switch/nfsmw-nx/";

// ---- Log on the SD card

FILE* g_log = nullptr;
void EscribirLinea(const char* linea) {
  if (g_log) {
    std::fputs(linea, g_log);
    std::fputc('\n', g_log);
  }
}
void VaciarLog() {
  if (g_log) std::fflush(g_log);
}

std::vector<uint8_t> LeerArchivo(const std::string& ruta) {
  std::ifstream f(ruta, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() <= 0) return {};
  std::vector<uint8_t> d(size_t(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) return {};
  return d;
}

bool EscribirArchivo(const std::string& ruta, const std::vector<uint8_t>& d) {
  std::ofstream f(ruta, std::ios::binary);
  return f.write(reinterpret_cast<const char*>(d.data()), d.size()) && f.flush();
}

// ---- The test's own shaders (GLSL compiled by uam on the console at start)

// One quad per draw from gl_VertexID (a triangle strip), its rectangle in clip space (x0, y0, x1, y1: y up, as in
// Direct3D), its color and its depth from the UBO. uv (0, 0) is the corner at (x0, y0).
const char kVsCuadro[] = R"(#version 460
layout(std140, binding = 0) uniform Cuadro { vec4 rect; vec4 color; vec4 extra; } q;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
void main() {
  vec2 uv = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  gl_Position = vec4(mix(q.rect.xy, q.rect.zw, uv), q.extra.x, 1.0);
  v_color = q.color;
  v_uv = uv;
}
)";
const char kFsCuadro[] = R"(#version 460
layout(std140, binding = 0) uniform Cuadro { vec4 rect; vec4 color; vec4 extra; } q;
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main() { o = q.extra.y > 0.5 ? texture(tex, v_uv) : v_color; }
)";
// The benchmark's vertex shader: a full-screen quad writing the 22 interpolators every library pixel shader may
// read (TEXCOORD0-15 at 0-15, COLOR0-5 at 16-21: the deko3d library's fixed locations).
const char kVsPantalla[] = R"(#version 460
layout(location = 0) out vec4 o[22];
void main() {
  vec2 uv = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
  for (int i = 0; i < 22; ++i) o[i] = vec4(uv, 0.25 + 0.03 * float(i), 1.0);
}
)";

struct Propio {
  const char* nombre;
  const char* glsl;
  bool vertices;
  std::vector<uint8_t> dksh;
};
Propio g_propios[] = {
    {"cuadro_vs", kVsCuadro, true, {}},
    {"cuadro_fs", kFsCuadro, false, {}},
    {"pantalla_vs", kVsPantalla, true, {}},
};

// ---- The compile worker: uam is not reentrant and needs a big stack

constexpr uint32_t kBase = (1u << 8) | (1u << 9);  // SPEC_CONSTANT_CONSTANTES_UBO | SPEC_CONSTANT_INV_TAMANO_TEX

struct InfoShader {  // per library shader, from its conversion with the usual value
  bool ok = false;
  bool vertices = false;
  uint32_t texturas = 0, texturas_3d = 0, texturas_cubo = 0;
  uint32_t mascara = 0;
  uint64_t huella_spirv = 0;
};

std::atomic<int> g_fase{0};  // 0 own shaders, 1 library, 2 done
std::atomic<uint32_t> g_hechos{0}, g_total{1};
std::atomic<bool> g_fallo{false};
nfsmw::native::BibliotecaShaders g_biblioteca;
std::vector<InfoShader> g_info;
deko::CacheDksh g_consola;

void Trabajador(void*) {
  const uint64_t t0 = deko::AhoraNs();
  uam::init(true);
  deko::Registro("[prueba] uam iniciado en %.1f ms", (deko::AhoraNs() - t0) / 1e6);
  for (Propio& p : g_propios) {
    uam::Result r = uam::compile(p.vertices ? uam::Stage::Vertex : uam::Stage::Fragment, p.glsl);
    if (!r.ok) {
      deko::Registro("[prueba] AUTOPRUEBA FALLIDA: %s no compila: %s", p.nombre, r.log.c_str());
      g_fallo = true;
    }
    p.dksh = std::move(r.dksh);
  }
  if (!g_fallo) deko::Registro("[prueba] uam listo: %zu shaders propios", std::size(g_propios));
  deko::VaciarRegistro();
  g_fase = 1;

  const std::string ruta_biblioteca = std::string(kCarpeta) + "nfsmw_shaders_dk.nfsp";
  try {
    g_biblioteca.Cargar(fs::path(ruta_biblioteca));
  } catch (const std::exception& e) {
    deko::Registro("[prueba] sin biblioteca de deko3d (%s): %s; se salta la prueba de shaders", ruta_biblioteca.c_str(),
                 e.what());
    g_fase = 2;
    return;
  }
  const auto& shaders = g_biblioteca.shaders();
  g_info.resize(shaders.size());
  std::map<uint64_t, size_t> por_huella;
  for (size_t i = 0; i < shaders.size(); ++i) {
    g_info[i].vertices = shaders[i].vertices;
    g_info[i].huella_spirv = deko::HuellaSpirv(shaders[i].spirv);
    g_info[i].mascara = deko::MascaraEspecializacion(shaders[i].spirv);
    por_huella.emplace(g_info[i].huella_spirv, i);
  }

  // What to compile: the PC's variants if its cache is here (to compare), or each shader once
  deko::CacheDksh anfitrion;
  std::string motivo;
  const std::vector<uint8_t> archivo = LeerArchivo(std::string(kCarpeta) + "shadercache_dksh.bin");
  const bool comparar = !archivo.empty() && anfitrion.Cargar(archivo, motivo);
  if (!archivo.empty() && !comparar) deko::Registro("[prueba] shadercache_dksh.bin no sirve: %s", motivo.c_str());
  std::vector<deko::CacheDksh::Clave> claves;
  if (comparar) {
    for (const auto& [clave, dksh] : anfitrion.entradas()) claves.push_back(clave);
  } else {
    for (const InfoShader& s : g_info)
      claves.push_back({s.huella_spirv, kBase & s.mascara, s.vertices ? ~0u : 0u});
  }
  deko::Registro("[prueba] biblioteca: %zu shaders; %zu variantes que compilar%s", shaders.size(), claves.size(),
               comparar ? " y comparar con las del PC" : "");
  deko::VaciarRegistro();
  g_total = uint32_t(claves.size());

  size_t iguales = 0, distintas = 0, fallidas = 0, sin_shader = 0;
  std::vector<double> ms;
  for (const deko::CacheDksh::Clave& clave : claves) {
    const auto it = por_huella.find(clave.huella);
    if (it == por_huella.end()) {
      ++sin_shader;
      ++g_hechos;
      continue;
    }
    const size_t i = it->second;
    const uint64_t t = deko::AhoraNs();
    const deko::GlslDeko g = deko::SpirvAGlsl(shaders[i].spirv, clave.especializacion, g_info[i].vertices ? clave.variante : ~0u);
    uam::Result r;
    if (!g.glsl.empty()) r = uam::compile(g.vertices ? uam::Stage::Vertex : uam::Stage::Fragment, g.glsl.c_str());
    ms.push_back((deko::AhoraNs() - t) / 1e6);
    if (!r.ok) {
      if (fallidas++ < 20)
        deko::Registro("[prueba] FALLO n%zu %08X %08X: %s%s", i, clave.especializacion, clave.variante, g.error.c_str(),
                     r.log.c_str());
    } else {
      if (clave.especializacion == (kBase & g_info[i].mascara)) {
        g_info[i].ok = true;
        g_info[i].texturas = g.texturas;
        g_info[i].texturas_3d = g.texturas_3d;
        g_info[i].texturas_cubo = g.texturas_cubo;
      }
      if (comparar) {
        const std::vector<uint8_t>* pc = anfitrion.Buscar(clave);
        if (pc && *pc == r.dksh) {
          ++iguales;
        } else if (distintas++ < 20) {
          deko::Registro("[prueba] DISTINTO del PC: n%zu %08X %08X (%zu bytes aqui, %zu en el PC)", i,
                       clave.especializacion, clave.variante, r.dksh.size(), pc ? pc->size() : 0);
        }
      }
      g_consola.Anadir(clave, std::move(r.dksh));
    }
    ++g_hechos;
  }
  std::sort(ms.begin(), ms.end());
  double suma = 0;
  for (double v : ms) suma += v;
  auto pct = [&](double p) { return ms.empty() ? 0.0 : ms[std::min(ms.size() - 1, size_t(p * ms.size()))]; };
  deko::Registro("[prueba] compilacion en la consola: %zu variantes, %zu fallidas; media %.1f ms, p50 %.1f, p90 %.1f, "
               "p99 %.1f, max %.1f; total %.1f s",
               ms.size(), fallidas, ms.empty() ? 0.0 : suma / ms.size(), pct(0.5), pct(0.9), pct(0.99),
               ms.empty() ? 0.0 : ms.back(), suma / 1000);
  if (comparar)
    deko::Registro("[prueba] comparacion con el PC: %zu iguales, %zu distintas, %zu sin shader en la biblioteca", iguales,
                 distintas, sin_shader);
  if (fallidas || distintas || sin_shader) g_fallo = true;
  const std::string salida = std::string(kCarpeta) + "shadercache_dksh_consola.bin";
  deko::Registro("[prueba] %s: %s", salida.c_str(), EscribirArchivo(salida, g_consola.Serializar()) ? "escrita" : "NO SE PUDO ESCRIBIR");
  deko::VaciarRegistro();
  g_fase = 2;
}

// ---- Rendering

struct Imagen {
  DkImage imagen;
  deko::BloqueImagen memoria;
};

void CrearImagen(deko::Nucleo& n, Imagen& img, DkImageType tipo, DkImageFormat formato, uint32_t flags, uint32_t ancho,
                 uint32_t alto, uint32_t profundidad, const char* nombre) {
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, n.dispositivo());
  m.type = tipo;
  m.flags = flags;
  m.format = formato;
  m.dimensions[0] = ancho;
  m.dimensions[1] = alto;
  m.dimensions[2] = profundidad;
  deko::ArreglarTamanoDeTesela(m, alto);  // uncompressed: one row of blocks per row
  DkImageLayout disposicion;
  dkImageLayoutInitialize(&disposicion, &m);
  img.memoria = n.ReservarImagen(uint32_t(dkImageLayoutGetSize(&disposicion)), dkImageLayoutGetAlignment(&disposicion));
  dkImageInitialize(&img.imagen, &disposicion, img.memoria.bloque, img.memoria.offset);
  deko::Registro("[prueba] imagen %s: %ux%ux%u, %u KiB", nombre, ancho, alto, profundidad, img.memoria.bytes >> 10);
}

// Copies RGBA8 texels into an image through the upload ring (the copy engine tiles them). `capas`: the depth of a
// 3D image, or the faces of a cube.
void SubirImagen(deko::Nucleo& n, Imagen& img, const void* datos, uint32_t ancho, uint32_t alto, uint32_t capas) {
  const uint32_t bytes = ancho * alto * 4 * capas;
  const deko::Subida s = n.Reservar(bytes, 256);
  std::memcpy(s.cpu, datos, bytes);
  DkImageView vista;
  dkImageViewDefaults(&vista, &img.imagen);
  const DkCopyBuf origen = {s.gpu, 0, 0};
  const DkImageRect rect = {0, 0, 0, ancho, alto, capas};
  dkCmdBufCopyBufferToImage(n.comandos(), &origen, &vista, &rect, 0);
}

void Cuadro(deko::Nucleo& n, float x0, float y0, float x1, float y1, float r, float g, float b, float z, bool textura) {
  const deko::Subida s = n.Reservar(48, DK_UNIFORM_BUF_ALIGNMENT);
  if (!s) return;
  const float d[12] = {x0, y0, x1, y1, r, g, b, 1.0f, z, textura ? 1.0f : 0.0f, 0, 0};
  std::memcpy(s.cpu, d, sizeof(d));
  dkCmdBufBindUniformBuffer(n.comandos(), DkStage_Vertex, 0, s.gpu, 48);
  dkCmdBufBindUniformBuffer(n.comandos(), DkStage_Fragment, 0, s.gpu, 48);
  dkCmdBufDraw(n.comandos(), DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

void EstadoBasico(deko::Nucleo& n, uint32_t ancho, uint32_t alto, bool profundidad) {
  DkCmdBuf c = n.comandos();
  const DkViewport vp = {0, 0, float(ancho), float(alto), 0, 1};
  const DkScissor tijera = {0, 0, ancho, alto};
  dkCmdBufSetViewports(c, 0, &vp, 1);
  dkCmdBufSetScissors(c, 0, &tijera, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  rs.provokingVertex = DkProvokingVertex_First;  // the Vulkan path's (HowToDeko3d.md, section 5.5)
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = profundidad;
  ds.depthWriteEnable = profundidad;
  dkCmdBufBindRasterizerState(c, &rs);
  dkCmdBufBindColorState(c, &cs);
  dkCmdBufBindColorWriteState(c, &cws);
  dkCmdBufBindDepthStencilState(c, &ds);
  dkCmdBufBindVtxAttribState(c, nullptr, 0);
  dkCmdBufBindVtxBufferState(c, nullptr, 0);
}

// The library's pixel shaders on full-screen passes (P0, step 4).
void MedirShaders(deko::Nucleo& n, const DkShader& vs_pantalla, uint32_t ancho, uint32_t alto) {
  const auto& shaders = g_biblioteca.shaders();
  Imagen destino;
  CrearImagen(n, destino, DkImageType_2D, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, ancho, alto, 1,
              "destino de la medida");
  struct Medida {
    size_t i;
    DkShader ps;
    double ms = 0;
  };
  std::vector<Medida> medidas;
  for (size_t i = 0; i < shaders.size(); ++i) {
    const InfoShader& s = g_info[i];
    if (s.vertices || !s.ok) continue;
    const std::vector<uint8_t>* dksh = g_consola.Buscar({s.huella_spirv, kBase & s.mascara, 0});
    Medida m{i, {}};
    char nombre[24];
    std::snprintf(nombre, sizeof(nombre), "n%zu", i);
    if (dksh && n.CargarShader(m.ps, *dksh, nombre)) medidas.push_back(m);
  }
  // Constants: every pixel shader constant 0.5; the shared block zero but for 1/size of every register (1/64)
  float ps_constantes[224 * 4];
  std::fill(std::begin(ps_constantes), std::end(ps_constantes), 0.5f);
  float compartidas[31 * 4] = {};
  for (int r = 0; r < 16; ++r) compartidas[(360 + r * 8) / 4] = compartidas[(360 + r * 8) / 4 + 1] = 1.0f / 64;
  constexpr uint32_t kPasadas = 8;
  constexpr size_t kPorTrabajo = 16;
  uint64_t trabajo = 1000;
  double ns_pared = 0, ns_gpu = 0;
  for (size_t a = 0; a < medidas.size(); a += kPorTrabajo) {
    const size_t b = std::min(medidas.size(), a + kPorTrabajo);
    n.EmpezarTrabajo(trabajo++);
    DkCmdBuf c = n.comandos();
    n.BindDescriptores();
    n.InvalidarCodigoSiHaceFalta();
    DkImageView vista;
    dkImageViewDefaults(&vista, &destino.imagen);
    dkCmdBufBindRenderTarget(c, &vista, nullptr);
    EstadoBasico(n, ancho, alto, false);
    const deko::Subida ubo_ps = n.Reservar(sizeof(ps_constantes), DK_UNIFORM_BUF_ALIGNMENT);
    const deko::Subida ubo_c = n.Reservar(sizeof(compartidas), DK_UNIFORM_BUF_ALIGNMENT);
    std::memcpy(ubo_ps.cpu, ps_constantes, sizeof(ps_constantes));
    std::memcpy(ubo_c.cpu, compartidas, sizeof(compartidas));
    dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 1, ubo_ps.gpu, sizeof(ps_constantes));
    dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 2, ubo_c.gpu, sizeof(compartidas));
    for (size_t k = a; k < b; ++k) {
      Medida& m = medidas[k];
      const InfoShader& s = g_info[m.i];
      for (uint32_t t = 0; t < deko::kMaxTexturas; ++t) {
        if (!(s.texturas & (1u << t))) continue;
        const uint32_t imagen = (s.texturas_3d & (1u << t)) ? 1 : (s.texturas_cubo & (1u << t)) ? 2 : 0;
        const DkResHandle h = dkMakeTextureHandle(imagen, 0);
        dkCmdBufBindTextures(c, DkStage_Fragment, t, &h, 1);
      }
      const DkShader* par[] = {&vs_pantalla, &m.ps};
      dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, par, 2);
      dkCmdBufReportCounter(c, DkCounter_Timestamp, n.ConsultasGpu() + (k - a) * 32);
      for (uint32_t p = 0; p < kPasadas; ++p) dkCmdBufDraw(c, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
      dkCmdBufReportCounter(c, DkCounter_Timestamp, n.ConsultasGpu() + (k - a) * 32 + 16);
    }
    const uint64_t t0 = deko::AhoraNs();
    n.TerminarTrabajo();
    n.EsperarGpu();
    ns_pared += double(deko::AhoraNs() - t0);
    for (size_t k = a; k < b; ++k) {
      uint64_t inicio, fin;
      std::memcpy(&inicio, n.ConsultasCpu() + (k - a) * 32 + 8, 8);
      std::memcpy(&fin, n.ConsultasCpu() + (k - a) * 32 + 24, 8);
      const double ns = double(fin - inicio) * deko::kNsPorUnidadGpu;
      ns_gpu += ns;
      medidas[k].ms = ns / kPasadas / 1e6;
    }
  }
  std::sort(medidas.begin(), medidas.end(), [](const Medida& x, const Medida& y) { return x.ms > y.ms; });
  deko::Registro("[prueba] coste en la GPU de %zu pixel shaders de la biblioteca: pasadas de %ux%u, %u por shader",
               medidas.size(), ancho, alto, kPasadas);
  for (const Medida& m : medidas)
    deko::Registro("[prueba]   n%-4zu contenedor %016llX  %.3f ms por pasada", m.i,
                 static_cast<unsigned long long>(shaders[m.i].huella), m.ms);
  deko::Registro("[prueba] GPU (x%.3f) %.1f ms frente a %.1f ms de reloj de pared para todo (incluye el envio y la "
               "espera): si la GPU suma mas que la pared, el factor no vale",
               deko::kNsPorUnidadGpu, ns_gpu / 1e6, ns_pared / 1e6);
  deko::VaciarRegistro();
  n.LiberarImagenLuego(destino.memoria);
}

}  // namespace

int main(int, char**) {
  std::error_code ec;
  fs::create_directories(std::string(kCarpeta) + "logs", ec);  // the game makes it too (logs/nfsmw_NNN.log)
  g_log = std::fopen((std::string(kCarpeta) + "logs/dk_prueba.log").c_str(), "w");
  deko::FijarRegistro(EscribirLinea, VaciarLog);
  deko::Registro("[prueba] nfsmw deko3d: prueba del nucleo y de los shaders (%s)",
               deko::IdCompilador().c_str());

  // The worker: uam on its own thread, as the game's compile worker (HowToDeko3d.md, section 3.3)
  Thread trabajador;
  if (R_FAILED(threadCreate(&trabajador, Trabajador, nullptr, nullptr, 8u << 20, 0x3B, -2)) ||
      R_FAILED(threadStart(&trabajador))) {
    deko::Fatal("[prueba] no se pudo crear el hilo de compilacion");
  }

  uint32_t ancho, alto;
  deko::TamanoDePantalla(ancho, alto);
  deko::Dispositivo dispositivo;
  dispositivo.Crear(true);
  deko::Nucleo n;
  deko::ConfigNucleo config;
  config.mb_subida = 16;
  n.Crear(dispositivo, config);
  deko::Presentacion presentacion;
  presentacion.Crear(dispositivo, ancho, alto, 1);
  Imagen profundidad;
  CrearImagen(n, profundidad, DkImageType_2D, DkImageFormat_Z24S8, DkImageFlags_UsageRender, ancho, alto, 1,
              "profundidad");

  while (g_fase.load() < 1) svcSleepThread(1'000'000);
  DkShader propios[std::size(g_propios)];
  bool listos = !g_fallo;
  for (size_t i = 0; listos && i < std::size(g_propios); ++i)
    listos = n.CargarShader(propios[i], g_propios[i].dksh, g_propios[i].nombre);

  // Dummy textures: a 64x64 gradient with a white mark at its top left (the orientation check), and gray 3D and
  // cube ones for the benchmark. Descriptors 0 (2D), 1 (3D), 2 (cube); sampler 0, linear with repeat.
  Imagen tex2d, tex3d, texcubo;
  CrearImagen(n, tex2d, DkImageType_2D, DkImageFormat_RGBA8_Unorm, 0, 64, 64, 1, "textura 2D");
  CrearImagen(n, tex3d, DkImageType_3D, DkImageFormat_RGBA8_Unorm, 0, 4, 4, 4, "textura 3D");
  CrearImagen(n, texcubo, DkImageType_Cubemap, DkImageFormat_RGBA8_Unorm, 0, 4, 4, 6, "textura cubo");
  n.EmpezarTrabajo(0);
  {
    std::vector<uint32_t> gradiente(64 * 64), gris(4 * 4 * 6, 0xFF808080u);
    for (uint32_t y = 0; y < 64; ++y)
      for (uint32_t x = 0; x < 64; ++x)
        gradiente[y * 64 + x] = (x < 8 && y < 8) ? 0xFFFFFFFFu : 0xFF000000u | (y * 4) << 8 | (x * 4);
    SubirImagen(n, tex2d, gradiente.data(), 64, 64, 1);
    SubirImagen(n, tex3d, gris.data(), 4, 4, 4);
    SubirImagen(n, texcubo, gris.data(), 4, 4, 6);
    Imagen* imagenes[] = {&tex2d, &tex3d, &texcubo};
    for (uint32_t i = 0; i < 3; ++i) {
      DkImageView vista;
      dkImageViewDefaults(&vista, &imagenes[i]->imagen);
      DkImageDescriptor d;
      dkImageDescriptorInitialize(&d, &vista, false, false);
      dkCmdBufPushData(n.comandos(), n.DescriptoresImagen() + i * 32, &d, 32);
    }
    DkSampler s;
    dkSamplerDefaults(&s);
    s.minFilter = s.magFilter = DkFilter_Linear;
    s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_Repeat;
    DkSamplerDescriptor sd;
    dkSamplerDescriptorInitialize(&sd, &s);
    dkCmdBufPushData(n.comandos(), n.DescriptoresSampler(), &sd, 32);
    // the copies and the descriptors before anything samples them
    dkCmdBufBarrier(n.comandos(), DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  }
  n.TerminarTrabajo();

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState mando;
  padInitializeDefault(&mando);
  bool medido = false;
  uint64_t trabajo = 1;
  while (appletMainLoop()) {
    padUpdate(&mando);
    if (padGetButtonsDown(&mando) & HidNpadButton_Plus) break;
    if (!listos) {
      svcSleepThread(16'000'000);
      continue;
    }
    if (!medido && g_fase.load() == 2) {
      medido = true;
      if (!g_info.empty()) MedirShaders(n, propios[2], ancho, alto);
    }

    const int imagen = presentacion.Adquirir();
    n.EmpezarTrabajo(trabajo++);
    DkCmdBuf c = n.comandos();
    n.BindDescriptores();
    n.InvalidarCodigoSiHaceFalta();
    DkImageView color, z;
    dkImageViewDefaults(&color, &presentacion.Imagen(imagen));
    dkImageViewDefaults(&z, &profundidad.imagen);
    dkCmdBufBindRenderTarget(c, &color, &z);
    EstadoBasico(n, ancho, alto, true);
    dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, 0.1f, 0.1f, 0.1f, 1.0f);
    dkCmdBufClearDepthStencil(c, true, 1.0f, 0xFF, 0);
    const DkShader* cuadro[] = {&propios[0], &propios[1]};
    dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, cuadro, 2);
    const DkResHandle h = dkMakeTextureHandle(0, 0);
    dkCmdBufBindTextures(c, DkStage_Fragment, 0, &h, 1);
    // quadrants (clip-space y up: y = 1 is the top row)
    Cuadro(n, -1, 1, 0, 0, 1, 0, 0, 0.99f, false);
    Cuadro(n, 0, 1, 1, 0, 0, 1, 0, 0.99f, false);
    Cuadro(n, -1, 0, 0, -1, 0, 0, 1, 0.99f, false);
    Cuadro(n, 0, 0, 1, -1, 1, 1, 1, 0.99f, false);
    // depth: magenta far, yellow near, cyan farthest and drawn last
    Cuadro(n, -0.30f, 0.45f, 0.10f, -0.05f, 1, 0, 1, 0.6f, false);
    Cuadro(n, -0.10f, 0.30f, 0.30f, -0.20f, 1, 1, 0, 0.4f, false);
    Cuadro(n, -0.20f, 0.60f, 0.40f, 0.15f, 0, 1, 1, 0.8f, false);
    // textured square: uv (0, 0) at its top left
    Cuadro(n, -0.15f, -0.30f, 0.15f, -0.75f, 1, 1, 1, 0.5f, true);
    // progress
    const float progreso = g_fase.load() == 2 ? 1.0f : float(g_hechos.load()) / float(std::max(1u, g_total.load()));
    const bool fin = g_fase.load() == 2;
    Cuadro(n, -1, -0.92f, -1 + 2 * progreso, -1, fin && g_fallo ? 1.0f : fin ? 0.0f : 1.0f, fin && g_fallo ? 0.0f : 1.0f,
           0, 0.1f, false);
    n.TerminarTrabajo();
    presentacion.Presentar(imagen);

    if (trabajo % 600 == 0) {
      const deko::EstadisticasNucleo e = n.TomarEstadisticas();
      deko::Registro("[prueba] %llu trabajos: subida %llu KiB por trabajo, comandos max %llu KiB, desbordes %llu",
                   static_cast<unsigned long long>(e.trabajos),
                   static_cast<unsigned long long>(e.trabajos ? e.bytes_subida / e.trabajos >> 10 : 0),
                   static_cast<unsigned long long>(e.bytes_comandos_max >> 10),
                   static_cast<unsigned long long>(e.desbordes_comandos));
      deko::VaciarRegistro();
    }
  }

  threadWaitForExit(&trabajador);
  threadClose(&trabajador);
  presentacion.Destruir();
  n.Destruir();
  dispositivo.Destruir();
  deko::Registro("[prueba] fin");
  VaciarLog();
  std::fclose(g_log);
  return 0;
}

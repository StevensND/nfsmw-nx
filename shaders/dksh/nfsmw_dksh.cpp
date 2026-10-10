/**
 * @file    nfsmw_dksh.cpp
 * @brief   The deko3d shader library to a DKSH cache, on the PC (docs/HowToDeko3d.md, section 3, step P0)
 *
 * Each (shader, specialization value) goes the way the console's compile worker will take it: SPIR-V of the deko3d
 * library -> GLSL (nfsmw_dk_glsl.cpp, SPIRV-Cross) -> DKSH (uam with SwitchWakerHD's patches). The result is the
 * same cache file the renderer loads (nfsmw_dk_cache_dksh.h), and a report: what failed and why, every line uam
 * wrote (its "decays to floating point" warning is a bug to look at: HowToDeko3d.md, section 6, item 13), the
 * shapes uam is known to get wrong, and the compile times.
 *
 * Which variants
 *
 *   --lista FILE   the pipelines of a console session (cache/nfsmw_nativo_pipelines.bin on the SD card): its VS and
 *                  PS with its specialization value. Records whose shaders are not in this library are skipped.
 *   (default)      every vertex shader with the usual value, and every pixel shader with each value that changes
 *                  its code: the 8 alpha test functions, the cheap PCF, the shadow minimum, no blur, and without
 *                  1/size (the texture size query).
 *
 * The renderer's SPIR-V variants (kSpecSoloAlfa's color-write pruning, kSpecZTemprana's early depth test) are made
 * by nfsmw_nativo_dibujos.cpp at run time; here those bits only change the key.
 *
 * Usage
 *   nfsmw_dksh <nfsmw_shaders_dk.nfsp> <output cache> [--lista FILE] [--contenedores DIR] [--glsl DIR]
 *              [--informe FILE.tsv]
 *   --contenedores   the library's container folder: the report uses its names (p_000139) instead of numbers.
 *   Exit code 0 only if every variant compiled.
 */

#include "../shader_common.h"
#include "nfsmw_dk_cache_dksh.h"
#include "nfsmw_dk_glsl.h"
#include "nfsmw_shader_library.h"
#include "uam_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
// Inline, with the memcpy reads of nfsmw_shader_library.cpp (see the note there).
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> Leer(const fs::path& ruta) {
  std::ifstream f(ruta, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0) throw std::runtime_error("no se puede leer " + ruta.string());
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) throw std::runtime_error("lectura incompleta: " + ruta.string());
  return d;
}

void Escribir(const fs::path& ruta, const void* datos, size_t bytes) {
  std::ofstream f(ruta, std::ios::binary);
  if (!f.write(static_cast<const char*>(datos), bytes) || !f.flush())
    throw std::runtime_error("no se puede escribir " + ruta.string());
}

// The value nearly every pipeline has: constants through UBOs (nothing else in deko3d) and 1/size of the textures.
constexpr uint32_t kBase = SPEC_CONSTANT_CONSTANTES_UBO | SPEC_CONSTANT_INV_TAMANO_TEX;

// One variant to compile: a library shader, its specialization value and, for a vertex shader, the interpolators
// it keeps (SpirvAGlsl's `salidas`).
struct Variante {
  size_t shader = 0;
  uint32_t especializacion = 0;
  uint32_t salidas = ~0u;
};

std::vector<uint32_t> EspecializacionesPorDefecto(bool vertices) {
  if (vertices) return {kBase};
  std::vector<uint32_t> v = {kBase, 0};
  for (uint32_t f = 0; f < 8; ++f)
    v.push_back(kBase | SPEC_CONSTANT_ALPHA_TEST | (f << SPEC_CONSTANT_ALPHA_FUNC_SHIFT));
  v.push_back(kBase | SPEC_CONSTANT_PCF_BARATO);
  v.push_back(kBase | SPEC_CONSTANT_PCF_BARATO | SPEC_CONSTANT_SOMBRA_MINIMO);
  v.push_back(kBase | SPEC_CONSTANT_SIN_DESENFOQUE);
  return v;
}

// cache/nfsmw_nativo_pipelines.bin (nfsmw_nativo_dibujos.cpp, RegistroPipeline): a 16-byte header {"NFPL", 1, record
// size, count}, then the records. Only the key's especializacion (offset 20) and the two shader fingerprints (80, 88;
// the second is 0 without a pixel shader).
struct ParPipeline {
  uint64_t vs = 0, ps = 0;
  uint32_t especializacion = 0;
  auto operator<=>(const ParPipeline&) const = default;
};
std::set<ParPipeline> LeerLista(const fs::path& ruta) {
  const std::vector<uint8_t> d = Leer(ruta);
  uint32_t cab[4] = {};
  if (d.size() >= 16) std::memcpy(cab, d.data(), 16);
  constexpr uint32_t kRegistro = 424;
  if (cab[0] != 0x4C50464Eu || cab[1] != 1 || cab[2] != kRegistro || d.size() != 16 + size_t(cab[3]) * kRegistro)
    throw std::runtime_error(ruta.string() + " no es una lista de pipelines de la version 1");
  std::set<ParPipeline> pares;
  for (uint32_t i = 0; i < cab[3]; ++i) {
    const uint8_t* r = d.data() + 16 + size_t(i) * kRegistro;
    ParPipeline p;
    std::memcpy(&p.especializacion, r + 20, 4);
    std::memcpy(&p.vs, r + 80, 8);
    std::memcpy(&p.ps, r + 88, 8);
    pares.insert(p);
  }
  return pares;
}

struct Fila {
  std::string shader;
  uint32_t especializacion = 0;
  uint32_t variante = 0;
  bool ok = false;
  double ms = 0;
  size_t bytes = 0;
  uint32_t texturas = 0, ubos = 0;
  std::string motivo;  // conversion error, or uam's log
};

std::string UnaLinea(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 3) {
    std::fprintf(stderr,
                 "uso: nfsmw_dksh <nfsmw_shaders_dk.nfsp> <cache de salida> [--lista FICHERO] [--contenedores DIR]"
                 " [--glsl DIR] [--informe FICHERO.tsv]\n");
    return 1;
  }
  const fs::path biblioteca_ruta = argv[1], salida = argv[2];
  fs::path lista, contenedores, carpeta_glsl, informe;
  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string o = argv[i];
    if (o == "--lista") lista = argv[i + 1];
    else if (o == "--contenedores") contenedores = argv[i + 1];
    else if (o == "--glsl") carpeta_glsl = argv[i + 1];
    else if (o == "--informe") informe = argv[i + 1];
    else throw std::runtime_error("opcion desconocida: " + o);
  }

  nfsmw::native::BibliotecaShaders biblioteca;
  biblioteca.Cargar(biblioteca_ruta);
  const auto& shaders = biblioteca.shaders();
  if (shaders.empty()) throw std::runtime_error("la biblioteca no tiene shaders");

  // Names for the report: the first container (in name order) with each fingerprint.
  std::map<uint64_t, std::string> nombres;
  if (!contenedores.empty()) {
    std::vector<fs::path> rutas;
    for (const auto& e : fs::directory_iterator(contenedores))
      if (e.path().extension() == ".bin") rutas.push_back(e.path());
    std::sort(rutas.begin(), rutas.end());
    for (const auto& r : rutas) {
      const auto d = Leer(r);
      nombres.emplace(XXH3_64bits(d.data(), d.size()), r.stem().string());
    }
  }
  auto nombre = [&](size_t i) {
    const auto it = nombres.find(shaders[i].huella);
    return it != nombres.end() ? it->second : (shaders[i].vertices ? "vs" : "ps") + std::to_string(i);
  };

  std::vector<Variante> trabajo;
  if (!lista.empty()) {
    // Each pipeline's vertex shader keeps what its pixel shader reads, or only the position without one.
    std::map<uint64_t, size_t> por_huella;
    for (size_t i = 0; i < shaders.size(); ++i) por_huella.emplace(shaders[i].huella, i);
    std::map<size_t, uint32_t> lee;  // pixel shader -> its interpolators
    size_t sin_shader = 0;
    for (const ParPipeline& p : LeerLista(lista)) {
      const auto vs = por_huella.find(p.vs);
      const auto ps = p.ps ? por_huella.find(p.ps) : por_huella.end();
      if (vs == por_huella.end() || (p.ps && ps == por_huella.end())) {
        ++sin_shader;
        continue;
      }
      uint32_t salidas = 0;
      if (p.ps) {
        if (!lee.count(ps->second))
          lee[ps->second] = nfsmw::deko::SpirvAGlsl(shaders[ps->second].spirv, 0).interpoladores;
        salidas = lee[ps->second];
        trabajo.push_back({ps->second, p.especializacion, ~0u});
      }
      trabajo.push_back({vs->second, p.especializacion, salidas});
    }
    std::printf("lista: %zu variantes (%zu pipelines con shaders que esta biblioteca no tiene)\n", trabajo.size(),
                sin_shader);
  } else {
    // Without a list, every vertex shader with all its interpolators and with the position only, which checks the
    // removal of unread outputs.
    for (size_t i = 0; i < shaders.size(); ++i)
      for (uint32_t e : EspecializacionesPorDefecto(shaders[i].vertices)) {
        trabajo.push_back({i, e, ~0u});
        if (shaders[i].vertices) trabajo.push_back({i, e, 0});
      }
  }
  if (!carpeta_glsl.empty()) fs::create_directories(carpeta_glsl);

  uam::init(true);
  nfsmw::deko::CacheDksh cache;
  std::vector<Fila> filas;
  std::map<std::string, size_t> avisos_uam;
  size_t avisos_conversion = 0;
  // The key is the value masked to the bits the shader reads (MascaraEspecializacion): variants that differ only
  // in other bits are the same DKSH, compiled once.
  std::vector<uint32_t> mascaras(shaders.size());
  size_t mascaras_erroneas = 0;
  for (size_t i = 0; i < shaders.size(); ++i) {
    mascaras[i] = nfsmw::deko::MascaraEspecializacion(shaders[i].spirv);
    // The same from the GLSL: every line that names the constant, as SPIRV-Cross writes it. A bit read there and
    // missing from the mask would make two different shaders share a DKSH.
    const nfsmw::deko::GlslDeko g = nfsmw::deko::SpirvAGlsl(shaders[i].spirv, 0);
    uint32_t leidos = 0;
    bool raro = false;
    for (size_t at = g.glsl.find("g_SpecConstants"); at != std::string::npos;
         at = g.glsl.find("g_SpecConstants", at + 1)) {
      const size_t ini = g.glsl.rfind('\n', at) + 1;
      const std::string linea = g.glsl.substr(ini, g.glsl.find('\n', at) - ini);
      unsigned valor = 0;
      if (linea.find("const uint g_SpecConstants = ") == 0) continue;
      if (std::sscanf(linea.c_str() + (at - ini), "g_SpecConstants & %uu)", &valor) == 1) {
        leidos |= valor;
      } else if (std::sscanf(linea.c_str() + (at - ini), "g_SpecConstants == %uu)", &valor) == 1 &&
                 (valor & 0x7F000000u)) {
        continue;
      } else {
        raro = true;
      }
    }
    if (mascaras[i] != ~0u && (raro || (leidos & ~mascaras[i]))) {
      ++mascaras_erroneas;
      std::printf("MASCARA ERRONEA %s: SPIR-V %08X, GLSL lee %08X%s\n", nombre(i).c_str(), mascaras[i], leidos,
                  raro ? " y la usa de otra forma" : "");
    }
  }
  std::set<nfsmw::deko::CacheDksh::Clave> hechas;
  for (const Variante& v : trabajo) {
    const auto& s = shaders[v.shader];
    // The cache key: the value masked to what the shader reads, and the kept interpolators of a vertex shader.
    const nfsmw::deko::CacheDksh::Clave clave{nfsmw::deko::HuellaSpirv(s.spirv), v.especializacion & mascaras[v.shader],
                                           s.vertices ? v.salidas : 0u};
    if (!hechas.insert(clave).second) continue;
    Fila f;
    f.shader = nombre(v.shader);
    f.especializacion = clave.especializacion;
    f.variante = clave.variante;
    const auto t0 = std::chrono::steady_clock::now();
    const nfsmw::deko::GlslDeko g = nfsmw::deko::SpirvAGlsl(s.spirv, clave.especializacion, s.vertices ? v.salidas : ~0u);
    if (g.glsl.empty()) {
      f.motivo = "conversion: " + g.error;
    } else {
      f.texturas = g.texturas;
      f.ubos = g.ubos;
      for (const std::string& a : g.avisos) {
        ++avisos_conversion;
        f.motivo += "[conversion] " + a + " ";
      }
      if (!carpeta_glsl.empty()) {
        char sufijo[32];
        std::snprintf(sufijo, sizeof(sufijo), "_%08X_%08X", clave.especializacion, clave.variante);
        const std::string archivo = f.shader + sufijo + (g.vertices ? ".vert" : ".frag");
        Escribir(carpeta_glsl / archivo, g.glsl.data(), g.glsl.size());
      }
      uam::Result r = uam::compile(g.vertices ? uam::Stage::Vertex : uam::Stage::Fragment, g.glsl.c_str());
      f.ok = r.ok;
      f.bytes = r.dksh.size();
      f.motivo += UnaLinea(r.log);
      // uam's lines by kind: without the "0:91(35): " position, up to the first name or number in quotes
      size_t at = 0;
      while (at < r.log.size()) {
        size_t fin = r.log.find('\n', at);
        if (fin == std::string::npos) fin = r.log.size();
        std::string linea = r.log.substr(at, fin - at);
        if (const size_t pos = linea.find("): "); pos != std::string::npos && pos < 16) linea.erase(0, pos + 3);
        const size_t corte = linea.find_first_of("'`\"");
        if (!UnaLinea(linea).empty()) ++avisos_uam[UnaLinea(linea.substr(0, std::min<size_t>(corte, 80)))];
        at = fin + 1;
      }
      if (r.ok) cache.Anadir(clave, std::move(r.dksh));
    }
    f.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    filas.push_back(std::move(f));
  }
  uam::shutdown();

  const std::vector<uint8_t> archivo = cache.Serializar();
  Escribir(salida, archivo.data(), archivo.size());

  size_t fallos = 0;
  std::vector<double> tiempos;
  for (const Fila& f : filas) {
    fallos += !f.ok;
    tiempos.push_back(f.ms);
    if (!f.ok)
      std::printf("FALLO %s %08X %08X: %s\n", f.shader.c_str(), f.especializacion, f.variante, f.motivo.c_str());
  }
  std::sort(tiempos.begin(), tiempos.end());
  auto pct = [&](double p) { return tiempos.empty() ? 0.0 : tiempos[std::min(tiempos.size() - 1, size_t(p * tiempos.size()))]; };
  std::printf("\n%zu variantes de %zu shaders: %zu distintas con la mascara de cada shader, %zu compiladas, %zu fallidas\n",
              trabajo.size(), shaders.size(), filas.size(), filas.size() - fallos, fallos);
  size_t sin_mascara = 0, vs_sin_bits = 0, vs = 0;
  for (size_t i = 0; i < shaders.size(); ++i) {
    sin_mascara += mascaras[i] == ~0u;
    vs += shaders[i].vertices;
    vs_sin_bits += shaders[i].vertices && mascaras[i] == 0;
  }
  std::printf("mascaras: %zu de %zu vertex shaders no leen ningun bit; %zu shaders sin mascara (usos desconocidos)\n",
              vs_sin_bits, vs, sin_mascara);
  std::printf("tiempo por variante (SPIR-V -> GLSL -> DKSH): p50 %.1f ms, p90 %.1f, p99 %.1f, max %.1f\n", pct(0.5),
              pct(0.9), pct(0.99), tiempos.empty() ? 0.0 : tiempos.back());
  std::printf("avisos de la conversion (componente dinamico de un vector de UBO): %zu\n", avisos_conversion);
  std::printf("lineas de uam por tipo:%s\n", avisos_uam.empty() ? " ninguna" : "");
  for (const auto& [tipo, n] : avisos_uam) std::printf("  %6zu  %s\n", n, tipo.c_str());
  std::printf("cache: %zu DKSH, %zu bytes en %s\n  id: %s\n", cache.size(), archivo.size(), salida.string().c_str(),
              nfsmw::deko::IdCompilador().c_str());

  if (!informe.empty()) {
    std::ofstream t(informe);
    t << "shader\tespecializacion\tvariante\tok\tms\tbytes_dksh\ttexturas\tubos\tmotivo\n";
    for (const Fila& f : filas) {
      char linea[128];
      std::snprintf(linea, sizeof(linea), "\t%08X\t%08X\t%d\t%.2f\t%zu\t%08X\t%04X\t", f.especializacion, f.variante,
                    int(f.ok), f.ms, f.bytes, f.texturas, f.ubos);
      t << f.shader << linea << f.motivo << '\n';
    }
  }
  if (mascaras_erroneas) std::printf("%zu mascaras erroneas\n", mascaras_erroneas);
  return fallos || mascaras_erroneas ? 2 : 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "error: %s\n", e.what());
  return 1;
}

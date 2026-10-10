// nfsmw - deko3d renderer: the shader compile worker. See nfsmw_dk_compilador.h.

#include "nfsmw_dk_compilador.h"

#include "nfsmw_dk_nucleo.h"
#include "uam_api.h"

#include <switch.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>

namespace nfsmw::deko {

namespace {
constexpr size_t kPila = 8u << 20;
constexpr int kPrioridad = 0x3B;

// A shader that uses what the game's do (a UBO, a texture, a dynamic loop), to know at start that uam works here.
constexpr char kAutoprueba[] = R"(#version 460
layout(std140, binding = 0) uniform Bloque { vec4 v[4]; } b;
layout(binding = 0) uniform sampler2D t;
layout(location = 0) in vec4 entrada;
layout(location = 0) out vec4 salida;
void main() {
  vec4 r = texture(t, entrada.xy);
  for (int i = 0; i < int(b.v[0].x); ++i) r += b.v[1 + (i & 1)];
  salida = r * b.v[3];
}
)";
}  // namespace

struct Compilador::Datos {
  struct Tarea {
    bool vertices;
    std::string glsl;
    Hecho hecho;
  };
  Thread hilo{};
  std::mutex candado;
  std::condition_variable hay_trabajo;
  std::deque<Tarea> cola;
  bool parar = false;
  uint32_t pendientes = 0;

  static void Bucle(void* yo) { static_cast<Datos*>(yo)->Trabajar(); }

  void Trabajar() {
    const uint64_t t0 = AhoraNs();
    uam::init(true);
    const uam::Result prueba = uam::compile(uam::Stage::Fragment, kAutoprueba);
    if (prueba.ok) {
      Registro("[dk] uam listo: autoprueba compilada en %.1f ms (%zu bytes de DKSH)", (AhoraNs() - t0) / 1e6,
               prueba.dksh.size());
    } else {
      Registro("[dk] uam: AUTOPRUEBA FALLIDA: %s", prueba.log.c_str());
    }
    VaciarRegistro();
    for (;;) {
      Tarea tarea;
      {
        std::unique_lock<std::mutex> cerrojo(candado);
        hay_trabajo.wait(cerrojo, [this] { return parar || !cola.empty(); });
        if (parar) {
          break;
        }
        tarea = std::move(cola.front());
        cola.pop_front();
      }
      uam::Result r = uam::compile(tarea.vertices ? uam::Stage::Vertex : uam::Stage::Fragment, tarea.glsl.c_str());
      tarea.hecho(Resultado{r.ok, std::move(r.dksh), std::move(r.log)});
      std::lock_guard<std::mutex> cerrojo(candado);
      --pendientes;
    }
    uam::shutdown();
  }
};

Compilador::Compilador() : datos_(std::make_unique<Datos>()) {
  if (R_FAILED(threadCreate(&datos_->hilo, Datos::Bucle, datos_.get(), nullptr, kPila, kPrioridad, -2)) ||
      R_FAILED(threadStart(&datos_->hilo))) {
    Fatal("[dk] no se pudo crear el hilo de compilacion de shaders (pila de 8 MiB, prioridad 0x%X)", kPrioridad);
  }
}

Compilador::~Compilador() {
  {
    std::lock_guard<std::mutex> cerrojo(datos_->candado);
    datos_->parar = true;
  }
  datos_->hay_trabajo.notify_all();
  threadWaitForExit(&datos_->hilo);
  threadClose(&datos_->hilo);
}

void Compilador::Encolar(bool vertices, std::string glsl, Hecho hecho) {
  {
    std::lock_guard<std::mutex> cerrojo(datos_->candado);
    datos_->cola.push_back({vertices, std::move(glsl), std::move(hecho)});
    ++datos_->pendientes;
  }
  datos_->hay_trabajo.notify_one();
}

Compilador::Resultado Compilador::Compilar(bool vertices, std::string glsl) {
  std::promise<Resultado> promesa;
  std::future<Resultado> futuro = promesa.get_future();
  Encolar(vertices, std::move(glsl), [&promesa](Resultado r) { promesa.set_value(std::move(r)); });
  return futuro.get();
}

uint32_t Compilador::Pendientes() const {
  std::lock_guard<std::mutex> cerrojo(datos_->candado);
  return datos_->pendientes;
}

}  // namespace nfsmw::deko

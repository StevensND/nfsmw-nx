// nfsmw - deko3d renderer: the shader compile worker (docs/HowToDeko3d.md, section 3.3). Switch only.
//
// uam is not reentrant (Mesa's GLSL front end keeps one static context) and recurses deeply, so every compile in
// the process runs here: one thread with an 8 MB stack at priority 0x3B (0x3C is invalid for an application and
// made thread creation fail with 0xE001 in SwitchWakerHD), sleeping on a condition variable when there is no work.
// A self-test compile at start writes "uam listo" or "AUTOPRUEBA FALLIDA" to the log.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nfsmw::deko {

class Compilador {
 public:
  struct Resultado {
    bool ok = false;
    std::vector<uint8_t> dksh;
    std::string log;  // uam's lines; may be set when ok
  };
  using Hecho = std::function<void(Resultado)>;

  // Starts the thread. Ends the process if it cannot be created.
  Compilador();
  // Stops it after the current compile; queued work that has not started is dropped.
  ~Compilador();
  Compilador(const Compilador&) = delete;
  Compilador& operator=(const Compilador&) = delete;

  // GLSL to DKSH, waiting for the result (the presenter's own shaders, at start).
  Resultado Compilar(bool vertices, std::string glsl);
  // The same without waiting: `hecho` runs on the worker thread.
  void Encolar(bool vertices, std::string glsl, Hecho hecho);
  // Compiles waiting or queued and not finished.
  uint32_t Pendientes() const;

 private:
  struct Datos;
  std::unique_ptr<Datos> datos_;
};

}  // namespace nfsmw::deko

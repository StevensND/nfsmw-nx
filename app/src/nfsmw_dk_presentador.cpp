// nfsmw - deko3d renderer: provider, presenter and immediate drawer. See nfsmw_dk_presentador.h.

#include "nfsmw_dk_presentador.h"

#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ui/surface_switch.h>

#include <switch.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace nfsmw::deko {
namespace {

using rex::ui::Presenter;

// ---- Shaders of the presenter and the drawer (GLSL compiled by uam at start, on the compile worker)

// A rectangle from gl_VertexID (a triangle strip): its corners in clip space (x0, y0, x1, y1, y up) and the texture
// coordinates that go with them.
constexpr char kVsCopia[] = R"(#version 460
layout(std140, binding = 0) uniform Copia { vec4 destino; vec4 origen; } c;
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 t = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  gl_Position = vec4(mix(c.destino.xy, c.destino.zw, t), 0.0, 1.0);
  v_uv = mix(c.origen.xy, c.origen.zw, t);
}
)";
constexpr char kFsCopia[] = R"(#version 460
layout(binding = 0) uniform sampler2D salida;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(salida, v_uv).rgb, 1.0); }
)";
// Dear ImGui's vertices: position in the drawer's coordinate space (y down), texture coordinate, RGBA8 color.
constexpr char kVsUi[] = R"(#version 460
layout(std140, binding = 0) uniform Proyeccion { vec4 escala_desplazamiento; } p;
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
  gl_Position = vec4(a_pos * p.escala_desplazamiento.xy + p.escala_desplazamiento.zw, 0.0, 1.0);
  v_uv = a_uv;
  v_color = a_color;
}
)";
constexpr char kFsUi[] = R"(#version 460
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, v_uv) * v_color; }
)";

bool CompilarPropio(ProveedorDeko3d& proveedor, Nucleo& nucleo, DkShader& shader, bool vertices, const char* glsl,
                    const char* nombre) {
  Compilador::Resultado r = proveedor.compilador().Compilar(vertices, glsl);
  if (!r.ok) {
    Registro("[dk] el shader propio %s no compila: %s", nombre, r.log.c_str());
    return false;
  }
  return nucleo.CargarShader(shader, r.dksh, nombre);
}

void EstadoSinProfundidad(DkCmdBuf c, bool mezcla) {
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkColorStateSetBlendEnable(&cs, 0, mezcla);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  DkBlendState bs;
  dkBlendStateDefaults(&bs);
  bs.srcColorBlendFactor = DkBlendFactor_SrcAlpha;
  bs.dstColorBlendFactor = DkBlendFactor_InvSrcAlpha;
  bs.srcAlphaBlendFactor = DkBlendFactor_One;
  bs.dstAlphaBlendFactor = DkBlendFactor_InvSrcAlpha;
  dkCmdBufBindRasterizerState(c, &rs);
  dkCmdBufBindColorState(c, &cs);
  dkCmdBufBindColorWriteState(c, &cws);
  dkCmdBufBindDepthStencilState(c, &ds);
  dkCmdBufBindBlendState(c, 0, &bs);
}

void Encuadre(DkCmdBuf c, uint32_t ancho, uint32_t alto) {
  const DkViewport vp = {0, 0, float(ancho), float(alto), 0, 1};
  const DkScissor tijera = {0, 0, ancho, alto};
  dkCmdBufSetViewports(c, 0, &vp, 1);
  dkCmdBufSetScissors(c, 0, &tijera, 1);
}

DkSamplerDescriptor DescriptorSampler(DkFilter filtro, DkWrapMode envoltura) {
  DkSampler s;
  dkSamplerDefaults(&s);
  s.minFilter = s.magFilter = filtro;
  s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = envoltura;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  return d;
}

// The UI drawers paint the swapchain image the presenter has just painted the guest output into.
class ContextoUIDeko3d final : public rex::ui::UIDrawContext {
 public:
  ContextoUIDeko3d(Presenter& presentador, uint32_t ancho, uint32_t alto, DkImage& destino)
      : UIDrawContext(presentador, ancho, alto), destino(destino) {}
  DkImage& destino;
};

// ---- The presenter

class PresentadorDeko3d final : public Presenter {
 public:
  PresentadorDeko3d(ProveedorDeko3d& proveedor, HostGpuLossCallback perdida)
      : Presenter(perdida), proveedor_(proveedor) {}

  ~PresentadorDeko3d() override {
    ShutdownPaintThread();  // its thread calls PaintAndPresentImpl (presenter.h)
    {
      std::lock_guard<std::mutex> cerrojo(captura_candado_);
      captura_.Destruir();
    }
    cadena_.Destruir();
    proveedor_.dispositivo().EsperarInactiva();
    for (ImagenSalida& s : salidas_) {
      proveedor_.dispositivo().DestruirBloque(s.memoria);
      s.memoria = nullptr;
    }
    nucleo_.Destruir();
  }

  bool Inicializar() {
    ConfigNucleo config;
    config.ranuras = 3;
    config.mb_subida = 1;
    config.kb_comandos = 256;
    config.mb_codigo = 1;
    config.mb_trozo_imagenes = 1;
    config.descriptores_imagen = kGuestOutputMailboxSize;
    config.descriptores_sampler = 1;
    config.kb_consultas = 4;
    nucleo_.Crear(proveedor_.dispositivo(), config);
    if (!CompilarPropio(proveedor_, nucleo_, vs_, true, kVsCopia, "presentador_vs") ||
        !CompilarPropio(proveedor_, nucleo_, fs_, false, kFsCopia, "presentador_fs")) {
      return false;
    }
    // The sampler of the guest output, once
    nucleo_.EmpezarTrabajo(++trabajo_);
    const DkSamplerDescriptor sampler = DescriptorSampler(DkFilter_Linear, DkWrapMode_ClampToEdge);
    dkCmdBufPushData(nucleo_.comandos(), nucleo_.DescriptoresSampler(), &sampler, sizeof(sampler));
    nucleo_.TerminarTrabajo();
    return InitializeCommonSurfaceIndependent();
  }

  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override {
    return rex::ui::Surface::kTypeFlag_ViNWindow;
  }

  // The PNG captures (nfsmw_captura_cada_s): the latest guest output, copied to CPU memory with its own command
  // buffer (any thread may call this, Presenter's contract) and turned from RGB10A2 into R8 G8 B8 X8.
  bool CaptureGuestOutput(rex::ui::RawImage& image_out) override {
    std::lock_guard<std::mutex> cerrojo(captura_candado_);
    uint32_t indice;
    GuestOutputProperties propiedades;
    std::unique_lock<std::mutex> consumidor(ConsumeGuestOutput(indice, &propiedades, nullptr));
    if (indice == UINT32_MAX) {
      return false;
    }
    ImagenSalida& s = salidas_[indice];
    const uint32_t ancho = std::min(propiedades.frontbuffer_width, s.ancho);
    const uint32_t alto = std::min(propiedades.frontbuffer_height, s.alto);
    if (!s.memoria || !ancho || !alto) {
      return false;
    }
    const uint32_t bytes = ancho * alto * 4;
    if (!captura_.dispositivo()) {
      ConfigNucleo config;
      config.ranuras = 1;
      config.mb_subida = 1;
      config.kb_comandos = 64;
      config.mb_codigo = 1;
      config.mb_trozo_imagenes = 1;
      config.descriptores_imagen = 1;
      config.descriptores_sampler = 1;
      config.kb_consultas = 4;
      captura_.Crear(proveedor_.dispositivo(), config);
    }
    if (captura_bytes_ < bytes) {
      // A read-back block (CPU-cached, GPU-uncached); the old one goes when the Nucleo does.
      captura_bytes_ = (bytes + 0xFFFF) & ~0xFFFFu;
      captura_bloque_ = captura_.CrearBloqueLectura(captura_bytes_, "captura de la salida");
    }
    dkMemBlockFlushCpuCache(captura_bloque_, 0, bytes);  // before the GPU writes it
    captura_.EmpezarTrabajo(++captura_trabajo_);
    DkCmdBuf c = captura_.comandos();
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image);  // what the ring thread wrote into it
    DkImageView v;
    dkImageViewDefaults(&v, &s.imagen);
    const DkImageRect rect = {0, 0, 0, ancho, alto, 1};
    const DkCopyBuf destino = {dkMemBlockGetGpuAddr(captura_bloque_), ancho * 4, ancho * 4 * alto};
    dkCmdBufCopyImageToBuffer(c, &v, &rect, &destino, 0);
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    captura_.TerminarTrabajo();
    captura_.EsperarGpu();
    consumidor.unlock();
    dkMemBlockFlushCpuCache(captura_bloque_, 0, bytes);  // and before the CPU reads it
    const uint32_t* texeles = static_cast<const uint32_t*>(dkMemBlockGetCpuAddr(captura_bloque_));
    image_out.width = ancho;
    image_out.height = alto;
    image_out.stride = size_t(ancho) * 4;
    image_out.data.resize(size_t(bytes));
    uint8_t* salida = image_out.data.data();
    for (uint32_t i = 0; i < ancho * alto; ++i) {
      const uint32_t t = texeles[i];
      salida[i * 4 + 0] = uint8_t(((t & 0x3FF) * 255 + 511) / 1023);
      salida[i * 4 + 1] = uint8_t((((t >> 10) & 0x3FF) * 255 + 511) / 1023);
      salida[i * 4 + 2] = uint8_t((((t >> 20) & 0x3FF) * 255 + 511) / 1023);
      salida[i * 4 + 3] = 0xFF;
    }
    return true;
  }

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      rex::ui::Surface& superficie, uint32_t ancho, uint32_t alto, bool was_paintable,
      bool& is_vsync_implicit_out) override {
    (void)was_paintable;
    is_vsync_implicit_out = false;  // swap interval 0, as the Vulkan path's IMMEDIATE
    if (superficie.GetType() != rex::ui::Surface::kTypeIndex_ViNWindow) {
      return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    NWindow* ventana = static_cast<rex::ui::ViNWindowSurface&>(superficie).window();
    if (cadena_.creada() && ventana == ventana_ && ancho == cadena_.ancho() && alto == cadena_.alto()) {
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    cadena_.Destruir();
    ventana_ = ventana;
    cadena_.Crear(proveedor_.dispositivo(), ancho, alto, 0, ventana);
    REXLOG_INFO("[deko3d] presentador: cadena de {}x{} en la ventana del sistema", ancho, alto);
    return SurfacePaintConnectResult::kSuccess;
  }

  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override { cadena_.Destruir(); }

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t ancho, uint32_t alto,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override {
    ImagenSalida& s = salidas_[mailbox_index];
    if (!s.memoria || s.ancho != ancho || s.alto != alto) {
      Dispositivo& d = proveedor_.dispositivo();
      if (s.memoria) {
        // Another size: the GPU may still be writing or reading the old image
        d.EsperarInactiva();
        d.DestruirBloque(s.memoria);
        s.memoria = nullptr;
      }
      DkImageLayoutMaker m;
      dkImageLayoutMakerDefaults(&m, d.dispositivo());
      m.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
      m.format = kFormatoSalida;
      m.dimensions[0] = ancho;
      m.dimensions[1] = alto;
      ArreglarTamanoDeTesela(m, alto);
      DkImageLayout disposicion;
      dkImageLayoutInitialize(&disposicion, &m);
      char nombre[48];
      std::snprintf(nombre, sizeof(nombre), "salida del juego %u (%ux%u)", mailbox_index, ancho, alto);
      s.memoria = d.CrearBloque(uint32_t(dkImageLayoutGetSize(&disposicion)),
                                DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, nombre);
      dkImageInitialize(&s.imagen, &disposicion, s.memoria, 0);
      s.ancho = ancho;
      s.alto = alto;
      s.version = siguiente_version_++;
    }
    ContextoSalidaDeko3d contexto(is_8bpc_out_ref, s.imagen, ancho, alto, s.version);
    return refresher(contexto);
  }

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override {
    if (!cadena_.creada()) {
      return PaintResult::kNotPresentedConnectionOutdated;
    }
    const int imagen = cadena_.Adquirir();
    const uint32_t ancho = cadena_.ancho(), alto = cadena_.alto();
    nucleo_.EmpezarTrabajo(++trabajo_);
    DkCmdBuf c = nucleo_.comandos();
    nucleo_.BindDescriptores();
    nucleo_.InvalidarCodigoSiHaceFalta();
    DkImageView vista;
    dkImageViewDefaults(&vista, &cadena_.Imagen(imagen));
    dkCmdBufBindRenderTarget(c, &vista, nullptr);
    Encuadre(c, ancho, alto);
    dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, 0, 0, 0, 1);  // the letterbox

    GuestOutputProperties propiedades;
    GuestOutputPaintConfig configuracion;
    uint32_t indice;
    {
      std::unique_lock<std::mutex> consumidor(ConsumeGuestOutput(indice, &propiedades, &configuracion));
    }
    // The mailbox does not give the refresher this image again until a newer one has been acquired, and an image
    // is only destroyed (a new size) after the GPU is idle: it can be used after leaving the lock.
    if (indice != UINT32_MAX) {
      ImagenSalida& s = salidas_[indice];
      const GuestOutputPaintFlow flujo =
          GetGuestOutputPaintFlow(propiedades, ancho, alto, 16384, 16384, configuracion);
      if (flujo.effect_count && s.memoria) {
        uint32_t invalidar = DkInvalidateFlags_Image;
        if (s.version_descriptor != s.version) {
          DkImageView v;
          dkImageViewDefaults(&v, &s.imagen);
          DkImageDescriptor d;
          dkImageDescriptorInitialize(&d, &v, false, false);
          dkCmdBufPushData(c, nucleo_.DescriptoresImagen() + indice * sizeof(d), &d, sizeof(d));
          s.version_descriptor = s.version;
          invalidar |= DkInvalidateFlags_Descriptors;
        }
        // The ring thread's writes into the image end before it is sampled (the GPU overlaps submissions)
        dkCmdBufBarrier(c, DkBarrier_Fragments, invalidar);
        const auto [salida_ancho, salida_alto] = flujo.effect_output_sizes[flujo.effect_count - 1];
        const float x0 = float(flujo.output_x) / float(ancho) * 2 - 1;
        const float x1 = float(flujo.output_x + int32_t(salida_ancho)) / float(ancho) * 2 - 1;
        const float y0 = 1 - float(flujo.output_y) / float(alto) * 2;
        const float y1 = 1 - float(flujo.output_y + int32_t(salida_alto)) / float(alto) * 2;
        const float u1 = float(propiedades.frontbuffer_width) / float(s.ancho);
        const float v1 = float(propiedades.frontbuffer_height) / float(s.alto);
        const float datos[8] = {x0, y0, x1, y1, 0, 0, u1, v1};
        const Subida ubo = nucleo_.Reservar(sizeof(datos), DK_UNIFORM_BUF_ALIGNMENT);
        if (ubo) {
          std::memcpy(ubo.cpu, datos, sizeof(datos));
          EstadoSinProfundidad(c, false);
          dkCmdBufBindVtxAttribState(c, nullptr, 0);
          dkCmdBufBindVtxBufferState(c, nullptr, 0);
          const DkShader* par[] = {&vs_, &fs_};
          dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, par, 2);
          dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 0, ubo.gpu, sizeof(datos));
          const DkResHandle h = dkMakeTextureHandle(indice, 0);
          dkCmdBufBindTextures(c, DkStage_Fragment, 0, &h, 1);
          dkCmdBufDraw(c, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        }
      }
    }
    nucleo_.TerminarTrabajo();

    if (execute_ui_drawers) {
      ContextoUIDeko3d contexto(*this, ancho, alto, cadena_.Imagen(imagen));
      ExecuteUIDrawersFromUIThread(contexto);
    }
    cadena_.Presentar(imagen);
    return PaintResult::kPresented;
  }

 private:
  struct ImagenSalida {
    DkMemBlock memoria = nullptr;
    DkImage imagen{};
    uint32_t ancho = 0, alto = 0;
    uint64_t version = 0;
    uint64_t version_descriptor = 0;  // the image whose descriptor is in the table (paint thread only)
  };

  ProveedorDeko3d& proveedor_;
  Nucleo nucleo_;  // the painting thread's
  Presentacion cadena_;
  NWindow* ventana_ = nullptr;
  DkShader vs_{}, fs_{};
  uint64_t trabajo_ = 0;
  std::array<ImagenSalida, kGuestOutputMailboxSize> salidas_{};
  uint64_t siguiente_version_ = 1;
  // CaptureGuestOutput's own recorder and read-back memory
  std::mutex captura_candado_;
  Nucleo captura_;
  DkMemBlock captura_bloque_ = nullptr;
  uint32_t captura_bytes_ = 0;
  uint64_t captura_trabajo_ = 0;
};

// ---- Dear ImGui

class DibujanteDeko3d;

class TexturaDeko3d final : public rex::ui::ImmediateTexture {
 public:
  TexturaDeko3d(DibujanteDeko3d& dueno, uint32_t ancho, uint32_t alto)
      : ImmediateTexture(ancho, alto), dueno(dueno) {}
  ~TexturaDeko3d() override;
  DibujanteDeko3d& dueno;
  uint32_t ranura = 0;   // image descriptor
  uint32_t sampler = 0;  // sampler descriptor
  BloqueImagen memoria;
  DkImage imagen{};
};

class DibujanteDeko3d final : public rex::ui::ImmediateDrawer {
 public:
  // Samplers 0-3: nearest/linear x clamp/repeat. Image descriptor 0: a white texel for draws without a texture.
  static constexpr uint32_t kTexturas = 256;

  explicit DibujanteDeko3d(ProveedorDeko3d& proveedor) : proveedor_(proveedor) {}

  ~DibujanteDeko3d() override { nucleo_.Destruir(); }

  bool Inicializar() {
    ConfigNucleo config;
    config.ranuras = 3;
    config.mb_subida = 4;
    config.kb_comandos = 256;
    config.mb_codigo = 1;
    config.mb_trozo_imagenes = 8;
    config.descriptores_imagen = kTexturas;
    config.descriptores_sampler = 4;
    config.kb_consultas = 4;
    nucleo_.Crear(proveedor_.dispositivo(), config);
    if (!CompilarPropio(proveedor_, nucleo_, vs_, true, kVsUi, "ui_vs") ||
        !CompilarPropio(proveedor_, nucleo_, fs_, false, kFsUi, "ui_fs")) {
      return false;
    }
    for (uint32_t i = kTexturas; i-- > 1;) {
      libres_.push_back(i);
    }
    nucleo_.EmpezarTrabajo(++trabajo_);
    const DkSamplerDescriptor samplers[4] = {
        DescriptorSampler(DkFilter_Nearest, DkWrapMode_ClampToEdge),
        DescriptorSampler(DkFilter_Linear, DkWrapMode_ClampToEdge),
        DescriptorSampler(DkFilter_Nearest, DkWrapMode_Repeat),
        DescriptorSampler(DkFilter_Linear, DkWrapMode_Repeat)};
    dkCmdBufPushData(nucleo_.comandos(), nucleo_.DescriptoresSampler(), samplers, sizeof(samplers));
    const uint32_t blanco = 0xFFFFFFFFu;
    Subir(blanco_, 0, 1, 1, reinterpret_cast<const uint8_t*>(&blanco));
    nucleo_.TerminarTrabajo();
    return true;
  }

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t ancho, uint32_t alto,
                                                           rex::ui::ImmediateTextureFilter filtro, bool repetida,
                                                           const uint8_t* datos) override {
    if (libres_.empty() || !ancho || !alto) {
      REXLOG_ERROR("[deko3d] dibujante: no quedan descriptores para una textura de {}x{}", ancho, alto);
      return nullptr;
    }
    auto t = std::make_unique<TexturaDeko3d>(*this, ancho, alto);
    t->ranura = libres_.back();
    libres_.pop_back();
    t->sampler = (filtro == rex::ui::ImmediateTextureFilter::kLinear ? 1 : 0) + (repetida ? 2 : 0);
    // In the middle of a frame (ImGui uploads its font on the first draw) the copy goes into that frame's
    // commands, before its draws; otherwise in a work of its own.
    const bool propia = !nucleo_.TrabajoAbierto();
    if (propia) {
      nucleo_.EmpezarTrabajo(++trabajo_);
    }
    Subir(*t, t->ranura, ancho, alto, datos);
    if (propia) {
      nucleo_.TerminarTrabajo();
    }
    return t;
  }

  TexturaDeko3d& blanco() { return blanco_; }

  void Liberar(TexturaDeko3d& t) {
    nucleo_.LiberarImagenLuego(t.memoria);
    libres_.push_back(t.ranura);  // reused by a later texture: its descriptor is written in a later work
  }

  void Begin(rex::ui::UIDrawContext& contexto, float ancho_espacio, float alto_espacio) override {
    ImmediateDrawer::Begin(contexto, ancho_espacio, alto_espacio);
    auto& c = static_cast<ContextoUIDeko3d&>(contexto);
    nucleo_.EmpezarTrabajo(++trabajo_);
    DkCmdBuf cmd = nucleo_.comandos();
    nucleo_.BindDescriptores();
    nucleo_.InvalidarCodigoSiHaceFalta();
    DkImageView vista;
    dkImageViewDefaults(&vista, &c.destino);
    dkCmdBufBindRenderTarget(cmd, &vista, nullptr);
    Encuadre(cmd, c.render_target_width(), c.render_target_height());
    EstadoSinProfundidad(cmd, true);
    const DkShader* par[] = {&vs_, &fs_};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, par, 2);
    const DkVtxAttribState atributos[3] = {
        {0, 0, offsetof(rex::ui::ImmediateVertex, x), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(rex::ui::ImmediateVertex, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(rex::ui::ImmediateVertex, color), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0}};
    const DkVtxBufferState bufer = {sizeof(rex::ui::ImmediateVertex), 0};
    dkCmdBufBindVtxAttribState(cmd, atributos, 3);
    dkCmdBufBindVtxBufferState(cmd, &bufer, 1);
    // From the coordinate space (y down) to clip space (y up)
    const float proyeccion[4] = {2.0f / ancho_espacio, -2.0f / alto_espacio, -1.0f, 1.0f};
    const Subida ubo = nucleo_.Reservar(sizeof(proyeccion), DK_UNIFORM_BUF_ALIGNMENT);
    if (ubo) {
      std::memcpy(ubo.cpu, proyeccion, sizeof(proyeccion));
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, ubo.gpu, sizeof(proyeccion));
    }
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& lote) override {
    lote_valido_ = false;
    con_indices_ = lote.indices && lote.index_count > 0;
    const uint32_t bytes_v = uint32_t(lote.vertex_count) * sizeof(rex::ui::ImmediateVertex);
    const uint32_t bytes_i = con_indices_ ? uint32_t(lote.index_count) * sizeof(uint16_t) : 0;
    const Subida v = nucleo_.Reservar(bytes_v, 16);
    const Subida i = bytes_i ? nucleo_.Reservar(bytes_i, 16) : Subida{};
    if (!v || (bytes_i && !i)) {
      return;
    }
    std::memcpy(v.cpu, lote.vertices, bytes_v);
    dkCmdBufBindVtxBuffer(nucleo_.comandos(), 0, v.gpu, bytes_v);
    if (bytes_i) {
      std::memcpy(i.cpu, lote.indices, bytes_i);
      dkCmdBufBindIdxBuffer(nucleo_.comandos(), DkIdxFormat_Uint16, i.gpu);
    }
    lote_valido_ = true;
  }

  void Draw(const rex::ui::ImmediateDraw& dibujo) override {
    if (!lote_valido_ || dibujo.count <= 0) {
      return;
    }
    uint32_t x, y, w, h;
    if (!ScissorToRenderTarget(dibujo, x, y, w, h)) {
      return;
    }
    DkCmdBuf cmd = nucleo_.comandos();
    const DkScissor tijera = {x, y, w, h};
    dkCmdBufSetScissors(cmd, 0, &tijera, 1);
    const auto* t = static_cast<const TexturaDeko3d*>(dibujo.texture);
    const DkResHandle handle = t ? dkMakeTextureHandle(t->ranura, t->sampler) : dkMakeTextureHandle(0, 0);
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &handle, 1);
    const DkPrimitive primitiva =
        dibujo.primitive_type == rex::ui::ImmediatePrimitiveType::kLines ? DkPrimitive_Lines : DkPrimitive_Triangles;
    if (con_indices_) {
      dkCmdBufDrawIndexed(cmd, primitiva, uint32_t(dibujo.count), 1, uint32_t(dibujo.index_offset),
                          dibujo.base_vertex, 0);
    } else {
      dkCmdBufDraw(cmd, primitiva, uint32_t(dibujo.count), 1, uint32_t(dibujo.base_vertex), 0);
    }
  }

  void EndDrawBatch() override { lote_valido_ = false; }

  void End() override {
    nucleo_.TerminarTrabajo();
    ImmediateDrawer::End();
  }

 private:
  // Records the upload of RGBA8 texels and the image's descriptor into the open work.
  void Subir(TexturaDeko3d& t, uint32_t ranura, uint32_t ancho, uint32_t alto, const uint8_t* datos) {
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, nucleo_.dispositivo());
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = ancho;
    m.dimensions[1] = alto;
    ArreglarTamanoDeTesela(m, alto);
    DkImageLayout disposicion;
    dkImageLayoutInitialize(&disposicion, &m);
    t.memoria = nucleo_.ReservarImagen(uint32_t(dkImageLayoutGetSize(&disposicion)),
                                       dkImageLayoutGetAlignment(&disposicion));
    dkImageInitialize(&t.imagen, &disposicion, t.memoria.bloque, t.memoria.offset);
    DkCmdBuf cmd = nucleo_.comandos();
    const uint32_t bytes = ancho * alto * 4;
    const Subida s = nucleo_.Reservar(bytes, 256);
    if (s && datos) {
      std::memcpy(s.cpu, datos, bytes);
      DkImageView vista;
      dkImageViewDefaults(&vista, &t.imagen);
      const DkCopyBuf origen = {s.gpu, 0, 0};
      const DkImageRect rect = {0, 0, 0, ancho, alto, 1};
      dkCmdBufCopyBufferToImage(cmd, &origen, &vista, &rect, 0);
    }
    DkImageView vista;
    dkImageViewDefaults(&vista, &t.imagen);
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &vista, false, false);
    dkCmdBufPushData(cmd, nucleo_.DescriptoresImagen() + ranura * sizeof(d), &d, sizeof(d));
    // the copy (copy engine) and the descriptor before any draw samples it
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  }

  ProveedorDeko3d& proveedor_;
  Nucleo nucleo_;  // the UI thread's
  DkShader vs_{}, fs_{};
  TexturaDeko3d blanco_{*this, 1, 1};
  std::vector<uint32_t> libres_;
  uint64_t trabajo_ = 0;
  bool lote_valido_ = false;
  bool con_indices_ = false;
};

TexturaDeko3d::~TexturaDeko3d() {
  if (this != &dueno.blanco()) {
    dueno.Liberar(*this);
  }
}

}  // namespace

// ---- The provider

std::unique_ptr<ProveedorDeko3d> ProveedorDeko3d::Crear() {
  // The core's lines (device, memory blocks, uam, errors) into the game's log, logs/nfsmw_NNN.log next to the NRO.
  // A deko3d failure ends the process: the core flushes before every creation and before Fatal.
  FijarRegistro([](const char* linea) { REXLOG_INFO("{}", linea); }, [] { rex::FlushLogging(); });
  std::unique_ptr<ProveedorDeko3d> p(new ProveedorDeko3d());
  p->dispositivo_.Crear(true);
  p->compilador_ = std::make_unique<Compilador>();
  return p;
}

ProveedorDeko3d::~ProveedorDeko3d() {
  compilador_.reset();
  dispositivo_.Destruir();
}

std::unique_ptr<rex::ui::Presenter> ProveedorDeko3d::CreatePresenter(
    rex::ui::Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  auto p = std::make_unique<PresentadorDeko3d>(*this, host_gpu_loss_callback);
  if (!p->Inicializar()) {
    REXLOG_ERROR("[deko3d] No se pudo inicializar el presentador");
    return nullptr;
  }
  return p;
}

std::unique_ptr<rex::ui::ImmediateDrawer> ProveedorDeko3d::CreateImmediateDrawer() {
  auto d = std::make_unique<DibujanteDeko3d>(*this);
  if (!d->Inicializar()) {
    REXLOG_ERROR("[deko3d] No se pudo inicializar el dibujante de la interfaz");
    return nullptr;
  }
  return d;
}

}  // namespace nfsmw::deko

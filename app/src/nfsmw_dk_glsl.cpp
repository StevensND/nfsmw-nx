// nfsmw - deko3d renderer: SPIR-V of the deko3d library to GLSL for uam. See nfsmw_dk_glsl.h.

#include "nfsmw_dk_glsl.h"

#include <spirv_glsl.hpp>

#include <cctype>
#include <exception>

namespace nfsmw::deko {
namespace {

// What the deko3d SPIR-V must not ask for: uam's front end (Mesa 19's GLSL) has none of it. Each would mean that
// something compiled without -DNFSMW_DEKO3D, or that shader_common.h's deko3d branch let a path through.
constexpr const char* kExtensionesProhibidas[] = {
    "GL_EXT_buffer_reference", "GL_ARB_gpu_shader_int64", "GL_EXT_shader_explicit_arithmetic_types",
    "GL_EXT_nonuniform_qualifier", "GL_EXT_samplerless_texture_functions",
};

// The index after `g_UboX.v[...]`, when there is one and it is not an integer literal: a dynamic component of a
// UBO vector, which uam reads as .x every time (HowToDeko3d.md, section 6, item 12). XenosRecomp's constants
// always index with literals; this guards against a new helper or rewrite that does not.
void BuscarComponentesDinamicos(const std::string& glsl, std::vector<std::string>& avisos) {
  for (size_t at = glsl.find("g_Ubo"); at != std::string::npos; at = glsl.find("g_Ubo", at + 1)) {
    size_t i = glsl.find(".v[", at);
    if (i == std::string::npos || i > at + 24) {
      continue;
    }
    // the closing bracket of the vector index, with nested brackets
    i += 3;
    int profundidad = 1;
    while (i < glsl.size() && profundidad) {
      profundidad += glsl[i] == '[' ? 1 : glsl[i] == ']' ? -1 : 0;
      ++i;
    }
    if (i >= glsl.size() || glsl[i] != '[') {
      continue;
    }
    const size_t fin = glsl.find(']', i);
    const std::string indice = glsl.substr(i + 1, fin == std::string::npos ? 0 : fin - i - 1);
    bool literal = !indice.empty();
    for (char c : indice) {
      literal &= std::isdigit(static_cast<unsigned char>(c)) || c == 'u';
    }
    if (!literal) {
      const size_t linea = glsl.rfind('\n', at) + 1;
      avisos.push_back("componente dinamico de un vector de UBO: " +
                       glsl.substr(linea, glsl.find('\n', at) - linea));
    }
  }
}

// Whether word k of a SPIR-V instruction (k = 0 is the opcode word) is a literal rather than an id. Before the first
// OpFunction only OpTypeArray's length, OpSpecConstantComposite's members and OpVariable's initializer can name a
// constant (OpSpecConstantOp is handled by the caller): everything else there is a literal, a type or a variable.
// In functions, the literal operands of the instructions DXC writes; any other position counts as an id.
bool OperandoLiteral(uint32_t op, size_t k, bool en_funciones) {
  if (op == 5 || op == 6 || op == 71 || op == 72 || op == 8) {  // OpName, OpMemberName, OpDecorate, ..., OpLine
    return true;
  }
  if (!en_funciones) {
    switch (op) {
      case 28: return k != 3;  // OpTypeArray: its length
      case 45: return k < 3;   // OpSpecConstantComposite
      case 59: return k != 4;  // OpVariable: its initializer
      default: return true;
    }
  }
  switch (op) {
    case 54: return k == 3;                      // OpFunction: function control
    case 59: return k == 3;                      // OpVariable: storage class
    case 12: return k == 4;                      // OpExtInst: instruction number
    case 61: return k >= 4;                      // OpLoad: memory access
    case 62: case 63: return k >= 3;             // OpStore, OpCopyMemory: memory access
    case 79: return k >= 5;                      // OpVectorShuffle: components
    case 81: return k >= 4;                      // OpCompositeExtract: indexes
    case 82: return k >= 5;                      // OpCompositeInsert: indexes
    case 246: return k >= 3;                     // OpLoopMerge: loop control
    case 247: return k >= 2;                     // OpSelectionMerge: selection control
    case 250: return k >= 4;                     // OpBranchConditional: weights
    case 251: return k >= 3 && (k - 3) % 2 == 0;  // OpSwitch: case values
    // Image instructions: their image operands mask (the ids after it are operands)
    case 87: case 88: case 91: case 92: case 95: case 98: return k == 5;
    case 89: case 90: case 93: case 94: case 96: case 97: return k == 6;
    case 99: return k == 4;  // OpImageWrite
    default: return false;
  }
}

// The vertex shader outputs at the `quitar` locations as plain globals: `layout(location = N) out vec4 x;` becomes
// `vec4 x;`. Nothing reads them, so Mesa's front end in uam drops their writes and everything that only fed them.
// (SPIRV-Cross's mask_stage_output_by_location only acts in its MSL backend.) false if a declaration is not there
// in that form.
bool QuitarSalidas(std::string& glsl, uint32_t quitar, std::string& error) {
  for (uint32_t ubicacion = 0; ubicacion < 32; ++ubicacion) {
    if (!(quitar & (1u << ubicacion))) {
      continue;
    }
    const std::string prefijo = "layout(location = " + std::to_string(ubicacion) + ") out ";
    size_t at = glsl.find("\n" + prefijo);
    if (at == std::string::npos) {
      error = "no se encuentra la salida en la ubicacion " + std::to_string(ubicacion);
      return false;
    }
    ++at;
    const size_t fin = glsl.find(';', at);
    const std::string resto = glsl.substr(at + prefijo.size(), fin - at - prefijo.size());  // "vec4 name"
    if (fin == std::string::npos || resto.find(' ') == std::string::npos || resto.find('(') != std::string::npos ||
        resto.rfind("flat ", 0) == 0 || resto.rfind("noperspective ", 0) == 0) {
      error = "salida con una forma inesperada: " + glsl.substr(at, fin - at);
      return false;
    }
    glsl.replace(at, fin - at, resto);
  }
  return true;
}

}  // namespace

uint32_t MascaraEspecializacion(std::span<const uint32_t> espirv) {
  constexpr uint32_t kOpConstant = 43, kOpSpecConstantOp = 52, kOpFunction = 54, kOpDecorate = 71,
                     kOpIEqual = 170, kOpINotEqual = 171, kOpBitwiseAnd = 199, kDecorationSpecId = 1;
  constexpr uint32_t kTodo = ~0u;
  // Bits a value of the renderer never has (bits 24-30 of shader_common.h): a comparison with a literal that has
  // any of them is always false.
  constexpr uint32_t kBitsNuncaUsados = 0x7F000000u;
  if (espirv.size() < 5 || espirv[0] != 0x07230203u) {
    return kTodo;
  }
  auto recorrer = [&](auto&& hacer) {
    for (size_t i = 5; i < espirv.size();) {
      const uint32_t n = espirv[i] >> 16;
      if (n == 0 || i + n > espirv.size()) {
        return false;
      }
      hacer(espirv[i] & 0xFFFFu, espirv.subspan(i, n));
      i += n;
    }
    return true;
  };
  uint32_t constante = 0;  // the id of constant_id 0
  std::vector<std::pair<uint32_t, uint32_t>> literales;  // OpConstant id, 32-bit value
  if (!recorrer([&](uint32_t op, std::span<const uint32_t> w) {
        if (op == kOpDecorate && w.size() == 4 && w[2] == kDecorationSpecId && w[3] == 0) {
          constante = w[1];
        } else if (op == kOpConstant && w.size() == 4) {
          literales.push_back({w[2], w[3]});
        }
      })) {
    return kTodo;
  }
  if (!constante) {
    return 0;
  }
  auto literal = [&](uint32_t id, uint32_t& valor) {
    for (const auto& [i, v] : literales) {
      if (i == id) {
        valor = v;
        return true;
      }
    }
    return false;
  };
  uint32_t mascara = 0;
  bool desconocido = false;
  bool en_funciones = false;  // past the first OpFunction: the module's global section is over
  recorrer([&](uint32_t op, std::span<const uint32_t> w) {
    en_funciones |= op == kOpFunction;
    if (op == kOpSpecConstantOp && w.size() == 6 && (w[4] == constante || w[5] == constante)) {
      uint32_t valor;
      if (!literal(w[4] == constante ? w[5] : w[4], valor)) {
        desconocido = true;
      } else if (w[3] == kOpBitwiseAnd) {
        mascara |= valor;
      } else if (!((w[3] == kOpIEqual || w[3] == kOpINotEqual) && (valor & kBitsNuncaUsados))) {
        desconocido = true;
      }
      return;
    }
    // Any other operand that is the constant's id: a use this does not understand. Literal operands are skipped
    // where they are known to be (a literal that happens to equal the id, a type width or a switch case, would
    // only cost the masking; an operand wrongly taken as a literal would be a wrong DKSH, so only those listed).
    for (size_t k = 1; k < w.size(); ++k) {
      if (w[k] == constante && !OperandoLiteral(op, k, en_funciones)) {
        desconocido = true;
      }
    }
  });
  return desconocido ? kTodo : mascara;
}

GlslDeko SpirvAGlsl(std::span<const uint32_t> espirv, uint32_t especializacion, uint32_t salidas) {
  GlslDeko r;
  try {
    spirv_cross::CompilerGLSL compilador(espirv.data(), espirv.size());
    r.vertices = compilador.get_execution_model() == spv::ExecutionModelVertex;
    uint32_t quitar = 0;  // interpolator outputs of a vertex shader that become plain globals (QuitarSalidas)
    if (r.vertices) {
      for (const spirv_cross::Resource& s : compilador.get_shader_resources().stage_outputs) {
        const uint32_t ubicacion = compilador.get_decoration(s.id, spv::DecorationLocation);
        if (ubicacion >= 32) {
          continue;
        }
        if (salidas & (1u << ubicacion)) {
          r.interpoladores |= 1u << ubicacion;
        } else {
          quitar |= 1u << ubicacion;
        }
      }
    }

    spirv_cross::CompilerGLSL::Options opciones;
    opciones.version = 460;
    opciones.es = false;
    opciones.vulkan_semantics = false;  // uam's front end is GL's
    compilador.set_common_options(opciones);

    // GLSL without Vulkan semantics has no separate images and samplers. Each register's texture is sampled
    // with its own sampler (the pair of tfetch2DSombraMin with its register's), and a texture that is only asked
    // its size gets a dummy sampler: either way the combination takes the texture's binding, which is the
    // deko3d slot the renderer binds dkMakeTextureHandle(image, sampler) to.
    compilador.build_dummy_sampler_for_combined_images();
    compilador.build_combined_image_samplers();
    for (const spirv_cross::CombinedImageSampler& c : compilador.get_combined_image_samplers()) {
      if (!compilador.has_decoration(c.image_id, spv::DecorationBinding)) {
        r.error = "una textura sin binding: " + compilador.get_name(c.image_id);
        return r;
      }
      const uint32_t enlace = compilador.get_decoration(c.image_id, spv::DecorationBinding);
      if (enlace >= kMaxTexturas) {
        r.error = "textura " + compilador.get_name(c.image_id) + " en el binding " + std::to_string(enlace) +
                  ", fuera de los 32 de deko3d";
        return r;
      }
      compilador.set_decoration(c.combined_id, spv::DecorationBinding, enlace);
      r.texturas |= 1u << enlace;
      const spv::Dim dimension = compilador.get_type_from_variable(c.image_id).image.dim;
      if (dimension == spv::Dim3D) {
        r.texturas_3d |= 1u << enlace;
      } else if (dimension == spv::DimCube) {
        r.texturas_cubo |= 1u << enlace;
      }
    }

    const auto usados = compilador.get_active_interface_variables();
    if (!r.vertices) {
      for (const spirv_cross::Resource& e : compilador.get_shader_resources(usados).stage_inputs) {
        const uint32_t ubicacion = compilador.get_decoration(e.id, spv::DecorationLocation);
        if (ubicacion < 32) {
          r.interpoladores |= 1u << ubicacion;
        }
      }
    }
    for (const spirv_cross::Resource& ubo : compilador.get_shader_resources(usados).uniform_buffers) {
      const uint32_t enlace = compilador.get_decoration(ubo.id, spv::DecorationBinding);
      if (enlace >= kMaxUbos) {
        r.error = "UBO " + ubo.name + " en el binding " + std::to_string(enlace) + ", fuera de los 16 de deko3d";
        return r;
      }
      r.ubos |= 1u << enlace;
    }

    // The value is the default of SPIRV_CROSS_CONSTANT_ID_0 in the GLSL, so the text says which variant it is;
    // Mesa's front end folds the branches that depend on it.
    for (const spirv_cross::SpecializationConstant& c : compilador.get_specialization_constants()) {
      if (c.constant_id != 0) {
        r.error = "constante de especializacion inesperada: " + std::to_string(c.constant_id);
        return r;
      }
      compilador.get_constant(c.id).m.c[0].r[0].u32 = especializacion;
    }

    r.glsl = compilador.compile();
    if (quitar && !QuitarSalidas(r.glsl, quitar, r.error)) {
      r.glsl.clear();
      return r;
    }
  } catch (const std::exception& e) {
    r.glsl.clear();
    r.error = std::string("SPIRV-Cross: ") + e.what();
    return r;
  }

  for (const char* extension : kExtensionesProhibidas) {
    if (r.glsl.find(extension) != std::string::npos) {
      r.error = std::string("el GLSL pide ") + extension + ", que uam no tiene";
      r.glsl.clear();
      return r;
    }
  }
  BuscarComponentesDinamicos(r.glsl, r.avisos);
  return r;
}

}  // namespace nfsmw::deko

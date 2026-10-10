/**
 * @file        system/gpu_plugin_loader.cpp
 * @brief       Host-side loader for GPU emulation plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/gpu_plugin.h>

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::system {

namespace {

// Plugin binaries follow the SDK's per-config postfix convention; this TU is
// part of rexruntime, so REXGLUE_BUILD_CONFIG matches the plugin's config.
std::string PluginFileName(std::string_view name) {
  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  std::string_view postfix = "";
  if (kConfig == "Debug") {
    postfix = "d";
  } else if (kConfig == "RelWithDebInfo") {
    postfix = "rd";
  }
#if REX_PLATFORM_WIN32
  return fmt::format("rexgpu-{}{}.dll", name, postfix);
#elif REX_PLATFORM_MAC
  return fmt::format("librexgpu-{}{}.dylib", name, postfix);
#else
  return fmt::format("librexgpu-{}{}.so", name, postfix);
#endif
}

// Plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown.
std::vector<platform::DynamicLibrary>& LoadedPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

#if REX_PLATFORM_SWITCH && REXGLUE_SWITCH_DEKO3D

// nfsmw: the deko3d build links no GPU plugin; the title brings its graphics system (config.graphics).
std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {
  (void)backend;
  REXSYS_ERROR("GPU plugin '{}' is not linked into this executable (deko3d build: no GPU plugins)", name);
  return nullptr;
}

#elif REX_PLATFORM_SWITCH
}  // namespace rex::system

// A NRO cannot load code at run time: rexgpu-xenos is linked into the
// executable, and its plugin_main.cpp entry points are called directly.
extern "C" uint32_t rex_gpu_abi_version(void);
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
                                                         const rex::system::GpuCreateInfo* info);

namespace rex::system {

std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {
  if (name != "xenos") {
    REXSYS_ERROR("GPU plugin '{}' is not linked into this executable (only 'xenos' is)", name);
    return nullptr;
  }
  uint32_t plugin_abi = rex_gpu_abi_version();
  if (plugin_abi != kGpuPluginAbiVersion) {
    REXSYS_ERROR("GPU plugin '{}' has ABI version {}, host expects {}", name, plugin_abi,
                 kGpuPluginAbiVersion);
    return nullptr;
  }
  std::string backend_str(backend);
  GpuCreateInfo info{};
  info.struct_size = sizeof(GpuCreateInfo);
  info.backend = backend_str.c_str();
  IGraphicsSystem* graphics_system = rex_gpu_create(kGpuPluginAbiVersion, &info);
  if (!graphics_system) {
    REXSYS_ERROR("GPU plugin '{}' factory returned no graphics system (backend '{}')", name,
                 backend_str);
    return nullptr;
  }
  REXSYS_DEBUG("GPU plugin '{}' linked statically", name);
  return std::unique_ptr<IGraphicsSystem>(graphics_system);
}

#else

std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {
  auto path = rex::filesystem::GetExecutableFolder() / PluginFileName(name);
  if (!std::filesystem::exists(path)) {
    REXSYS_ERROR(
        "GPU plugin '{}' not found at {}. Stage it next to the executable "
        "(GPU_PLUGINS {} in rexglue_configure_target).",
        name, path.string(), name);
    return nullptr;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("GPU plugin '{}' failed to load: {}", name, path.string());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<GpuAbiVersionFn>(kGpuAbiVersionSymbol);
  auto create_fn = library.GetSymbol<GpuCreateFn>(kGpuCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("GPU plugin '{}' is not a rexglue GPU plugin (missing {} / {} exports): {}", name,
                 kGpuAbiVersionSymbol, kGpuCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kGpuPluginAbiVersion) {
    REXSYS_ERROR("GPU plugin '{}' has ABI version {}, host expects {}: {}", name, plugin_abi,
                 kGpuPluginAbiVersion, path.string());
    return nullptr;
  }

  std::string backend_str(backend);
  GpuCreateInfo info{};
  info.struct_size = sizeof(GpuCreateInfo);
  info.backend = backend_str.c_str();

  IGraphicsSystem* graphics_system = create_fn(kGpuPluginAbiVersion, &info);
  if (!graphics_system) {
    REXSYS_ERROR("GPU plugin '{}' factory returned no graphics system (backend '{}')", name,
                 backend_str);
    return nullptr;
  }

  LoadedPlugins().push_back(std::move(library));
  REXSYS_DEBUG("GPU plugin '{}' loaded ({})", name, path.filename().string());
  return std::unique_ptr<IGraphicsSystem>(graphics_system);
}

#endif  // REX_PLATFORM_SWITCH

}  // namespace rex::system

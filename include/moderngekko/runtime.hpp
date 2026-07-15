#pragma once

#include "moderngekko/game.hpp"
#include "moderngekko/module_abi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace moderngekko
{
struct ModuleSource
{
  enum class Kind
  {
    None,
    DynamicPath,
    AttachedDescriptor,
  };

  static ModuleSource DynamicPath(std::filesystem::path path);
  static ModuleSource AttachedDescriptor(const ModernGekkoModuleDesc* descriptor);

  Kind kind = Kind::None;
  std::filesystem::path path;
  const ModernGekkoModuleDesc* descriptor = nullptr;
};

enum class ShaderCompilationPolicy
{
  Synchronous,
  SynchronousUberShaders,
  AsynchronousUberShaders,
  AsynchronousSkipRendering,
};

struct GraphicsSettings
{
  std::string backend;
  std::optional<int> internal_resolution_scale;
  std::optional<ShaderCompilationPolicy> shader_compilation;
  std::optional<bool> wait_for_shaders_before_starting;
  bool force_widescreen = false;
  bool show_fps = false;
};

struct AudioSettings
{
  std::string backend;
};

struct InputSettings
{
  bool background_input = false;
};

struct DebugSettings
{
  std::filesystem::path symbol_map;
  bool trace_functions = false;
  std::string trace_function;
  std::optional<std::uint32_t> idle_pc;
};

enum class WindowSystem
{
  Default,
  Wayland,
  X11,
};

struct RuntimeConfig
{
  std::filesystem::path game_root;
  std::filesystem::path user_directory;
  std::filesystem::path input_movie;
  ModuleSource module;
  GraphicsSettings graphics;
  AudioSettings audio;
  InputSettings input;
  DebugSettings debug;
  WindowSystem window_system = WindowSystem::Default;
  bool headless = false;
  bool allow_interpreter = false;
  bool allow_fallback = false;
  std::optional<std::string> window_title;
};

enum class RuntimeErrorCode
{
  AlreadyActive,
  InvalidGame,
  ModuleRequired,
  ModuleRejected,
  PlatformUnavailable,
  InitializationFailed,
  BootFailed,
  InvalidState,
};

struct RuntimeError
{
  RuntimeErrorCode code = RuntimeErrorCode::InitializationFailed;
  std::string message;
};

enum class RuntimeExitReason
{
  Stopped,
  BootFailed,
};

struct RuntimeRunResult
{
  RuntimeExitReason reason = RuntimeExitReason::Stopped;
  std::optional<RuntimeError> error;
};

class Runtime;

struct RuntimeCreateResult
{
  std::unique_ptr<Runtime> runtime;
  std::optional<RuntimeError> error;

  explicit operator bool() const { return runtime != nullptr; }
};

class Runtime final
{
public:
  static RuntimeCreateResult Create(RuntimeConfig config);

  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  RuntimeRunResult Run();
  void RequestStop();
  std::optional<RuntimeError> RequestScreenshot(std::string_view name);
  std::optional<RuntimeError> RequestScreenshotOnHostEvent(std::string_view name,
                                                           std::uint32_t event_id);
  std::optional<RuntimeError> Pause();
  std::optional<RuntimeError> Resume();

  const RuntimeConfig& GetConfig() const;
  const GameMetadata& GetGameMetadata() const;
  const std::string& GetWindowTitle() const;

private:
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
}  // namespace moderngekko

namespace ModernGekko = moderngekko;

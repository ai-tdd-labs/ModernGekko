#include "moderngekko/runtime.hpp"

#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/Movie.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/VideoConfig.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/module_loader.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_functions) ==
              offsetof(StaticRecompModuleDesc, chunk_functions));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform* s_platform = nullptr;
std::string s_window_title;

bool EnsureOutputParent(const std::filesystem::path& path)
{
  if (path.empty() || path.parent_path().empty())
    return true;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  return !ec;
}

Movie::ControllerTypeArray GetMovieControllers()
{
  Movie::ControllerTypeArray controllers{};
  for (int index = 0; index < 4; ++index)
  {
    const SerialInterface::SIDevices device =
        Config::Get(Config::GetInfoForSIDevice(index));
    if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[index] = Movie::ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(device))
      controllers[index] = Movie::ControllerType::GC;
  }
  if (controllers == Movie::ControllerTypeArray{})
    controllers[0] = Movie::ControllerType::GC;
  return controllers;
}

Movie::WiimoteEnabledArray GetMovieWiimotes()
{
  Movie::WiimoteEnabledArray wiimotes{};
  for (int index = 0; index < 4; ++index)
  {
    wiimotes[index] =
        Config::Get(Config::GetInfoForWiimoteSource(index)) != WiimoteSource::None;
  }
  return wiimotes;
}

bool SaveStateSynchronously(Core::System& system, const std::filesystem::path& path)
{
  std::promise<void> queued;
  std::future<void> queued_future = queued.get_future();
  Core::RunOnCPUThread(system, [&system, path, &queued] {
    State::SaveAs(system, path.string());
    queued.set_value();
  });
  queued_future.wait();
  UICommon::FlushUnsavedData();
  return std::filesystem::is_regular_file(path);
}
}

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string&)
{
  if (s_platform)
    s_platform->SetTitle(s_window_title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() { return !s_platform || s_platform->IsWindowFocused(); }
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() { return s_platform && s_platform->IsWindowFullscreen(); }
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string&) {}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   std::int64_t, std::int64_t, int, int)
{
  return false;
}
std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

namespace moderngekko
{
struct Runtime::Impl
{
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  Common::EventHook state_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  std::atomic<bool> booted{false};
  std::atomic<bool> running{false};
};

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path)
{
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc* descriptor)
{
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config)
{
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {{}, RuntimeError{RuntimeErrorCode::AlreadyActive,
                             "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};
  if (!config.input_movie.empty() && !std::filesystem::is_regular_file(config.input_movie))
    return {{}, RuntimeError{RuntimeErrorCode::InitializationFailed,
                             "input movie not found: " + config.input_movie.string()}};
  if (!config.input_savestate.empty() &&
      !std::filesystem::is_regular_file(config.input_savestate))
  {
    return {{}, RuntimeError{RuntimeErrorCode::InitializationFailed,
                             "savestate not found: " + config.input_savestate.string()}};
  }
  if (!config.input_movie.empty() && !config.record_movie.empty())
  {
    return {{}, RuntimeError{RuntimeErrorCode::InitializationFailed,
                             "movie playback and recording cannot be active together"}};
  }
  if (!config.input_movie.empty() && !config.input_savestate.empty())
  {
    return {{}, RuntimeError{
                    RuntimeErrorCode::InitializationFailed,
                    "a DTM controls its own starting state; do not combine --movie and --load-state"}};
  }
  if (!EnsureOutputParent(config.record_movie))
  {
    return {{}, RuntimeError{RuntimeErrorCode::InitializationFailed,
                             "cannot create movie output directory: " +
                                 config.record_movie.parent_path().string()}};
  }
  if (!EnsureOutputParent(config.save_state_on_exit))
  {
    return {{}, RuntimeError{RuntimeErrorCode::InitializationFailed,
                             "cannot create savestate output directory: " +
                                 config.save_state_on_exit.parent_path().string()}};
  }

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result = validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result = validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {{}, RuntimeError{RuntimeErrorCode::ModuleRequired,
                             "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok)
  {
    if (!config.allow_interpreter)
    {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message +=
            ": " + std::string(moderngekko_module_status_string(module_result.validation_status));
      return {{}, RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or("ModernGekko - " + impl->metadata.game_name +
                                                   " [" + impl->metadata.disc_id + "]");

  UICommon::SetUserDirectory(impl->config.user_directory.string());
  UICommon::Init();
  impl->ui_initialized = true;

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef MODERNGEKKO_HAVE_COCOA
  else
    impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11)
    impl->platform = Platform::CreateWaylandPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system == WindowSystem::X11)
    impl->platform = Platform::CreateX11Platform();
#endif
  if (!impl->platform || !impl->platform->Init())
  {
    UICommon::Shutdown();
    return {{}, RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                             "the requested Dolphin host platform is unavailable"}};
  }

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);

  // Runtime invariants belong to CurrentRun, whose priority is above movie and
  // per-game layers. A DTM recorded under Dolphin's JIT must never replace the
  // supplied native module with the recorded CPU core during playback.
  Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  if (!impl->config.graphics.backend.empty())
    Config::SetCurrent(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetCurrent(Config::GFX_EFB_SCALE, *impl->config.graphics.internal_resolution_scale);
  if (impl->config.graphics.shader_compilation)
  {
    ShaderCompilationMode mode = ShaderCompilationMode::Synchronous;
    switch (*impl->config.graphics.shader_compilation)
    {
    case ShaderCompilationPolicy::Synchronous:
      mode = ShaderCompilationMode::Synchronous;
      break;
    case ShaderCompilationPolicy::SynchronousUberShaders:
      mode = ShaderCompilationMode::SynchronousUberShaders;
      break;
    case ShaderCompilationPolicy::AsynchronousUberShaders:
      mode = ShaderCompilationMode::AsynchronousUberShaders;
      break;
    case ShaderCompilationPolicy::AsynchronousSkipRendering:
      mode = ShaderCompilationMode::AsynchronousSkipRendering;
      break;
    }
    Config::SetCurrent(Config::GFX_SHADER_COMPILATION_MODE, mode);
  }
  if (impl->config.graphics.wait_for_shaders_before_starting)
  {
    Config::SetCurrent(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
                       *impl->config.graphics.wait_for_shaders_before_starting);
  }
  // Always select an aspect for this run so a previous widescreen launch or a
  // movie config cannot silently leak into a 4:3 baseline (or vice versa).
  const AspectMode aspect_mode = impl->config.graphics.force_widescreen ?
                                     AspectMode::ForceWide :
                                     AspectMode::ForceStandard;
  Config::SetCurrent(Config::GFX_ASPECT_RATIO, aspect_mode);
  Config::SetCurrent(Config::GFX_SUGGESTED_ASPECT_RATIO, aspect_mode);
  Config::SetCurrent(Config::GFX_SHOW_FPS, impl->config.graphics.show_fps);
  if (!impl->config.audio.backend.empty())
    Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  else if (impl->config.headless)
    Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, std::string("No Audio Output"));
  Config::SetCurrent(Config::MAIN_INPUT_BACKGROUND_INPUT, impl->config.input.background_input);
  Config::SetCurrent(Config::MAIN_STATICRECOMP_SYMBOL_MAP,
                     impl->config.debug.symbol_map.string());
  Config::SetCurrent(Config::MAIN_STATICRECOMP_FUNCTION_PROFILE,
                     impl->config.debug.function_profile.string());
  Config::SetCurrent(Config::MAIN_STATICRECOMP_FUNCTION_PROFILE_START_FRAME,
                     impl->config.debug.function_profile_start_frame.value_or(0));
  Config::SetCurrent(Config::MAIN_STATICRECOMP_FUNCTION_PROFILE_END_FRAME,
                     impl->config.debug.function_profile_end_frame.value_or(0));
  Config::SetCurrent(Config::MAIN_STATICRECOMP_TRACE_FUNCTIONS,
                     impl->config.debug.trace_functions);
  Config::SetCurrent(Config::MAIN_STATICRECOMP_TRACE_FUNCTION,
                     impl->config.debug.trace_function);
  Config::SetCurrent(Config::MAIN_STATICRECOMP_IDLE_PC,
                     impl->config.debug.idle_pc.value_or(0));
  Config::SetCurrent(Config::MAIN_STATICRECOMP_ALLOW_FALLBACK,
                     impl->config.allow_fallback);

  auto& jit = Core::System::GetInstance().GetJitInterface();
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Dynamic(impl->config.module.path.string()));
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc*>(impl->config.module.descriptor)));
  else
    jit.SetStaticRecompModuleSource({});

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime()
{
  RequestStop();
  if (m_impl->booted)
  {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run()
{
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState, "runtime is already running"}};

  auto boot = BootParameters::GenerateFromFile(m_impl->metadata.main_dol.string());
  if (!boot)
  {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, "Dolphin rejected the extracted disc"}};
  }
  if (!m_impl->config.input_movie.empty())
  {
    std::optional<std::string> savestate_path;
    auto& movie = Core::System::GetInstance().GetMovie();
    if (!movie.PlayInput(m_impl->config.input_movie.string(), &savestate_path))
    {
      m_impl->running = false;
      return {RuntimeExitReason::BootFailed,
              RuntimeError{RuntimeErrorCode::BootFailed,
                           "Dolphin rejected input movie: " +
                               m_impl->config.input_movie.string()}};
    }
    boot->boot_session_data.SetSavestateData(std::move(savestate_path),
                                             DeleteSavestateAfterBoot::No);
  }
  else if (!m_impl->config.input_savestate.empty())
  {
    boot->boot_session_data.SetSavestateData(m_impl->config.input_savestate.string(),
                                             DeleteSavestateAfterBoot::No);
  }
  m_impl->state_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    if (state == Core::State::Uninitialized && m_impl->platform)
      m_impl->platform->Stop();
  });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo()))
  {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;

  std::string media_error;
  if (!m_impl->config.record_movie.empty())
  {
    constexpr auto recording_start_timeout = std::chrono::seconds(30);
    const auto deadline = std::chrono::steady_clock::now() + recording_start_timeout;
    while (Core::GetState(Core::System::GetInstance()) == Core::State::Starting &&
           std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto& movie = Core::System::GetInstance().GetMovie();
    const std::filesystem::path movie_state =
        m_impl->config.record_movie.string() + ".sav";
    if (Core::GetState(Core::System::GetInstance()) != Core::State::Running ||
        !SaveStateSynchronously(Core::System::GetInstance(), movie_state) ||
        !movie.BeginRecordingInput(GetMovieControllers(), GetMovieWiimotes()))
    {
      media_error = "Dolphin could not capture the starting state or start input recording";
      m_impl->platform->Stop();
    }
    else
    {
      std::fprintf(stderr, "[moderngekko] recording DTM: %s\n",
                   m_impl->config.record_movie.string().c_str());
    }
  }

  m_impl->platform->MainLoop();

  auto& system = Core::System::GetInstance();
  auto& movie = system.GetMovie();
  if (!m_impl->config.record_movie.empty() && movie.IsRecordingInput())
  {
    UICommon::FlushUnsavedData();
    {
      const Core::CPUThreadGuard guard(system);
      movie.SaveRecording(m_impl->config.record_movie.string());
      movie.EndPlayInput(false);
    }
    if (!std::filesystem::is_regular_file(m_impl->config.record_movie) ||
        !std::filesystem::is_regular_file(m_impl->config.record_movie.string() + ".sav"))
    {
      media_error = "Dolphin did not finish the DTM/savestate recording";
    }
    else
    {
      std::fprintf(stderr, "[moderngekko] saved DTM: %s\n",
                   m_impl->config.record_movie.string().c_str());
    }
  }
  if (!m_impl->config.save_state_on_exit.empty() &&
      !SaveStateSynchronously(system, m_impl->config.save_state_on_exit))
  {
    media_error = "Dolphin did not finish the requested savestate";
  }

  Core::Stop(Core::System::GetInstance());
  std::string native_fallback_violation;
  if (const auto* static_core = dynamic_cast<const StaticRecompCore*>(
          Core::System::GetInstance().GetJitInterface().GetCore());
      static_core && static_core->HasNativeFallbackViolation())
  {
    native_fallback_violation = static_core->GetNativeFallbackViolation();
  }
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  if (!native_fallback_violation.empty())
  {
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, std::move(native_fallback_violation)}};
  }
  if (!media_error.empty())
  {
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, std::move(media_error)}};
  }
  return {};
}

void Runtime::RequestStop()
{
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::RequestScreenshot(std::string_view name)
{
  if (!m_impl->running || !m_impl->booted)
    return RuntimeError{RuntimeErrorCode::InvalidState, "runtime is not running"};
  if (name.empty())
    return RuntimeError{RuntimeErrorCode::InvalidState, "screenshot name is empty"};
  Core::SaveScreenShot(name);
  return {};
}

std::optional<RuntimeError> Runtime::RequestScreenshotOnHostEvent(std::string_view name,
                                                                  std::uint32_t event_id)
{
  if (!m_impl->running || !m_impl->booted)
    return RuntimeError{RuntimeErrorCode::InvalidState, "runtime is not running"};
  if (name.empty())
    return RuntimeError{RuntimeErrorCode::InvalidState, "screenshot name is empty"};
  if (event_id == 0)
    return RuntimeError{RuntimeErrorCode::InvalidState, "screenshot host event is zero"};
  if (!Core::SaveScreenShotOnHostEvent(name, event_id))
    return RuntimeError{RuntimeErrorCode::InvalidState, "screenshot renderer is not ready"};
  return {};
}

std::optional<RuntimeError> Runtime::Pause()
{
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState, "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume()
{
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState, "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig& Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata& Runtime::GetGameMetadata() const { return m_impl->metadata; }
const std::string& Runtime::GetWindowTitle() const { return m_impl->title; }
}  // namespace moderngekko

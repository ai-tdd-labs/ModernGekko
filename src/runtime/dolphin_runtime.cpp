#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBACore.h"
#include "Core/Movie.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoConfig.h"
#include "dolphin_runtime_internal.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/module_loader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fmt/format.h>
#include <mutex>
#include <thread>
#include <utility>

namespace {
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_functions) ==
              offsetof(StaticRecompModuleDesc, chunk_functions));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;
u64 s_previous_net_wait_ns = 0;
double s_net_wait_ms_per_second = 0.0;
std::chrono::steady_clock::time_point s_previous_net_wait_sample;

std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  const auto now = std::chrono::steady_clock::now();
  std::string formatted_title = fmt::format("{} | {:.1f} FPS", title, fps);
  const NetPlay::InputWaitTelemetry telemetry =
      NetPlay::NetPlayClient::GetInputWaitTelemetry();
  if (!telemetry.active) {
    s_previous_net_wait_ns = 0;
    s_net_wait_ms_per_second = 0.0;
    s_previous_net_wait_sample = {};
    return formatted_title;
  }
  if (s_previous_net_wait_sample.time_since_epoch().count() == 0) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  } else if (telemetry.total_wait_ns < s_previous_net_wait_ns) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
    s_net_wait_ms_per_second = 0.0;
  } else if (now - s_previous_net_wait_sample >=
             std::chrono::milliseconds(500)) {
    const double seconds =
        std::chrono::duration<double>(now - s_previous_net_wait_sample).count();
    s_net_wait_ms_per_second =
        static_cast<double>(telemetry.total_wait_ns - s_previous_net_wait_ns) /
        1000000.0 / seconds;
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  }
  return fmt::format("{} | Net wait {:.1f} ms/s | Buffer {}", formatted_title,
                     s_net_wait_ms_per_second, telemetry.buffer_size);
}
} // namespace

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  if (!s_platform)
    return;

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(
        title, Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
struct Runtime::Impl {
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

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};
  if (!config.input_movie.empty() && !std::filesystem::is_regular_file(config.input_movie))
    return {{}, RuntimeError{RuntimeErrorCode::InitializationFailed,
                             "input movie not found: " + config.input_movie.string()}};

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {
        {},
        RuntimeError{
            RuntimeErrorCode::ModuleRequired,
            "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      return {
          {},
          RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      "ModernGekko - " + impl->metadata.game_name + " [" +
      impl->metadata.disc_id + "]");

  if (!s_external_ui_common) {
    UICommon::SetUserDirectory(impl->config.user_directory.string());
    UICommon::Init();
    impl->ui_initialized = true;
  }

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
#ifdef MODERNGEKKO_HAVE_COCOA
  else impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland) impl->platform =
      Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11) impl->platform =
      Platform::CreateWaylandPlatform();
#endif
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
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
  Config::SetCurrent(Config::GFX_SHADER_CACHE, true);
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
  else
  {
    Config::SetCurrent(Config::GFX_SHADER_COMPILATION_MODE,
                       ShaderCompilationMode::AsynchronousUberShaders);
  }
  if (impl->config.graphics.wait_for_shaders_before_starting)
  {
    Config::SetCurrent(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
                       *impl->config.graphics.wait_for_shaders_before_starting);
  }
  else
  {
    Config::SetCurrent(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  }
  // Always select an aspect for this run so a previous widescreen launch or a
  // movie config cannot silently leak into a 4:3 baseline (or vice versa).
  const AspectMode aspect_mode = impl->config.graphics.force_widescreen ?
                                     AspectMode::ForceWide :
                                     AspectMode::ForceStandard;
  Config::SetCurrent(Config::GFX_ASPECT_RATIO, aspect_mode);
  Config::SetCurrent(Config::GFX_SUGGESTED_ASPECT_RATIO, aspect_mode);
  Config::SetCurrent(Config::GFX_SHOW_FPS, impl->config.graphics.show_fps);
  const std::vector<std::string> audio_backends = AudioCommon::GetSoundBackends();
  if (impl->config.headless)
  {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  }
  else if (impl->config.audio.backend.empty() ||
           !std::ranges::contains(audio_backends, impl->config.audio.backend))
  {
    impl->config.audio.backend = AudioCommon::GetDefaultSoundBackend();
    if (impl->config.audio.backend == BACKEND_NULLSOUND)
    {
      const auto available =
          std::ranges::find_if(audio_backends, [](const std::string& backend) {
            return backend != BACKEND_NULLSOUND;
          });
      if (available != audio_backends.end())
        impl->config.audio.backend = *available;
    }
  }
  Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  Config::SetCurrent(Config::MAIN_INPUT_BACKGROUND_INPUT, impl->config.input.background_input);
  Config::SetCurrent(Config::MAIN_STATICRECOMP_SYMBOL_MAP,
                     impl->config.debug.symbol_map.string());
  Config::SetCurrent(Config::MAIN_STATICRECOMP_TRACE_FUNCTIONS,
                     impl->config.debug.trace_functions);
  Config::SetCurrent(Config::MAIN_STATICRECOMP_TRACE_FUNCTION,
                     impl->config.debug.trace_function);
  Config::SetCurrent(Config::MAIN_STATICRECOMP_IDLE_PC,
                     impl->config.debug.idle_pc.value_or(0));
  Config::SetCurrent(Config::MAIN_STATICRECOMP_ALLOW_FALLBACK,
                     impl->config.allow_fallback);

  auto &jit = Core::System::GetInstance().GetJitInterface();
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    jit.SetStaticRecompModuleSource(
        StaticRecompModuleSource::Dynamic(impl->config.module.path.string()));
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            impl->config.module.descriptor)));
  else
    jit.SetStaticRecompModuleSource({});

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  RequestStop();
  if (m_impl->booted) {
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
  s_show_fps_in_title = true;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  std::unique_ptr<BootParameters> boot;
  {
    std::lock_guard lock(s_runtime_mutex);
    if (s_boot_session_data)
      boot = BootParameters::GenerateFromFile(
          m_impl->metadata.main_dol.string(), std::move(*s_boot_session_data));
    else
      boot =
          BootParameters::GenerateFromFile(m_impl->metadata.main_dol.string());
    s_boot_session_data.reset();
  }
  if (!boot)
  {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
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
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  std::atomic_bool title_thread_done = false;
  std::thread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title) {
    title_thread = std::thread([&title_thread_done] {
      while (!title_thread_done.load(std::memory_order_relaxed)) {
        Host_UpdateTitle({});
        for (int i = 0;
             i < 10 && !title_thread_done.load(std::memory_order_relaxed); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  m_impl->platform->MainLoop();
  title_thread_done.store(true, std::memory_order_relaxed);
  if (title_thread.joinable())
    title_thread.join();
  m_impl->platform->SaveWindowGeometry();
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
  return {};
}

void Runtime::RequestStop() {
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

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }
} // namespace moderngekko

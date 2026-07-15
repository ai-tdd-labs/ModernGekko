#include "moderngekko/runtime.hpp"

#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/HW/GBACore.h"
#include "Core/Movie.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/VideoConfig.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/module_loader.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

namespace
{
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform* s_platform = nullptr;
std::string s_window_title;
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
  m_impl->platform->MainLoop();
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

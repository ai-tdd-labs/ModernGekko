#include "frontend_config.hpp"
#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"
#include "netplay_session.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
#ifndef MODERNGEKKO_RUNNER_NAME
#define MODERNGEKKO_RUNNER_NAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

volatile std::sig_atomic_t s_stop_requested = 0;

void HandleStopSignal(int) { s_stop_requested = 1; }

void Usage() {
  std::cerr << "usage: " MODERNGEKKO_RUNNER_NAME
               " [--game <extracted-root>] [--module <path>]\n"
               "       [--user-dir <path>] [--title <text>]\n"
               "       [--movie <dtm>]\n"
               "       [--graphics <backend>] [--audio <backend>]\n"
               "       [--shader-compilation <sync|sync-ubershaders|async-ubershaders|async-skip>]\n"
               "       [--wait-for-shaders-before-starting]\n"
               "       [--symbols <path>] [--trace-functions | --trace-function <name>]\n"
               "       [--idle-pc <address>]\n"
               "       [--screenshot-request <path>]\n"
               "       [--widescreen] [--show-fps] [--wayland] [-X11] [--headless]\n"
               "       [--allow-interpreter] [--allow-fallback]\n"
               "       [--netplay-host | --netplay-join <host>] [--netplay-port <port>]\n"
               "       [--nickname <name>] [--buffer <auto|1-20>] [--controller <device>]...\n"
               "       With no --game, boots the path in <user-dir>/default-game.txt.\n";
}

std::uint32_t ParseAddress(const char* option, const char* text)
{
  try
  {
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 0);
    if (text[consumed] != '\0' || value > std::numeric_limits<std::uint32_t>::max())
      throw std::out_of_range("address");
    return static_cast<std::uint32_t>(value);
  }
  catch (const std::exception&)
  {
    std::cerr << option << " requires a 32-bit address (for example 0x800F2038)\n";
    std::exit(2);
  }
}

std::optional<std::uint32_t> TryParseAddress(std::string_view text)
{
  try
  {
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(std::string(text), &consumed, 0);
    if (consumed != text.size() || value > std::numeric_limits<std::uint32_t>::max())
      return std::nullopt;
    return static_cast<std::uint32_t>(value);
  }
  catch (const std::exception&)
  {
    return std::nullopt;
  }
}

moderngekko::ShaderCompilationPolicy ParseShaderCompilationPolicy(const char* text)
{
  const std::string_view value{text};
  if (value == "sync")
    return moderngekko::ShaderCompilationPolicy::Synchronous;
  if (value == "sync-ubershaders")
    return moderngekko::ShaderCompilationPolicy::SynchronousUberShaders;
  if (value == "async-ubershaders")
    return moderngekko::ShaderCompilationPolicy::AsynchronousUberShaders;
  if (value == "async-skip")
    return moderngekko::ShaderCompilationPolicy::AsynchronousSkipRendering;

  std::cerr << "--shader-compilation requires one of: sync, sync-ubershaders, "
               "async-ubershaders, async-skip\n";
  std::exit(2);
}

std::filesystem::path ReadDefaultGame(const std::filesystem::path& user_directory)
{
  std::ifstream file(user_directory / "default-game.txt");
  std::string path;
  std::getline(file, path);
  if (!path.empty() && path.back() == '\r')
    path.pop_back();
  return path;
}

std::filesystem::path DefaultUserDirectory() {
#if defined(_WIN32)
  if (const char *local_app_data = std::getenv("LOCALAPPDATA"))
    return std::filesystem::path(local_app_data) /
           MODERNGEKKO_USER_DIRECTORY_NAME;
#endif
  if (const char *xdg = std::getenv("XDG_DATA_HOME"))
    return std::filesystem::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home) / ".local" / "share" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

std::string LibrarySuffix() {
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::filesystem::path ExecutableDirectory(const char *argv0) {
  std::error_code ec;
#if defined(__linux__)
  const std::filesystem::path proc_executable =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const std::filesystem::path executable =
      std::filesystem::weakly_canonical(argv0, ec);
  return ec ? std::filesystem::current_path() : executable.parent_path();
}
} // namespace

int RunMain(int argc, char **argv) {
  moderngekko::RuntimeConfig config;
  config.user_directory = DefaultUserDirectory();
#ifdef MODERNGEKKO_DEFAULT_WINDOW_TITLE
  config.window_title = MODERNGEKKO_DEFAULT_WINDOW_TITLE;
#endif
  std::filesystem::path module_path;
  std::filesystem::path screenshot_request;
  std::optional<moderngekko::frontend::NetplayRole> netplay_role;
  std::string netplay_address;
  std::optional<std::uint16_t> netplay_port;
  std::string netplay_nickname;
  std::string netplay_buffer;
  std::vector<std::string> netplay_controllers;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](const char *option) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << option << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--game")
      config.game_root = value("--game");
    else if (arg == "--module")
      module_path = value("--module");
    else if (arg == "--user-dir")
      config.user_directory = value("--user-dir");
    else if (arg == "--movie")
      config.input_movie = value("--movie");
    else if (arg == "--title")
      config.window_title = value("--title");
    else if (arg == "--graphics")
      config.graphics.backend = value("--graphics");
    else if (arg == "--shader-compilation")
      config.graphics.shader_compilation =
          ParseShaderCompilationPolicy(value("--shader-compilation"));
    else if (arg == "--wait-for-shaders-before-starting")
      config.graphics.wait_for_shaders_before_starting = true;
    else if (arg == "--audio")
      config.audio.backend = value("--audio");
    else if (arg == "--symbols")
      config.debug.symbol_map = value("--symbols");
    else if (arg == "--trace-functions")
      config.debug.trace_functions = true;
    else if (arg == "--trace-function")
      config.debug.trace_function = value("--trace-function");
    else if (arg == "--idle-pc")
      config.debug.idle_pc = ParseAddress("--idle-pc", value("--idle-pc"));
    else if (arg == "--screenshot-request")
      screenshot_request = value("--screenshot-request");
    else if (arg == "--widescreen")
      config.graphics.force_widescreen = true;
    else if (arg == "--show-fps")
      config.graphics.show_fps = true;
    else if (arg == "-X11" || arg == "--x11")
      config.window_system = moderngekko::WindowSystem::X11;
    else if (arg == "--wayland")
      config.window_system = moderngekko::WindowSystem::Wayland;
    else if (arg == "--headless")
      config.headless = true;
    else if (arg == "--allow-interpreter")
      config.allow_interpreter = true;
    else if (arg == "--allow-fallback")
      config.allow_fallback = true;
    else if (arg == "--netplay-host")
      netplay_role = moderngekko::frontend::NetplayRole::Host;
    else if (arg == "--netplay-join") {
      netplay_role = moderngekko::frontend::NetplayRole::Join;
      netplay_address = value("--netplay-join");
    } else if (arg == "--netplay-port") {
      const std::string port_value = value("--netplay-port");
      unsigned int port = 0;
      const auto parsed = std::from_chars(
          port_value.data(), port_value.data() + port_value.size(), port);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != port_value.data() + port_value.size() || port == 0 ||
          port > 65535) {
        std::cerr << "--netplay-port must be between 1 and 65535\n";
        return 2;
      }
      netplay_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--nickname")
      netplay_nickname = value("--nickname");
    else if (arg == "--buffer")
      netplay_buffer = value("--buffer");
    else if (arg == "--controller")
      netplay_controllers.emplace_back(value("--controller"));
    else if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      Usage();
      return 2;
    }
  }
  if (config.game_root.empty())
    config.game_root = ReadDefaultGame(config.user_directory);
  if (config.game_root.empty()) {
    std::cerr << "no game configured; use --game once or create "
              << (config.user_directory / "default-game.txt") << '\n';
    Usage();
    return 2;
  }

  auto frontend_config =
      moderngekko::frontend::LoadConfig(config.user_directory, true);
  if (!frontend_config) {
    std::cerr << "invalid config.ini: " << frontend_config.error << '\n';
    return 2;
  }
  config.graphics.internal_resolution_scale = frontend_config.dolphin_scale;
  config.show_fps_in_title = frontend_config.show_fps_in_title;

  if (!netplay_role && !frontend_config.controller.empty()) {
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, frontend_config.controller,
            &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::cout << "controller configuration: " << controller_message << '\n';
  }

  const auto inspected = moderngekko::InspectGame(config.game_root);
  if (!inspected) {
    std::cerr << "invalid game: " << inspected.error << '\n';
    return 2;
  }

#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (inspected.metadata->disc_id != MODERNGEKKO_REQUIRED_DISC_ID) {
    std::cerr << "unsupported disc ID: expected "
              << MODERNGEKKO_REQUIRED_DISC_ID << ", got "
              << inspected.metadata->disc_id << '\n';
    return 2;
  }
#endif

  // Compatibility discovery belongs to the runner, never the runtime library.
  if (module_path.empty()) {
    if (const char *env = std::getenv("STATICRECOMP_MODULE"))
      module_path = env;
    else {
      const std::string module_name =
          "g" + inspected.metadata->disc_id + "_recomp" + LibrarySuffix();
      const auto bundled = ExecutableDirectory(argv[0]) / module_name;
      const auto user_module =
          config.user_directory / "StaticRecompModules" / module_name;
      if (std::filesystem::is_regular_file(bundled))
        module_path = bundled;
      else if (std::filesystem::is_regular_file(user_module))
        module_path = user_module;
    }
  }
  if (!module_path.empty())
    config.module =
        moderngekko::ModuleSource::DynamicPath(std::move(module_path));

#if defined(__linux__) || defined(_WIN32)
  if (!config.headless && config.graphics.backend.empty())
    config.graphics.backend = "Vulkan";
#endif

  if (netplay_role) {
    moderngekko::frontend::NetplayOptions options;
    options.role = *netplay_role;
    options.address = netplay_address.empty() ? frontend_config.netplay_address
                                              : netplay_address;
    options.port = netplay_port.value_or(frontend_config.netplay_port);
    options.nickname = netplay_nickname.empty()
                           ? frontend_config.netplay_nickname
                           : netplay_nickname;
    options.buffer = netplay_buffer.empty() ? frontend_config.netplay_buffer
                                            : netplay_buffer;
    const std::vector<std::string> configured_controllers =
        moderngekko::frontend::ReadConfiguredControllers(config.user_directory);
    options.controllers =
        netplay_controllers.empty()
            ? (configured_controllers.empty() ? frontend_config.controllers
                                              : configured_controllers)
            : netplay_controllers;
    if (options.controllers.empty() && !frontend_config.controller.empty())
      options.controllers.push_back(frontend_config.controller);
    if (options.controllers.empty()) {
      std::cerr << "netplay requires at least one selected controller\n";
      return 2;
    }
    frontend_config.netplay_address = options.address;
    frontend_config.netplay_port = options.port;
    frontend_config.netplay_nickname = options.nickname;
    frontend_config.netplay_buffer = options.buffer;
    frontend_config.controllers = options.controllers;
    frontend_config.controller = options.controllers.front();
    std::string controller_message;
    if (!moderngekko::frontend::EnsureControllerConfig(
            config.user_directory, options.controllers, &controller_message)) {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::string save_error;
    if (!moderngekko::frontend::SaveConfig(config.user_directory,
                                           frontend_config, &save_error)) {
      std::cerr << "configuration: " << save_error << '\n';
      return 2;
    }
    std::cerr << "netplay: runner started\n";
    return moderngekko::frontend::RunNetplayLobby(
        std::move(config), std::move(frontend_config), std::move(options));
  }

  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created) {
    std::cerr << "initialization failed: " << created.error->message << '\n';
    return 1;
  }
  std::cout << "audio backend: " << created.runtime->GetConfig().audio.backend
            << '\n';

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  std::atomic_bool control_watcher_done = false;
  std::thread control_watcher([&] {
    while (!control_watcher_done.load(std::memory_order_relaxed))
    {
      if (s_stop_requested)
      {
        s_stop_requested = 0;
        created.runtime->RequestStop();
        return;
      }
      if (!screenshot_request.empty())
      {
        std::error_code ec;
        if (std::filesystem::is_regular_file(screenshot_request, ec))
        {
          std::ifstream request_file(screenshot_request);
          std::string request;
          std::getline(request_file, request);
          if (!request.empty() && request.back() == '\r')
            request.pop_back();

          std::optional<std::uint32_t> host_event;
          bool valid_request = true;
          constexpr std::string_view event_prefix = "event=";
          if (request.starts_with(event_prefix))
          {
            host_event = TryParseAddress(std::string_view(request).substr(event_prefix.size()));
            valid_request = host_event.has_value() && *host_event != 0;
          }
          else if (!request.empty() && request != "capture")
          {
            valid_request = false;
          }

          std::optional<moderngekko::RuntimeError> error;
          if (valid_request)
          {
            error = host_event ? created.runtime->RequestScreenshotOnHostEvent(
                                     screenshot_request.stem().string(), *host_event) :
                                 created.runtime->RequestScreenshot(
                                     screenshot_request.stem().string());
          }

          if (!valid_request)
          {
            std::cerr << "invalid screenshot request: " << request << '\n';
            std::filesystem::remove(screenshot_request, ec);
            screenshot_request.clear();
          }
          else if (error && error->code != moderngekko::RuntimeErrorCode::InvalidState)
          {
            std::cerr << "screenshot request failed: " << error->message << '\n';
            std::filesystem::remove(screenshot_request, ec);
            screenshot_request.clear();
          }
          else if (!error)
          {
            std::filesystem::remove(screenshot_request, ec);
            if (host_event)
            {
              std::cout << "renderer screenshot armed: " << screenshot_request.stem().string()
                        << ".png event=0x" << std::hex << std::uppercase << *host_event
                        << std::dec << '\n';
            }
            else
            {
              std::cout << "renderer screenshot requested: "
                        << screenshot_request.stem().string() << ".png\n";
            }
            // --screenshot-request is a one-shot control. Once Dolphin owns
            // the pending capture, stop polling the filesystem every 2 ms;
            // the watcher must remain alive only to service stop signals.
            screenshot_request.clear();
          }
        }
      }
      // Screenshot requests are used for deterministic A/B capture. Keep the
      // control latency below one 60 Hz frame only for that opt-in mode; normal
      // gameplay retains the low-wakeup stop-signal watcher cadence.
      std::this_thread::sleep_for(
          std::chrono::milliseconds(screenshot_request.empty() ? 50 : 2));
    }
  });
  const moderngekko::RuntimeRunResult result = created.runtime->Run();
  control_watcher_done.store(true, std::memory_order_relaxed);
  control_watcher.join();
  if (result.error)
  {
    std::cerr << "runtime failed: " << result.error->message << '\n';
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  try {
    return RunMain(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "fatal error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "fatal error: unknown exception\n";
  }
  return 1;
}

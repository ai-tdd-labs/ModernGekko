#include "moderngekko/game.hpp"
#include "moderngekko/module_abi.h"
#include "rel_source_archive.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr std::string_view RECOMPCORE_REVISION = "42a6bb23db8510fbcd34184bb54aa5679b05dd9b";
constexpr std::string_view REL_PACKAGING_REVISION = "combined-rarc-v2";

struct BuildOptions
{
  std::string toolchain = "auto";
  fs::path output;
  fs::path module_patch;
  fs::path module_patch_addresses;
  unsigned jobs = 0;
  bool fast_build = false;
  bool max_optimization = false;
  std::vector<std::string> hot_dol_chunks;
  std::vector<std::string> runner_arguments;
};

fs::path DefaultOutput()
{
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"))
    return fs::path(xdg) / "moderngekko" / "modules";
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".cache" / "moderngekko" / "modules";
  return "moderngekko-modules";
}

std::string Suffix()
{
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::string Quote(const fs::path& value)
{
#if defined(_WIN32)
  std::string text = value.string();
  return '"' + text + '"';
#else
  std::string text = value.string();
  std::string result = "'";
  for (char c : text)
    result += c == '\'' ? "'\\''" : std::string(1, c);
  return result + "'";
#endif
}

std::uint64_t Fnv1a(std::string_view value)
{
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char c : value)
    hash = (hash ^ c) * 0x100000001b3ULL;
  return hash;
}

std::uint64_t Fnv1aAppend(std::uint64_t hash, std::string_view value)
{
  for (unsigned char c : value)
    hash = (hash ^ c) * 0x100000001b3ULL;
  return hash;
}

std::string HexFingerprint(std::uint64_t value)
{
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

std::string ReadFile(const fs::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string FingerprintFile(const fs::path& path)
{
  if (!fs::is_regular_file(path))
    return "missing";
  return HexFingerprint(Fnv1a(ReadFile(path)));
}

std::string FingerprintTree(const fs::path& root)
{
  if (!fs::is_directory(root))
    return "missing";

  std::vector<fs::path> files;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
  {
    if (entry.is_regular_file())
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const fs::path& path : files)
  {
    const std::string relative = fs::relative(path, root).generic_string();
    hash = Fnv1aAppend(hash, relative);
    hash = Fnv1aAppend(hash, std::string_view("\0", 1));
    hash = Fnv1aAppend(hash, ReadFile(path));
    hash = Fnv1aAppend(hash, std::string_view("\0", 1));
  }
  return HexFingerprint(hash);
}

std::vector<fs::path> CollectRelFiles(const fs::path& root)
{
  std::vector<fs::path> files;
  if (!fs::is_directory(root))
    return files;

  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
  {
    if (!entry.is_regular_file())
      continue;
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".rel")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::string FingerprintRelSources(
    const fs::path& root, const std::vector<fs::path>& files,
    const std::vector<moderngekko::port::RelArchive>& archives)
{
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const fs::path& path : files)
  {
    const std::string relative = fs::relative(path, root).generic_string();
    hash = Fnv1aAppend(hash, "loose:");
    hash = Fnv1aAppend(hash, relative);
    hash = Fnv1aAppend(hash, std::string_view("\0", 1));
    hash = Fnv1aAppend(hash, ReadFile(path));
    hash = Fnv1aAppend(hash, std::string_view("\0", 1));
  }
  for (const auto& archive : archives)
  {
    const std::string relative = fs::relative(archive.path, root).generic_string();
    for (const auto& rel : archive.rels)
    {
      hash = Fnv1aAppend(hash, "rarc:");
      hash = Fnv1aAppend(hash, relative);
      hash = Fnv1aAppend(hash, ":");
      hash = Fnv1aAppend(hash, rel.name);
      hash = Fnv1aAppend(hash, std::string_view("\0", 1));
      hash = Fnv1aAppend(
          hash, std::string_view(reinterpret_cast<const char*>(rel.bytes.data()), rel.bytes.size()));
      hash = Fnv1aAppend(hash, std::string_view("\0", 1));
    }
  }
  return HexFingerprint(hash);
}

std::string ReadCommand(const std::string& command)
{
#if defined(_WIN32)
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe)
    return {};
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe))
    output += buffer;
#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return output;
}

bool RunCommand(const std::string& command)
{
  std::cout << "+ " << command << '\n';
  return std::system(command.c_str()) == 0;
}

fs::path SiblingExecutable(const char* argv0, std::string name)
{
  std::error_code ec;
  fs::path self = fs::weakly_canonical(argv0, ec);
#if defined(_WIN32)
  name += ".exe";
#endif
  const fs::path sibling = self.parent_path() / name;
  return fs::is_regular_file(sibling) ? sibling : fs::path(std::move(name));
}

std::string PlatformName(moderngekko::GamePlatform platform)
{
  return platform == moderngekko::GamePlatform::Wii ? "Wii (Broadway)" : "GameCube (Gekko)";
}

std::string ActiveModule(const fs::path& output, std::string_view id)
{
  std::ifstream file(output / id / "active-module.txt");
  std::string value;
  std::getline(file, value);
  return value;
}

std::string CachedModuleStatus(const fs::path& output,
                               const moderngekko::GameMetadata& game)
{
  const std::string active = ActiveModule(output, game.disc_id);
  if (active.empty())
    return "none";

  const fs::path module = active;
  if (!fs::is_regular_file(module))
    return "missing: " + module.string();

  std::ifstream manifest(module.parent_path() / "manifest.txt");
  std::string line;
  while (std::getline(manifest, line))
  {
    constexpr std::string_view prefix = "dol_sha256=";
    if (line.starts_with(prefix))
    {
      const bool current = line.substr(prefix.size()) == game.dol_sha256;
      return std::string(current ? "current: " : "stale: ") + module.string();
    }
  }
  return "unverified: " + module.string();
}

int Inspect(const fs::path& root, const fs::path& output)
{
  const auto result = moderngekko::InspectGame(root);
  if (!result)
  {
    std::cerr << "invalid extracted game: " << result.error << '\n';
    return 1;
  }
  const auto& game = *result.metadata;
  std::cout << "Game name: " << game.game_name << '\n'
            << "Disc ID:   " << game.disc_id << '\n'
            << "Platform:  " << PlatformName(game.platform) << '\n'
            << "Entry:     0x" << std::hex << std::setw(8) << std::setfill('0')
            << game.entry_point << std::dec << '\n'
            << "DOL SHA-256: " << game.dol_sha256 << '\n'
            << "Cached module: " << CachedModuleStatus(output, game) << '\n';
  return 0;
}

std::optional<fs::path> Build(const char* argv0, const fs::path& root,
                              BuildOptions options)
{
  const auto inspected = moderngekko::InspectGame(root);
  if (!inspected)
  {
    std::cerr << "invalid extracted game: " << inspected.error << '\n';
    return std::nullopt;
  }
  const auto& game = *inspected.metadata;
  if (options.output.empty())
    options.output = DefaultOutput();

  std::string module_patch_contents;
  std::string module_patch_addresses_contents;
  if (!options.module_patch.empty())
  {
    std::error_code ec;
    options.module_patch = fs::weakly_canonical(options.module_patch, ec);
    if (ec || !fs::is_regular_file(options.module_patch))
    {
      std::cerr << "module patch not found: " << options.module_patch << '\n';
      return std::nullopt;
    }
    module_patch_contents = ReadFile(options.module_patch);
    if (module_patch_contents.empty())
    {
      std::cerr << "module patch is empty: " << options.module_patch << '\n';
      return std::nullopt;
    }

    if (options.module_patch_addresses.empty())
    {
      std::cerr << "--module-patch requires --module-patch-addresses <file>\n";
      return std::nullopt;
    }
    options.module_patch_addresses = fs::weakly_canonical(options.module_patch_addresses, ec);
    if (ec || !fs::is_regular_file(options.module_patch_addresses))
    {
      std::cerr << "module patch-address manifest not found: "
                << options.module_patch_addresses << '\n';
      return std::nullopt;
    }
    module_patch_addresses_contents = ReadFile(options.module_patch_addresses);
    if (module_patch_addresses_contents.empty())
    {
      std::cerr << "module patch-address manifest is empty: "
                << options.module_patch_addresses << '\n';
      return std::nullopt;
    }
  }
  else if (!options.module_patch_addresses.empty())
  {
    std::cerr << "--module-patch-addresses requires --module-patch <file>\n";
    return std::nullopt;
  }

  std::string compiler;
  if (options.toolchain == "auto")
#if defined(_WIN32)
    compiler = "cl";
#else
    compiler = ReadCommand("clang --version 2>&1").empty() ? "gcc" : "clang";
#endif
  else if (options.toolchain == "clang")
    compiler = "clang";
  else if (options.toolchain == "gcc")
    compiler = "gcc";
  else if (options.toolchain == "msvc")
  {
#if defined(_WIN32)
    compiler = "cl";
#else
    std::cerr << "MSVC modules can only be built on Windows\n";
    return std::nullopt;
#endif
  }
  else
  {
    std::cerr << "unknown toolchain: " << options.toolchain << '\n';
    return std::nullopt;
  }

  const std::string compiler_identity = ReadCommand(compiler + " --version 2>&1");
  if (compiler_identity.empty())
  {
    std::cerr << "compiler is unavailable: " << compiler << '\n';
    return std::nullopt;
  }
#if defined(__x86_64__) || defined(_M_X64)
  constexpr std::string_view architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  constexpr std::string_view architecture = "aarch64";
#else
  constexpr std::string_view architecture = "unsupported";
#endif
  const bool lto_enabled = compiler == "clang" && options.max_optimization;
  const unsigned optimization_level =
      options.fast_build ? 0u : (options.max_optimization ? 2u : 1u);
  std::string hot_dol_chunks;
  for (const std::string& chunk : options.hot_dol_chunks)
  {
    if (!hot_dol_chunks.empty())
      hot_dol_chunks += ';';
    hot_dol_chunks += chunk;
  }
  const fs::path source_root = fs::path(MODERNGEKKO_SOURCE_DIR);
  const fs::path dolrecomp = SiblingExecutable(argv0, "dolrecomp");
  const std::vector<fs::path> rel_files = CollectRelFiles(game.root / "files");
  std::vector<moderngekko::port::RelArchive> rel_archives;
  std::string rel_archive_error;
  if (!moderngekko::port::CollectRelArchives(game.root / "files", &rel_archives,
                                             &rel_archive_error))
  {
    std::cerr << "failed to inspect REL archives: " << rel_archive_error << '\n';
    return std::nullopt;
  }
  std::size_t archived_rel_count = 0;
  for (const auto& archive : rel_archives)
    archived_rel_count += archive.rels.size();
  const std::size_t rel_count = rel_files.size() + archived_rel_count;
  const std::string rel_fingerprint =
      FingerprintRelSources(game.root, rel_files, rel_archives);
  if (rel_count != 0)
  {
    std::cout << "REL sources: " << rel_files.size() << " loose + " << archived_rel_count
              << " archived (" << rel_count << " total)\n";
  }
  std::string flags;
  if (compiler == "clang")
  {
    flags = "compile:-O" + std::to_string(optimization_level) +
            " -fvisibility=hidden -ffp-contract=off -fno-fast-math ";
    flags += lto_enabled ? "-flto=thin link:-flto=thin" : "link:no-lto";
#if defined(__linux__)
    flags += " -fuse-ld=lld";
#endif
  }
  else if (compiler == "gcc")
  {
    flags = "compile:-O" + std::to_string(optimization_level) +
            " -fvisibility=hidden -ffp-contract=off -fno-fast-math link:no-lto";
  }
  else
  {
    flags = optimization_level == 0 ? "compile:/Od /fp:strict" :
                                      "compile:/O2 /fp:strict";
  }
  std::string build_identity = std::string(RECOMPCORE_REVISION) + "|dolrecomp=" +
      std::string(RECOMPCORE_REVISION) + "|module-abi=" +
      std::to_string(MODERNGEKKO_MODULE_ABI_VERSION) + "|cpu-abi=" +
      std::to_string(MODERNGEKKO_CPU_ABI_VERSION) + "|" + compiler_identity + "|" +
      std::string(architecture) + "|" + flags + "|sparse-patch-dispatch=v1" +
      "|dolrecomp-bin=" + FingerprintFile(dolrecomp) +
      "|gxruntime-tree=" + FingerprintTree(source_root / "vendor/dolphin/GXRuntime") +
      "|module-template-tree=" +
      FingerprintTree(source_root / "vendor/dolphin/module-template");
  if (!hot_dol_chunks.empty())
    build_identity += "|hot-dol-chunks=" + hot_dol_chunks;
  if (rel_count != 0)
    build_identity += "|native-rels=" + rel_fingerprint +
                      "|rel-packaging=" + std::string(REL_PACKAGING_REVISION);
  std::string identity = build_identity;
  std::ostringstream module_patch_id;
  std::ostringstream module_patch_addresses_id;
  if (!module_patch_contents.empty())
  {
    module_patch_id << std::hex << std::setfill('0') << std::setw(16)
                    << Fnv1a(module_patch_contents);
    module_patch_addresses_id << std::hex << std::setfill('0') << std::setw(16)
                              << Fnv1a(module_patch_addresses_contents);
    identity += "|module-patch=" + module_patch_id.str() +
                "|module-patch-addresses=" + module_patch_addresses_id.str();
  }
  std::ostringstream key_tail;
  key_tail << std::hex << std::setfill('0') << std::setw(16) << Fnv1a(identity);
  const std::string cache_key = game.dol_sha256 + "-" + key_tail.str();
  const fs::path artifact = options.output / game.disc_id / cache_key;
  const fs::path module = artifact / ("g" + game.disc_id + "_recomp" + Suffix());
  // Patch contents belong in the immutable published artifact key, but not in
  // the mutable build workspace key. Reusing one workspace lets Ninja rebuild
  // only module_patch.c after an iterative patch edit instead of recompiling
  // every generated CPU chunk. Patched and unpatched source graphs stay apart.
  const std::string workspace_identity =
      build_identity + (module_patch_contents.empty() ? "|workspace=base-v2" :
                                                        "|workspace=patched-v2");
  std::ostringstream workspace_tail;
  workspace_tail << std::hex << std::setfill('0') << std::setw(16)
                 << Fnv1a(workspace_identity);
  const fs::path workspace = options.output / game.disc_id / ".work" /
                             (game.dol_sha256 + "-" + workspace_tail.str());
  const fs::path module_build = workspace / "module-build";
  const fs::path built = module_build / ("g" + game.disc_id + "_recomp" + Suffix());
  if (fs::is_regular_file(module))
  {
    fs::create_directories(options.output / game.disc_id);
    std::ofstream active(options.output / game.disc_id / "active-module.txt");
    active << module.string() << '\n';
    std::cout << "cache hit: " << module << '\n';
    return module;
  }

  const auto publish_module = [&]() -> std::optional<fs::path> {
    fs::create_directories(artifact);
    fs::copy_file(built, module, fs::copy_options::overwrite_existing);
    std::ofstream manifest(artifact / "manifest.txt");
    manifest << "disc_id=" << game.disc_id << '\n' << "dol_sha256=" << game.dol_sha256 << '\n'
             << "recompcore_revision=" << RECOMPCORE_REVISION << '\n'
             << "module_abi=" << MODERNGEKKO_MODULE_ABI_VERSION << '\n'
             << "cpu_abi=" << MODERNGEKKO_CPU_ABI_VERSION << '\n'
             << "compiler=" << compiler_identity << "architecture=" << architecture << '\n'
             << "flags=" << flags << '\n'
             << "hot_dol_chunks="
             << (hot_dol_chunks.empty() ? "none" : hot_dol_chunks) << '\n'
             << "rel_count=" << rel_count << '\n'
             << "rel_archive_count=" << rel_archives.size() << '\n'
             << "rel_fingerprint=" << rel_fingerprint << '\n'
             << "rel_packaging=" << REL_PACKAGING_REVISION << '\n'
             << "module_patch="
             << (module_patch_contents.empty() ? "none" : module_patch_id.str()) << '\n'
             << "module_patch_addresses="
             << (module_patch_addresses_contents.empty() ? "none" :
                                                          module_patch_addresses_id.str())
             << '\n';
    if (!module_patch_addresses_contents.empty())
      fs::copy_file(options.module_patch_addresses, artifact / "module_patch_addresses.txt",
                    fs::copy_options::overwrite_existing);
    fs::create_directories(options.output / game.disc_id);
    std::ofstream active(options.output / game.disc_id / "active-module.txt");
    active << module.string() << '\n';
    std::cout << "built module: " << module << '\n';
    return module;
  };
  fs::create_directories(workspace);
  const fs::path generated_parent = workspace / "dolrecomp-output";
  fs::path generated = game.platform == moderngekko::GamePlatform::Wii ?
      generated_parent / (game.disc_id + "_generated") : generated_parent / "generated";
  std::string generated_stem =
      game.platform == moderngekko::GamePlatform::Wii ? game.disc_id : "generated";
  const fs::path expected_header = generated / (generated_stem + ".h");
  const fs::path fallback_header = generated_parent / "generated" / "generated.h";
  if (!fs::is_regular_file(expected_header) && !fs::is_regular_file(fallback_header))
  {
    std::string generate = Quote(dolrecomp) + " -j" +
                           std::to_string(std::max(1u, std::thread::hardware_concurrency())) + " ";
    if (game.platform == moderngekko::GamePlatform::GameCube)
      generate +=
          "--cpu gekko --gamecube " + Quote(game.main_dol) + " " + Quote(generated_parent);
    else
      generate += "--cpu broadway " + Quote(game.main_dol) + " " + game.disc_id + " " +
                  Quote(generated_parent);
    if (!RunCommand(generate))
      return std::nullopt;
  }

  const fs::path rel_complete_marker = generated_parent / ".native_rels_complete";
  const std::string rel_marker_contents =
      std::string(REL_PACKAGING_REVISION) + "\n" + rel_fingerprint;
  if (rel_count != 0 &&
      (!fs::is_regular_file(rel_complete_marker) ||
       ReadFile(rel_complete_marker) != rel_marker_contents))
  {
    // A changed REL set must replace the previous catalog. Leaving a folder
    // for a removed module behind would silently package stale native code.
    std::error_code cleanup_error;
    fs::remove(rel_complete_marker, cleanup_error);
    cleanup_error.clear();
    fs::remove_all(generated / "rels", cleanup_error);
    if (cleanup_error)
    {
      std::cerr << "failed to clear stale REL output " << generated / "rels"
                << ": " << cleanup_error.message() << '\n';
      return std::nullopt;
    }
    const fs::path rel_input = generated_parent / "rel-input";
    fs::remove_all(rel_input, cleanup_error);
    if (cleanup_error)
    {
      std::cerr << "failed to clear staged REL input " << rel_input
                << ": " << cleanup_error.message() << '\n';
      return std::nullopt;
    }
    const fs::path loose_input = rel_input / "loose";
    for (const fs::path& source : rel_files)
    {
      const fs::path relative = fs::relative(source, game.root / "files");
      const fs::path destination = loose_input / relative;
      fs::create_directories(destination.parent_path(), cleanup_error);
      if (!cleanup_error)
      {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing,
                      cleanup_error);
      }
      if (cleanup_error)
      {
        std::cerr << "failed to stage loose REL " << source
                  << ": " << cleanup_error.message() << '\n';
        return std::nullopt;
      }
    }
    if (!moderngekko::port::ExtractRelArchives(rel_archives, rel_input / "archives",
                                               &rel_archive_error))
    {
      std::cerr << "failed to extract REL archives: " << rel_archive_error << '\n';
      return std::nullopt;
    }
    const std::string generate_rels =
        Quote(dolrecomp) + " -j" +
        std::to_string(std::max(1u, std::thread::hardware_concurrency())) +
        " --cpu gekko --gamecube " + Quote(rel_input) + " " +
        Quote(generated_parent);
    if (!RunCommand(generate_rels))
      return std::nullopt;
    std::ofstream marker(rel_complete_marker, std::ios::binary | std::ios::trunc);
    marker << rel_marker_contents;
    if (!marker)
    {
      std::cerr << "failed to write REL completion marker " << rel_complete_marker << '\n';
      return std::nullopt;
    }
  }

  // DolRecomp's optional title database affects output naming only. An
  // explicit --cpu broadway keeps Wii semantics even when that database is absent.
  if (!fs::is_regular_file(generated / (generated_stem + ".h")) &&
      fs::is_regular_file(generated_parent / "generated" / "generated.h"))
  {
    generated = generated_parent / "generated";
    generated_stem = "generated";
  }
  const fs::path emitted_header = generated / (generated_stem + ".h");
  if (!fs::is_regular_file(emitted_header))
  {
    std::cerr << "DolRecomp did not produce " << emitted_header << '\n';
    return std::nullopt;
  }
  if (emitted_header.filename() != "generated.h")
    fs::copy_file(emitted_header, generated / "generated.h", fs::copy_options::overwrite_existing);
  fs::copy_file(game.main_dol, generated / "main.dol", fs::copy_options::overwrite_existing);
  const fs::path emitted_smc = generated / (generated_stem + "_smc.txt");
  const fs::path normalized_smc = generated / "generated_smc.txt";
  if (fs::is_regular_file(emitted_smc))
  {
    if (emitted_smc != normalized_smc)
      fs::copy_file(emitted_smc, normalized_smc, fs::copy_options::overwrite_existing);
  }
  else
    std::ofstream{normalized_smc};
  const fs::path staged_module_patch = generated / "module_patch.c";
  const fs::path staged_module_patch_addresses = generated / "module_patch_addresses.txt";
  if (!module_patch_contents.empty())
  {
    fs::copy_file(options.module_patch, staged_module_patch, fs::copy_options::overwrite_existing);
    fs::copy_file(options.module_patch_addresses, staged_module_patch_addresses,
                  fs::copy_options::overwrite_existing);
  }
  else
  {
    std::error_code ec;
    fs::remove(staged_module_patch, ec);
    fs::remove(staged_module_patch_addresses, ec);
  }

  // Large generated chunks can make each Clang process consume substantial
  // memory.  A conservative default avoids swap storms on 8 GiB systems;
  // callers with more memory can opt into wider parallelism.
  const unsigned compile_jobs =
      options.jobs != 0
          ? options.jobs
          : std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
  std::string configure = "cmake -E env CMAKE_NINJA_FORCE_RESPONSE_FILE=1 cmake -S " +
      Quote(source_root / "vendor/dolphin/module-template") +
      " -B " + Quote(module_build) + " -G Ninja -DCMAKE_BUILD_TYPE=Release" +
      " -DCMAKE_C_COMPILER=" + compiler + " -DGAME_ID=" + game.disc_id +
      " -DMODERNGEKKO_ENABLE_LTO=" + std::string(lto_enabled ? "ON" : "OFF") +
      " -DMODERNGEKKO_FAST_BUILD=" + std::string(options.fast_build ? "ON" : "OFF") +
      " -DMODERNGEKKO_OPTIMIZATION_LEVEL=" +
      std::to_string(optimization_level) +
      " -DMODERNGEKKO_HOT_DOL_CHUNKS=" + Quote(hot_dol_chunks) +
      " -DGENERATED_DIR=" + Quote(generated) +
      " -DGXRUNTIME_DIR=" + Quote(source_root / "vendor/dolphin/GXRuntime") +
      " -DCHASSIS_ABI_DIR=" +
      Quote(source_root / "vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp");
  if (!RunCommand(configure) ||
      !RunCommand("cmake --build " + Quote(module_build) + " -j" +
                  std::to_string(compile_jobs)))
    return std::nullopt;

  if (!fs::is_regular_file(built))
  {
    std::cerr << "module build completed but did not produce " << built << '\n';
    return std::nullopt;
  }
  return publish_module();
}

void Usage()
{
  std::cerr << "usage: moderngekko-port inspect <game-root>\n"
               "       moderngekko-port build <game-root> [--toolchain auto|clang|gcc|msvc] [--output path] [--module-patch file.c --module-patch-addresses file.txt] [--jobs count] [--fast-build|--max-opt] [--hot-dol-chunk chunk.c]\n"
               "       moderngekko-port run <game-root> [build options] [-- runner options]\n";
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    Usage();
    return 2;
  }
  const std::string command = argv[1];
  const fs::path root = argv[2];
  BuildOptions options;
  bool runner_args = false;
  for (int i = 3; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (runner_args)
      options.runner_arguments.push_back(arg);
    else if (arg == "--")
      runner_args = true;
    else if (arg == "--toolchain" && i + 1 < argc)
      options.toolchain = argv[++i];
    else if (arg == "--output" && i + 1 < argc)
      options.output = argv[++i];
    else if (arg == "--module-patch" && i + 1 < argc)
      options.module_patch = argv[++i];
    else if (arg == "--module-patch-addresses" && i + 1 < argc)
      options.module_patch_addresses = argv[++i];
    else if (arg == "--jobs" && i + 1 < argc)
    {
      try
      {
        const std::string value = argv[++i];
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed == 0 || parsed > 128)
          throw std::out_of_range("jobs");
        options.jobs = static_cast<unsigned>(parsed);
      }
      catch (const std::exception&)
      {
        std::cerr << "--jobs must be an integer from 1 through 128\n";
        return 2;
      }
    }
    else if (arg == "--fast-build")
      options.fast_build = true;
    else if (arg == "--max-opt")
      options.max_optimization = true;
    else if (arg == "--hot-dol-chunk" && i + 1 < argc)
    {
      const std::string chunk = argv[++i];
      const fs::path chunk_path(chunk);
      if (chunk_path.filename() != chunk_path || !chunk.starts_with("chunk_") ||
          !chunk.ends_with(".c"))
      {
        std::cerr << "--hot-dol-chunk must be a generated DOL chunk basename\n";
        return 2;
      }
      options.hot_dol_chunks.push_back(chunk);
    }
    else if (command == "run")
      options.runner_arguments.push_back(arg);
    else
    {
      std::cerr << "unknown or incomplete option: " << arg << '\n';
      return 2;
    }
  }
  if (options.fast_build && options.max_optimization)
  {
    std::cerr << "--fast-build and --max-opt are mutually exclusive\n";
    return 2;
  }
  if (options.output.empty())
    options.output = DefaultOutput();
  if (command == "inspect")
    return Inspect(root, options.output);
  if (command != "build" && command != "run")
  {
    Usage();
    return 2;
  }
  const auto module = Build(argv[0], root, options);
  if (!module)
    return 1;
  if (command == "build")
    return 0;
  std::string run = Quote(SiblingExecutable(argv[0], "moderngekko-run")) + " --game " +
                    Quote(root) + " --module " + Quote(*module);
  for (const std::string& arg : options.runner_arguments)
    run += " " + Quote(arg);
  return std::system(run.c_str()) == 0 ? 0 : 1;
}

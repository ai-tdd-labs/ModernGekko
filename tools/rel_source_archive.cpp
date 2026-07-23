#include "rel_source_archive.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <system_error>

namespace fs = std::filesystem;

namespace moderngekko::port
{
namespace
{
constexpr std::uint32_t RARC_MAGIC = 0x52415243;
constexpr std::size_t RARC_HEADER_SIZE = 0x20;
constexpr std::size_t RARC_INFO_SIZE = 0x18;
constexpr std::size_t RARC_ENTRY_SIZE = 0x14;

std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
  return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

bool RangeFits(std::size_t offset, std::size_t size, std::size_t total)
{
  return offset <= total && size <= total - offset;
}

bool AddOffset(std::size_t base, std::uint32_t relative, std::size_t* result)
{
  if (relative > std::numeric_limits<std::size_t>::max() - base)
    return false;
  *result = base + relative;
  return true;
}

std::string Lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsRelName(const std::string& name)
{
  return Lower(fs::path(name).extension().string()) == ".rel";
}

std::string SafeName(const std::string& name, std::size_t index)
{
  std::string result = fs::path(name).filename().string();
  for (char& c : result)
  {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (!std::isalnum(byte) && c != '.' && c != '_' && c != '-')
      c = '_';
  }
  if (result.empty() || result == "." || result == "..")
    result = "module_" + std::to_string(index) + ".rel";
  return std::to_string(index) + "_" + result;
}

bool ReadBytes(const fs::path& path, std::vector<std::uint8_t>* bytes, std::string* error)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    *error = "cannot open " + path.string();
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0 || static_cast<std::uintmax_t>(size) >
                      static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
  {
    *error = "invalid file size for " + path.string();
    return false;
  }
  file.seekg(0, std::ios::beg);
  bytes->resize(static_cast<std::size_t>(size));
  if (!bytes->empty())
    file.read(reinterpret_cast<char*>(bytes->data()), size);
  if (!file)
  {
    *error = "cannot read " + path.string();
    return false;
  }
  return true;
}
}

bool ParseRarcRels(std::span<const std::uint8_t> bytes, std::vector<ArchivedRel>* rels,
                   std::string* error)
{
  rels->clear();
  error->clear();
  if (bytes.size() < 4 || ReadU32(bytes, 0) != RARC_MAGIC)
    return true;
  if (!RangeFits(0, RARC_HEADER_SIZE, bytes.size()))
  {
    *error = "truncated RARC header";
    return false;
  }

  const std::uint32_t declared_size = ReadU32(bytes, 4);
  const std::uint32_t info_offset_raw = ReadU32(bytes, 8);
  const std::uint32_t data_offset_raw = ReadU32(bytes, 12);
  if (declared_size > bytes.size())
  {
    *error = "RARC declared size exceeds file";
    return false;
  }

  const std::size_t info_offset = info_offset_raw;
  std::size_t data_offset = 0;
  if (!AddOffset(RARC_HEADER_SIZE, data_offset_raw, &data_offset) ||
      !RangeFits(info_offset, RARC_INFO_SIZE, bytes.size()))
  {
    *error = "RARC info or data offset is out of range";
    return false;
  }

  const std::uint32_t entry_count = ReadU32(bytes, info_offset + 8);
  const std::uint32_t entry_offset_raw = ReadU32(bytes, info_offset + 12);
  const std::uint32_t string_size = ReadU32(bytes, info_offset + 16);
  const std::uint32_t string_offset_raw = ReadU32(bytes, info_offset + 20);
  const std::uint64_t entry_bytes =
      static_cast<std::uint64_t>(entry_count) * RARC_ENTRY_SIZE;
  std::size_t entry_offset = 0;
  std::size_t string_offset = 0;
  if (!AddOffset(info_offset, entry_offset_raw, &entry_offset) ||
      !AddOffset(info_offset, string_offset_raw, &string_offset) ||
      entry_bytes > bytes.size() ||
      !RangeFits(entry_offset, static_cast<std::size_t>(entry_bytes), bytes.size()) ||
      !RangeFits(string_offset, string_size, bytes.size()))
  {
    *error = "RARC entry or string table is out of range";
    return false;
  }

  for (std::uint32_t i = 0; i < entry_count; ++i)
  {
    const std::size_t entry = entry_offset + static_cast<std::size_t>(i) * RARC_ENTRY_SIZE;
    const std::uint16_t id = ReadU16(bytes, entry);
    const std::uint32_t type_and_name = ReadU32(bytes, entry + 4);
    const std::uint8_t attributes = static_cast<std::uint8_t>(type_and_name >> 24);
    if (id == 0xffff || (attributes & 1) == 0)
      continue;

    const std::uint32_t name_relative = type_and_name & 0x00ffffff;
    if (name_relative >= string_size)
    {
      *error = "RARC file name is outside the string table";
      return false;
    }
    const std::size_t name_offset = string_offset + name_relative;
    const auto name_end = std::find(bytes.begin() + name_offset,
                                    bytes.begin() + string_offset + string_size, 0);
    if (name_end == bytes.begin() + string_offset + string_size)
    {
      *error = "RARC file name is not terminated";
      return false;
    }
    const std::string name(reinterpret_cast<const char*>(bytes.data() + name_offset),
                           static_cast<std::size_t>(name_end - (bytes.begin() + name_offset)));
    if (!IsRelName(name))
      continue;

    const std::uint32_t file_offset_raw = ReadU32(bytes, entry + 8);
    const std::uint32_t file_size = ReadU32(bytes, entry + 12);
    std::size_t file_offset = 0;
    if (!AddOffset(data_offset, file_offset_raw, &file_offset) ||
        !RangeFits(file_offset, file_size, bytes.size()))
    {
      *error = "RARC REL data is out of range";
      return false;
    }
    ArchivedRel rel;
    rel.name = name;
    rel.bytes.assign(bytes.begin() + file_offset, bytes.begin() + file_offset + file_size);
    rels->push_back(std::move(rel));
  }
  return true;
}

bool CollectRelArchives(const fs::path& root, std::vector<RelArchive>* archives,
                        std::string* error)
{
  archives->clear();
  error->clear();
  if (!fs::is_directory(root))
    return true;

  std::vector<fs::path> candidates;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
  {
    if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".arc")
      candidates.push_back(entry.path());
  }
  std::sort(candidates.begin(), candidates.end());

  for (const fs::path& path : candidates)
  {
    std::ifstream magic_file(path, std::ios::binary);
    std::array<std::uint8_t, 4> magic{};
    magic_file.read(reinterpret_cast<char*>(magic.data()), magic.size());
    if (magic_file.gcount() != static_cast<std::streamsize>(magic.size()) ||
        ReadU32(magic, 0) != RARC_MAGIC)
      continue;

    std::vector<std::uint8_t> bytes;
    if (!ReadBytes(path, &bytes, error))
      return false;
    RelArchive archive;
    archive.path = path;
    if (!ParseRarcRels(bytes, &archive.rels, error))
    {
      *error = path.string() + ": " + *error;
      return false;
    }
    if (!archive.rels.empty())
      archives->push_back(std::move(archive));
  }
  return true;
}

bool ExtractRelArchives(const std::vector<RelArchive>& archives, const fs::path& output,
                        std::string* error)
{
  error->clear();
  std::error_code ec;
  fs::remove_all(output, ec);
  if (ec)
  {
    *error = "cannot clear " + output.string() + ": " + ec.message();
    return false;
  }
  for (std::size_t archive_index = 0; archive_index < archives.size(); ++archive_index)
  {
    const fs::path directory = output / ("archive_" + std::to_string(archive_index));
    fs::create_directories(directory, ec);
    if (ec)
    {
      *error = "cannot create " + directory.string() + ": " + ec.message();
      return false;
    }
    for (std::size_t rel_index = 0; rel_index < archives[archive_index].rels.size(); ++rel_index)
    {
      const ArchivedRel& rel = archives[archive_index].rels[rel_index];
      const fs::path destination = directory / SafeName(rel.name, rel_index);
      std::ofstream file(destination, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(rel.bytes.data()),
                 static_cast<std::streamsize>(rel.bytes.size()));
      if (!file)
      {
        *error = "cannot write " + destination.string();
        return false;
      }
    }
  }
  return true;
}
}

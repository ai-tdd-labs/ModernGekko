#include "rel_source_archive.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using moderngekko::port::ArchivedRel;

namespace
{
void WriteU16(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint16_t value)
{
  (*bytes)[offset] = static_cast<std::uint8_t>(value >> 8);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteU32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value)
{
  (*bytes)[offset] = static_cast<std::uint8_t>(value >> 24);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 16);
  (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 8);
  (*bytes)[offset + 3] = static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> MakeArchive()
{
  constexpr std::size_t info = 0x20;
  constexpr std::size_t entries = 0x40;
  constexpr std::size_t strings = 0x68;
  constexpr std::size_t data = 0x80;
  std::vector<std::uint8_t> bytes(data + 8);
  WriteU32(&bytes, 0, 0x52415243);
  WriteU32(&bytes, 4, static_cast<std::uint32_t>(bytes.size()));
  WriteU32(&bytes, 8, info);
  WriteU32(&bytes, 12, data - 0x20);
  WriteU32(&bytes, info + 8, 2);
  WriteU32(&bytes, info + 12, entries - info);
  WriteU32(&bytes, info + 16, 24);
  WriteU32(&bytes, info + 20, strings - info);

  WriteU16(&bytes, entries, 1);
  WriteU32(&bytes, entries + 4, 0x01000000);
  WriteU32(&bytes, entries + 8, 0);
  WriteU32(&bytes, entries + 12, 4);
  WriteU16(&bytes, entries + 20, 2);
  WriteU32(&bytes, entries + 24, 0x0100000e);
  WriteU32(&bytes, entries + 28, 4);
  WriteU32(&bytes, entries + 32, 4);

  const char names[] = "actor.rel\0ignore.bin\0";
  std::copy(std::begin(names), std::end(names), bytes.begin() + strings);
  bytes[data] = 'Y';
  bytes[data + 1] = 'a';
  bytes[data + 2] = 'z';
  bytes[data + 3] = '0';
  bytes[data + 4] = 1;
  bytes[data + 5] = 2;
  bytes[data + 6] = 3;
  bytes[data + 7] = 4;
  return bytes;
}
}

int main()
{
  std::vector<ArchivedRel> rels;
  std::string error;
  const std::vector<std::uint8_t> archive = MakeArchive();
  assert(moderngekko::port::ParseRarcRels(archive, &rels, &error));
  assert(error.empty());
  assert(rels.size() == 1);
  assert(rels[0].name == "actor.rel");
  assert(rels[0].bytes == std::vector<std::uint8_t>({'Y', 'a', 'z', '0'}));

  std::vector<std::uint8_t> truncated = archive;
  truncated.resize(0x82);
  assert(!moderngekko::port::ParseRarcRels(truncated, &rels, &error));
  assert(!error.empty());

  const fs::path output = fs::temp_directory_path() / "moderngekko-rel-archive-test";
  std::error_code ec;
  fs::remove_all(output, ec);
  moderngekko::port::RelArchive source;
  source.path = "synthetic.arc";
  source.rels.push_back({"../unsafe.rel", {1, 2, 3}});
  assert(moderngekko::port::ExtractRelArchives({source}, output, &error));
  const fs::path extracted = output / "archive_0" / "0_unsafe.rel";
  assert(fs::is_regular_file(extracted));
  assert(!fs::exists(output.parent_path() / "unsafe.rel"));
  fs::remove_all(output, ec);
  return 0;
}

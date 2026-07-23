#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace moderngekko::port
{
struct ArchivedRel
{
  std::string name;
  std::vector<std::uint8_t> bytes;
};

struct RelArchive
{
  std::filesystem::path path;
  std::vector<ArchivedRel> rels;
};

// A non-RARC file is not an error and produces an empty result. Malformed
// archives fail closed so a damaged game extraction cannot publish a partial
// native REL catalog.
bool ParseRarcRels(std::span<const std::uint8_t> bytes, std::vector<ArchivedRel>* rels,
                   std::string* error);

bool CollectRelArchives(const std::filesystem::path& root, std::vector<RelArchive>* archives,
                        std::string* error);

bool ExtractRelArchives(const std::vector<RelArchive>& archives,
                        const std::filesystem::path& output, std::string* error);
}

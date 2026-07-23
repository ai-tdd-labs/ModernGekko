#include "moderngekko/module.h"

#include <cstdint>

namespace
{
void ChunkA(CPUState* state, std::uint32_t canonical_pc, intptr_t section_delta)
{
  state->gpr[0] = canonical_pc;
  state->gpr[1] = static_cast<std::uint32_t>(section_delta);
  state->gpr[2] = 0xA0u;
}

void ChunkB(CPUState* state, std::uint32_t canonical_pc, intptr_t section_delta)
{
  state->gpr[0] = canonical_pc;
  state->gpr[1] = static_cast<std::uint32_t>(section_delta);
  state->gpr[2] = 0xB0u;
}

void ChunkC(CPUState* state, std::uint32_t canonical_pc, intptr_t section_delta)
{
  state->gpr[0] = canonical_pc;
  state->gpr[1] = static_cast<std::uint32_t>(section_delta);
  state->gpr[2] = 0xC0u;
}

constexpr ModernGekkoRange rel_chunk_ranges[] = {
    {0x90001000u, 0x90001008u},
    {0x90001008u, 0x90001010u},
    {0x90002000u, 0x90002010u},
};

constexpr ModernGekkoRelChunkFn rel_chunk_functions[] = {
    ChunkA,
    ChunkB,
    ChunkC,
};

constexpr ModernGekkoRelExecutableSection rel_sections[] = {
    {1u, 0x00u, 0x10u, 0x90001000u, rel_chunk_ranges, 2u, nullptr, nullptr,
     rel_chunk_functions},
    {3u, 0x20u, 0x10u, 0x90002000u, rel_chunk_ranges + 2u, 1u, nullptr, nullptr,
     rel_chunk_functions + 2u},
};

constexpr ModernGekkoRelModuleDesc rel_module = {
    17u,
    rel_sections,
    2u,
};

constexpr ModernGekkoRange overlap_chunk_ranges[] = {
    {0x91000000u, 0x91000008u},
};

constexpr ModernGekkoRelChunkFn overlap_chunk_functions[] = {
    ChunkA,
};

constexpr ModernGekkoRelExecutableSection overlap_sections[] = {
    {1u, 0x08u, 0x08u, 0x91000000u, overlap_chunk_ranges, 1u, nullptr, nullptr,
     overlap_chunk_functions},
};

constexpr ModernGekkoRelModuleDesc overlap_module = {
    18u,
    overlap_sections,
    1u,
};
}

int main()
{
  ModernGekkoRelBindingRegistry* registry =
      moderngekko_rel_binding_registry_create();
  if (registry == nullptr)
    return 1;

  const std::uint32_t initial_bases[] = {0u, 0x80500000u, 0u, 0x80600000u};
  const ModernGekkoRelBindingDesc initial_binding = {
      &rel_module,
      1u,
      initial_bases,
      4u,
  };
  if (!moderngekko_rel_binding_registry_link(registry, &initial_binding))
    return 2;

  ModernGekkoRelBindingLookup lookup = {};
  if (!moderngekko_rel_binding_registry_lookup(registry, 0x80500000u, &lookup))
    return 3;
  if (lookup.module_id != 17u || lookup.generation != 1u ||
      lookup.section_index != 1u || lookup.section_base != 0x80500000u ||
      lookup.section_offset != 0u || lookup.canonical_pc != 0x90001000u)
  {
    return 4;
  }
  if (lookup.section_delta !=
      static_cast<intptr_t>(0x90001000ull - 0x80500000ull))
  {
    return 5;
  }
  if (!moderngekko_rel_binding_registry_lookup(registry, 0x8050000Fu, &lookup) ||
      lookup.section_offset != 0x0Fu || lookup.canonical_pc != 0x9000100Fu)
  {
    return 6;
  }
  if (moderngekko_rel_binding_registry_lookup(registry, 0x80500010u, &lookup))
    return 7;
  if (moderngekko_rel_binding_registry_lookup(registry, 0x80600010u, &lookup))
    return 8;
  if (!moderngekko_rel_binding_registry_lookup(registry, 0x80600020u, &lookup) ||
      lookup.section_index != 3u || lookup.section_base != 0x80600000u ||
      lookup.section_offset != 0x20u || lookup.canonical_pc != 0x90002000u)
  {
    return 9;
  }

  std::uint32_t section_base = 0u;
  if (!moderngekko_rel_binding_registry_lookup_section_base(
          registry, 17u, 1u, 1u, &section_base) ||
      section_base != 0x80500000u)
  {
    return 10;
  }
  if (!moderngekko_rel_binding_registry_lookup_section_base(
          registry, 17u, 1u, 3u, &section_base) ||
      section_base != 0x80600000u)
  {
    return 11;
  }
  if (moderngekko_rel_binding_registry_lookup_section_base(
          registry, 17u, 1u, 2u, &section_base))
  {
    return 12;
  }

  ModernGekkoRelChunkLookup chunk_lookup = {};
  CPUState state = {};
  if (!moderngekko_rel_binding_registry_lookup_chunk(registry, 0x80500004u,
                                                     &chunk_lookup))
  {
    return 13;
  }
  if (chunk_lookup.canonical_pc != 0x90001004u ||
      chunk_lookup.chunk_index != 0u ||
      chunk_lookup.chunk_function != ChunkA ||
      chunk_lookup.canonical_chunk_range.start != 0x90001000u ||
      chunk_lookup.canonical_chunk_range.end != 0x90001008u)
  {
    return 14;
  }
  chunk_lookup.chunk_function(&state, chunk_lookup.canonical_pc,
                              chunk_lookup.section_delta);
  if (state.gpr[0] != 0x90001004u || state.gpr[2] != 0xA0u)
  {
    return 15;
  }
  if (!moderngekko_rel_binding_registry_lookup_chunk(registry, 0x80500009u,
                                                     &chunk_lookup) ||
      chunk_lookup.canonical_pc != 0x90001009u ||
      chunk_lookup.chunk_index != 1u ||
      chunk_lookup.chunk_function != ChunkB)
  {
    return 16;
  }
  if (!moderngekko_rel_binding_registry_lookup_chunk(registry, 0x80600028u,
                                                     &chunk_lookup) ||
      chunk_lookup.chunk_index != 0u ||
      chunk_lookup.chunk_function != ChunkC ||
      chunk_lookup.canonical_pc != 0x90002008u)
  {
    return 17;
  }

  const std::uint32_t overlapping_bases[] = {0u, 0x80500000u};
  const ModernGekkoRelBindingDesc overlapping_binding = {
      &overlap_module,
      1u,
      overlapping_bases,
      2u,
  };
  if (moderngekko_rel_binding_registry_link(registry, &overlapping_binding))
    return 18;
  if (!moderngekko_rel_binding_registry_lookup(registry, 0x80500000u, &lookup) ||
      lookup.module_id != 17u || lookup.generation != 1u)
  {
    return 19;
  }

  if (!moderngekko_rel_binding_registry_unlink(registry, 17u, 1u))
    return 20;
  if (moderngekko_rel_binding_registry_lookup(registry, 0x80500000u, &lookup))
    return 21;
  if (moderngekko_rel_binding_registry_unlink(registry, 17u, 1u))
    return 22;

  const std::uint32_t reused_bases[] = {0u, 0x80500000u};
  const ModernGekkoRelBindingDesc reused_binding = {
      &overlap_module,
      7u,
      reused_bases,
      2u,
  };
  if (!moderngekko_rel_binding_registry_link(registry, &reused_binding))
    return 23;
  if (!moderngekko_rel_binding_registry_lookup_chunk(registry, 0x80500008u,
                                                     &chunk_lookup) ||
      chunk_lookup.module_id != 18u || chunk_lookup.generation != 7u ||
      chunk_lookup.canonical_pc != 0x91000000u)
  {
    return 24;
  }
  if (!moderngekko_rel_binding_registry_unlink(registry, 18u, 7u))
    return 25;

  const std::uint32_t reloaded_bases[] = {0u, 0x80700000u, 0u, 0x80800000u};
  const ModernGekkoRelBindingDesc reloaded_binding = {
      &rel_module,
      2u,
      reloaded_bases,
      4u,
  };
  if (!moderngekko_rel_binding_registry_link(registry, &reloaded_binding))
    return 26;
  if (!moderngekko_rel_binding_registry_lookup_chunk(registry, 0x80700004u,
                                                     &chunk_lookup) ||
      chunk_lookup.module_id != 17u || chunk_lookup.generation != 2u ||
      chunk_lookup.canonical_pc != 0x90001004u ||
      chunk_lookup.chunk_function != ChunkA)
  {
    return 27;
  }
  if (!moderngekko_rel_binding_registry_lookup_chunk(registry, 0x80800028u,
                                                     &chunk_lookup) ||
      chunk_lookup.canonical_pc != 0x90002008u ||
      chunk_lookup.chunk_function != ChunkC)
  {
    return 28;
  }

  const std::uint32_t missing_section_bases[] = {0u, 0x80900000u, 0u, 0u};
  const ModernGekkoRelBindingDesc missing_section_binding = {
      &rel_module,
      3u,
      missing_section_bases,
      4u,
  };
  if (moderngekko_rel_binding_registry_link(registry, &missing_section_binding))
    return 29;
  if (!moderngekko_rel_binding_registry_lookup(registry, 0x80800020u, &lookup) ||
      lookup.generation != 2u)
  {
    return 30;
  }

  moderngekko_rel_binding_registry_destroy(registry);
  return 0;
}

#include "moderngekko/module.h"

#include <algorithm>
#include <stdbool.h>
#include <new>
#include <stddef.h>
#include <string.h>
#include <vector>

struct ModernGekkoRelBindingRegistry
{
    struct RangeBinding
    {
        uint32_t start;
        uint32_t end;
        uint32_t module_id;
        uint32_t generation;
        uint32_t section_index;
        uint32_t section_base;
        intptr_t section_delta;
        const ModernGekkoRelExecutableSection* section;
    };

    struct ModuleBinding
    {
        uint32_t module_id;
        uint32_t generation;
        std::vector<uint32_t> section_bases;
    };

    std::vector<RangeBinding> ranges;
    std::vector<ModuleBinding> modules;
};

static bool add_u32(uint32_t a, uint32_t b, uint32_t* result);

static bool ranges_are_valid(const ModernGekkoRange* ranges, uint32_t count)
{
    uint32_t i;

    if (ranges == NULL || count == 0u)
        return false;

    for (i = 0; i < count; ++i)
    {
        if (ranges[i].start >= ranges[i].end)
            return false;
        if (i != 0u && ranges[i - 1u].end > ranges[i].start)
            return false;
    }

    return true;
}

static bool rel_sections_are_valid(const ModernGekkoRelExecutableSection* sections,
                                   uint32_t count)
{
    uint32_t i;

    if (sections == NULL || count == 0u)
        return false;

    for (i = 0; i < count; ++i)
    {
        const uint32_t offset = sections[i].offset;
        const uint32_t size = sections[i].size;
        const uint32_t section_index = sections[i].section_index;
        uint32_t canonical_end;

        if (size == 0u || offset > UINT32_MAX - size ||
            !add_u32(sections[i].canonical_start, size, &canonical_end) ||
            !ranges_are_valid(sections[i].chunk_ranges, sections[i].num_chunk_ranges) ||
            sections[i].chunk_functions == NULL)
            return false;

        {
            uint32_t chunk_index;
            uint32_t cursor = sections[i].canonical_start;
            for (chunk_index = 0; chunk_index < sections[i].num_chunk_ranges; ++chunk_index)
            {
                const ModernGekkoRange chunk = sections[i].chunk_ranges[chunk_index];
                if (chunk.start != cursor || chunk.end > canonical_end ||
                    sections[i].chunk_functions[chunk_index] == NULL)
                {
                    return false;
                }
                cursor = chunk.end;
            }
            if (cursor != canonical_end)
                return false;
        }

        if (i == 0u)
            continue;

        if (sections[i - 1u].section_index > section_index)
            return false;
        if (sections[i - 1u].section_index == section_index &&
            sections[i - 1u].offset + sections[i - 1u].size > offset)
        {
            return false;
        }
    }

    return true;
}

static bool rel_catalog_is_valid(const ModernGekkoRelModuleDesc* rel_modules, uint32_t count)
{
    uint32_t i;
    uint32_t j;

    if (count == 0u)
        return rel_modules == NULL;
    if (rel_modules == NULL)
        return false;

    for (i = 0; i < count; ++i)
    {
        if (rel_modules[i].module_id == 0u ||
            !rel_sections_are_valid(rel_modules[i].executable_sections,
                                    rel_modules[i].num_executable_sections))
        {
            return false;
        }

        for (j = i + 1u; j < count; ++j)
        {
            if (rel_modules[i].module_id == rel_modules[j].module_id)
                return false;
        }
    }

    return true;
}

static bool chunks_tile_code(const ModernGekkoModuleDesc* descriptor)
{
    uint32_t code_index;
    uint32_t chunk_index = 0u;

    if (!ranges_are_valid(descriptor->chunk_ranges, descriptor->num_chunk_ranges) ||
        descriptor->chunk_hashes == NULL || descriptor->chunk_functions == NULL)
    {
        return false;
    }

    for (code_index = 0; code_index < descriptor->num_code_ranges; ++code_index)
    {
        const ModernGekkoRange code = descriptor->code_ranges[code_index];
        uint32_t cursor = code.start;

        while (chunk_index < descriptor->num_chunk_ranges &&
               descriptor->chunk_ranges[chunk_index].start < code.end)
        {
            const ModernGekkoRange chunk = descriptor->chunk_ranges[chunk_index];

            if (chunk.start != cursor || chunk.end > code.end ||
                descriptor->chunk_functions[chunk_index] == NULL)
                return false;

            cursor = chunk.end;
            ++chunk_index;
        }

        if (cursor != code.end)
            return false;
    }

    return chunk_index == descriptor->num_chunk_ranges;
}

static bool address_is_covered(const ModernGekkoRange* ranges, uint32_t count, uint32_t address)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
    {
        if (address >= ranges[i].start && address < ranges[i].end)
            return true;
    }
    return false;
}

static bool add_u32(uint32_t a, uint32_t b, uint32_t* result)
{
    if (result == NULL || a > UINT32_MAX - b)
        return false;
    *result = a + b;
    return true;
}

static bool delta_from_addresses(uint32_t canonical_start, uint32_t actual_start,
                                 intptr_t* result)
{
    const int64_t delta = (int64_t)(uint64_t)canonical_start - (int64_t)(uint64_t)actual_start;

    if (result == NULL || delta < (int64_t)INTPTR_MIN || delta > (int64_t)INTPTR_MAX)
        return false;

    *result = (intptr_t)delta;
    return true;
}

static bool add_delta_u32(uint32_t value, intptr_t delta, uint32_t* result)
{
    const int64_t translated = (int64_t)(uint64_t)value + (int64_t)delta;

    if (result == NULL || translated < 0 || translated > (int64_t)UINT32_MAX)
        return false;

    *result = (uint32_t)translated;
    return true;
}

static bool overlaps(const ModernGekkoRelBindingRegistry::RangeBinding& lhs,
                     const ModernGekkoRelBindingRegistry::RangeBinding& rhs)
{
    return lhs.start < rhs.end && rhs.start < lhs.end;
}

static bool build_rel_binding(
    const ModernGekkoRelBindingDesc* binding,
    std::vector<ModernGekkoRelBindingRegistry::RangeBinding>* out_ranges,
    ModernGekkoRelBindingRegistry::ModuleBinding* out_module)
{
    uint32_t i;

    if (binding == NULL || out_ranges == NULL || out_module == NULL ||
        binding->module == NULL || binding->generation == 0u ||
        binding->section_bases == NULL || binding->num_section_bases == 0u ||
        binding->module->module_id == 0u ||
        !rel_sections_are_valid(binding->module->executable_sections,
                                binding->module->num_executable_sections))
    {
        return false;
    }

    out_ranges->clear();
    out_ranges->reserve(binding->module->num_executable_sections);

    out_module->module_id = binding->module->module_id;
    out_module->generation = binding->generation;
    out_module->section_bases.assign(binding->section_bases,
                                     binding->section_bases +
                                         binding->num_section_bases);

    for (i = 0; i < binding->module->num_executable_sections; ++i)
    {
        const ModernGekkoRelExecutableSection section =
            binding->module->executable_sections[i];
        ModernGekkoRelBindingRegistry::RangeBinding range = {};
        uint32_t start;
        uint32_t end;

        if (section.section_index >= binding->num_section_bases)
            return false;
        range.section_base = binding->section_bases[section.section_index];
        if (range.section_base == 0u || !add_u32(range.section_base, section.offset, &start) ||
            !add_u32(start, section.size, &end) ||
            !delta_from_addresses(section.canonical_start, start, &range.section_delta))
        {
            return false;
        }

        range.start = start;
        range.end = end;
        range.module_id = binding->module->module_id;
        range.generation = binding->generation;
        range.section_index = section.section_index;
        range.section = &binding->module->executable_sections[i];
        out_ranges->push_back(range);
    }

    std::sort(out_ranges->begin(), out_ranges->end(),
              [](const ModernGekkoRelBindingRegistry::RangeBinding& lhs,
                 const ModernGekkoRelBindingRegistry::RangeBinding& rhs) {
                  return lhs.start < rhs.start;
              });

    for (i = 1u; i < out_ranges->size(); ++i)
    {
        if (out_ranges->at(i - 1u).end > out_ranges->at(i).start)
            return false;
    }

    return true;
}

ModernGekkoModuleStatus moderngekko_validate_module(
    const ModernGekkoModuleDesc* descriptor,
    const ModernGekkoModuleRequirements* requirements)
{
    if (descriptor == NULL || requirements == NULL)
        return MODERNGEKKO_MODULE_NULL_DESCRIPTOR;
    if (descriptor->abi_version != MODERNGEKKO_MODULE_ABI_VERSION &&
        descriptor->abi_version != MODERNGEKKO_MODULE_ABI_VERSION_V3)
    {
        return MODERNGEKKO_MODULE_ABI_MISMATCH;
    }
    if (descriptor->cpu_abi_version != requirements->cpu_abi_version)
        return MODERNGEKKO_MODULE_CPU_ABI_MISMATCH;
    if (descriptor->cpu_state_size != requirements->cpu_state_size)
        return MODERNGEKKO_MODULE_CPU_STATE_SIZE_MISMATCH;
    if (memchr(descriptor->game_id, '\0', sizeof(descriptor->game_id)) == NULL ||
        descriptor->game_id[0] == '\0')
    {
        return MODERNGEKKO_MODULE_INVALID_GAME_ID;
    }
    if (requirements->game_id != NULL &&
        strcmp(descriptor->game_id, requirements->game_id) != 0)
    {
        return MODERNGEKKO_MODULE_GAME_ID_MISMATCH;
    }
    if (descriptor->dispatch == NULL)
        return MODERNGEKKO_MODULE_MISSING_DISPATCH;
    if (!ranges_are_valid(descriptor->code_ranges, descriptor->num_code_ranges))
        return MODERNGEKKO_MODULE_INVALID_CODE_RANGES;
    if (descriptor->num_smc_ranges != 0u &&
        !ranges_are_valid(descriptor->smc_ranges, descriptor->num_smc_ranges))
    {
        return MODERNGEKKO_MODULE_INVALID_SMC_RANGES;
    }
    if (!chunks_tile_code(descriptor))
        return MODERNGEKKO_MODULE_INVALID_CHUNKS;
    if (descriptor->abi_version >= MODERNGEKKO_MODULE_ABI_VERSION_V4 &&
        !rel_catalog_is_valid(descriptor->rel_modules, descriptor->num_rel_modules))
    {
        return MODERNGEKKO_MODULE_INVALID_REL_CATALOG;
    }
    if (!address_is_covered(descriptor->code_ranges, descriptor->num_code_ranges,
                            descriptor->entry_point))
        return MODERNGEKKO_MODULE_ENTRY_POINT_UNCOVERED;

    return MODERNGEKKO_MODULE_OK;
}

const char* moderngekko_module_status_string(ModernGekkoModuleStatus status)
{
    static const char* const messages[] = {
        "ok",
        "null descriptor or requirements",
        "module ABI mismatch",
        "CPU ABI mismatch",
        "CPU state size mismatch",
        "invalid game ID",
        "game ID mismatch",
        "missing dispatch function",
        "invalid code ranges",
        "invalid SMC ranges",
        "invalid chunks",
        "invalid REL catalog",
        "entry point is not covered by code ranges",
    };

    if ((unsigned)status >= sizeof(messages) / sizeof(messages[0]))
        return "unknown module status";

    return messages[status];
}

ModernGekkoRelBindingRegistry* moderngekko_rel_binding_registry_create(void)
{
    return new (std::nothrow) ModernGekkoRelBindingRegistry();
}

void moderngekko_rel_binding_registry_destroy(ModernGekkoRelBindingRegistry* registry)
{
    delete registry;
}

void moderngekko_rel_binding_registry_clear(ModernGekkoRelBindingRegistry* registry)
{
    if (registry == NULL)
        return;

    registry->ranges.clear();
    registry->modules.clear();
}

bool moderngekko_rel_binding_registry_link(
    ModernGekkoRelBindingRegistry* registry,
    const ModernGekkoRelBindingDesc* binding)
{
    std::vector<ModernGekkoRelBindingRegistry::RangeBinding> pending_ranges;
    ModernGekkoRelBindingRegistry::ModuleBinding pending_module;
    const ModernGekkoRelBindingRegistry::RangeBinding* pending_data;
    uint32_t i;

    if (registry == NULL || !build_rel_binding(binding, &pending_ranges, &pending_module))
        return false;

    for (const ModernGekkoRelBindingRegistry::ModuleBinding& module : registry->modules)
    {
        if (module.module_id == pending_module.module_id)
            return false;
    }

    pending_data = pending_ranges.data();
    for (i = 0; i < pending_ranges.size(); ++i)
    {
        for (const ModernGekkoRelBindingRegistry::RangeBinding& live_range : registry->ranges)
        {
            if (overlaps(pending_data[i], live_range))
                return false;
        }
    }

    registry->modules.push_back(std::move(pending_module));
    registry->ranges.insert(registry->ranges.end(), pending_ranges.begin(), pending_ranges.end());
    std::sort(registry->ranges.begin(), registry->ranges.end(),
              [](const ModernGekkoRelBindingRegistry::RangeBinding& lhs,
                 const ModernGekkoRelBindingRegistry::RangeBinding& rhs) {
                  return lhs.start < rhs.start;
              });
    return true;
}

bool moderngekko_rel_binding_registry_unlink(ModernGekkoRelBindingRegistry* registry,
                                             uint32_t module_id, uint32_t generation)
{
    std::vector<ModernGekkoRelBindingRegistry::ModuleBinding>::iterator module_it;
    std::vector<ModernGekkoRelBindingRegistry::RangeBinding>::iterator range_end;

    if (registry == NULL || module_id == 0u || generation == 0u)
        return false;

    module_it = std::find_if(
        registry->modules.begin(), registry->modules.end(),
        [module_id, generation](const ModernGekkoRelBindingRegistry::ModuleBinding& module) {
            return module.module_id == module_id && module.generation == generation;
        });
    if (module_it == registry->modules.end())
        return false;

    registry->modules.erase(module_it);
    range_end = std::remove_if(
        registry->ranges.begin(), registry->ranges.end(),
        [module_id, generation](const ModernGekkoRelBindingRegistry::RangeBinding& range) {
            return range.module_id == module_id && range.generation == generation;
        });
    registry->ranges.erase(range_end, registry->ranges.end());
    return true;
}

bool moderngekko_rel_binding_registry_lookup(
    const ModernGekkoRelBindingRegistry* registry,
    uint32_t address,
    ModernGekkoRelBindingLookup* out_binding)
{
    std::vector<ModernGekkoRelBindingRegistry::RangeBinding>::const_iterator it;

    if (registry == NULL)
        return false;

    it = std::upper_bound(
        registry->ranges.begin(), registry->ranges.end(), address,
        [](uint32_t guest_address, const ModernGekkoRelBindingRegistry::RangeBinding& range) {
            return guest_address < range.start;
        });
    if (it == registry->ranges.begin())
        return false;

    --it;
    if (address < it->start || address >= it->end)
        return false;

    if (out_binding != NULL)
    {
        out_binding->module_id = it->module_id;
        out_binding->generation = it->generation;
        out_binding->section_index = it->section_index;
        out_binding->section_base = it->section_base;
        out_binding->section_offset = address - it->section_base;
        if (!add_delta_u32(address, it->section_delta, &out_binding->canonical_pc))
            return false;
        out_binding->section_delta = it->section_delta;
    }
    return true;
}

bool moderngekko_rel_binding_registry_lookup_chunk(
    const ModernGekkoRelBindingRegistry* registry,
    uint32_t address,
    ModernGekkoRelChunkLookup* out_chunk)
{
    ModernGekkoRelBindingLookup binding = {};
    uint32_t chunk_index;

    if (out_chunk == NULL ||
        !moderngekko_rel_binding_registry_lookup(registry, address, &binding))
    {
        return false;
    }

    {
        std::vector<ModernGekkoRelBindingRegistry::RangeBinding>::const_iterator range_it =
            std::find_if(
                registry->ranges.begin(), registry->ranges.end(),
                [address](const ModernGekkoRelBindingRegistry::RangeBinding& range) {
                    return address >= range.start && address < range.end;
                });
        const ModernGekkoRelExecutableSection* section;

        if (range_it == registry->ranges.end())
            return false;
        section = range_it->section;
        for (chunk_index = 0; chunk_index < section->num_chunk_ranges; ++chunk_index)
        {
            const ModernGekkoRange chunk = section->chunk_ranges[chunk_index];
            if (binding.canonical_pc >= chunk.start && binding.canonical_pc < chunk.end)
            {
                out_chunk->module_id = binding.module_id;
                out_chunk->generation = binding.generation;
                out_chunk->section_index = binding.section_index;
                out_chunk->section_base = binding.section_base;
                out_chunk->section_offset = binding.section_offset;
                out_chunk->canonical_pc = binding.canonical_pc;
                out_chunk->section_delta = binding.section_delta;
                out_chunk->chunk_index = chunk_index;
                out_chunk->canonical_chunk_range = chunk;
                out_chunk->chunk_function = section->chunk_functions[chunk_index];
                out_chunk->chunk_hash = section->chunk_hashes == NULL
                                            ? NULL
                                            : &section->chunk_hashes[chunk_index];
                out_chunk->chunk_hash_mask = section->chunk_hash_masks == NULL
                                                 ? NULL
                                                 : &section->chunk_hash_masks[chunk_index];
                return true;
            }
        }
    }

    return false;
}

bool moderngekko_rel_binding_registry_lookup_section_base(
    const ModernGekkoRelBindingRegistry* registry,
    uint32_t module_id,
    uint32_t generation,
    uint32_t section_index,
    uint32_t* out_section_base)
{
    std::vector<ModernGekkoRelBindingRegistry::ModuleBinding>::const_iterator module_it;

    if (registry == NULL || out_section_base == NULL || module_id == 0u || generation == 0u)
        return false;

    module_it = std::find_if(
        registry->modules.begin(), registry->modules.end(),
        [module_id, generation](const ModernGekkoRelBindingRegistry::ModuleBinding& module) {
            return module.module_id == module_id && module.generation == generation;
        });
    if (module_it == registry->modules.end() ||
        section_index >= module_it->section_bases.size() ||
        module_it->section_bases[section_index] == 0u)
    {
        return false;
    }

    *out_section_base = module_it->section_bases[section_index];
    return true;
}

#ifndef MODERNGEKKO_MODULE_H
#define MODERNGEKKO_MODULE_H

#include "moderngekko/module_abi.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef enum ModernGekkoModuleStatus
{
    MODERNGEKKO_MODULE_OK = 0,
    MODERNGEKKO_MODULE_NULL_DESCRIPTOR,
    MODERNGEKKO_MODULE_ABI_MISMATCH,
    MODERNGEKKO_MODULE_CPU_ABI_MISMATCH,
    MODERNGEKKO_MODULE_CPU_STATE_SIZE_MISMATCH,
    MODERNGEKKO_MODULE_INVALID_GAME_ID,
    MODERNGEKKO_MODULE_GAME_ID_MISMATCH,
    MODERNGEKKO_MODULE_MISSING_DISPATCH,
    MODERNGEKKO_MODULE_INVALID_CODE_RANGES,
    MODERNGEKKO_MODULE_INVALID_SMC_RANGES,
    MODERNGEKKO_MODULE_INVALID_CHUNKS,
    MODERNGEKKO_MODULE_INVALID_REL_CATALOG,
    MODERNGEKKO_MODULE_ENTRY_POINT_UNCOVERED
} ModernGekkoModuleStatus;

typedef struct ModernGekkoModuleRequirements
{
    uint32_t cpu_abi_version;
    uint32_t cpu_state_size;
    const char* game_id;
} ModernGekkoModuleRequirements;

typedef struct ModernGekkoRelBindingRegistry ModernGekkoRelBindingRegistry;

typedef struct ModernGekkoRelBindingDesc
{
    const ModernGekkoRelModuleDesc* module;
    uint32_t generation;
    const uint32_t* section_bases;
    uint32_t num_section_bases;
} ModernGekkoRelBindingDesc;

typedef struct ModernGekkoRelBindingLookup
{
    uint32_t module_id;
    uint32_t generation;
    uint32_t section_index;
    uint32_t section_base;
    uint32_t section_offset;
    uint32_t canonical_pc;
    intptr_t section_delta;
} ModernGekkoRelBindingLookup;

typedef struct ModernGekkoRelChunkLookup
{
    uint32_t module_id;
    uint32_t generation;
    uint32_t section_index;
    uint32_t section_base;
    uint32_t section_offset;
    uint32_t canonical_pc;
    intptr_t section_delta;
    uint32_t chunk_index;
    ModernGekkoRange canonical_chunk_range;
    ModernGekkoRelChunkFn chunk_function;
    const uint64_t* chunk_hash;
    const uint64_t* chunk_hash_mask;
} ModernGekkoRelChunkLookup;

ModernGekkoModuleStatus moderngekko_validate_module(
    const ModernGekkoModuleDesc* descriptor,
    const ModernGekkoModuleRequirements* requirements);

const char* moderngekko_module_status_string(ModernGekkoModuleStatus status);

ModernGekkoRelBindingRegistry* moderngekko_rel_binding_registry_create(void);
void moderngekko_rel_binding_registry_destroy(ModernGekkoRelBindingRegistry* registry);
void moderngekko_rel_binding_registry_clear(ModernGekkoRelBindingRegistry* registry);
bool moderngekko_rel_binding_registry_link(
    ModernGekkoRelBindingRegistry* registry,
    const ModernGekkoRelBindingDesc* binding);
bool moderngekko_rel_binding_registry_unlink(ModernGekkoRelBindingRegistry* registry,
                                             uint32_t module_id, uint32_t generation);
bool moderngekko_rel_binding_registry_lookup(
    const ModernGekkoRelBindingRegistry* registry,
    uint32_t address,
    ModernGekkoRelBindingLookup* out_binding);
bool moderngekko_rel_binding_registry_lookup_chunk(
    const ModernGekkoRelBindingRegistry* registry,
    uint32_t address,
    ModernGekkoRelChunkLookup* out_chunk);
bool moderngekko_rel_binding_registry_lookup_section_base(
    const ModernGekkoRelBindingRegistry* registry,
    uint32_t module_id,
    uint32_t generation,
    uint32_t section_index,
    uint32_t* out_section_base);

#ifdef __cplusplus
}
#endif

#endif

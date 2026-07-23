#ifndef MODERNGEKKO_MODULE_ABI_H
#define MODERNGEKKO_MODULE_ABI_H

#include "moderngekko/cpu_state.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODERNGEKKO_MODULE_ABI_VERSION 4u
#define MODERNGEKKO_MODULE_ABI_VERSION_V3 3u
#define MODERNGEKKO_MODULE_ABI_VERSION_V4 MODERNGEKKO_MODULE_ABI_VERSION
#define MODERNGEKKO_GET_MODULE_SYMBOL "staticrecomp_get_module"

#if defined(_WIN32)
#define MODERNGEKKO_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define MODERNGEKKO_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define MODERNGEKKO_MODULE_EXPORT
#endif

typedef struct ModernGekkoRange
{
    uint32_t start;
    uint32_t end;
} ModernGekkoRange;

typedef void (*ModernGekkoRelChunkFn)(CPUState* state, uint32_t canonical_pc,
                                      intptr_t section_delta);

typedef struct ModernGekkoRelExecutableSection
{
    uint32_t section_index;
    uint32_t offset;
    uint32_t size;
    uint32_t canonical_start;
    const ModernGekkoRange* chunk_ranges;
    uint32_t num_chunk_ranges;
    const uint64_t* chunk_hashes;
    const uint64_t* chunk_hash_masks;
    const ModernGekkoRelChunkFn* chunk_functions;
} ModernGekkoRelExecutableSection;

typedef struct ModernGekkoRelModuleDesc
{
    uint32_t module_id;
    const ModernGekkoRelExecutableSection* executable_sections;
    uint32_t num_executable_sections;
} ModernGekkoRelModuleDesc;

typedef void (*ModernGekkoChunkFn)(CPUState* state);

typedef struct ModernGekkoModuleDesc
{
    uint32_t abi_version;
    uint32_t cpu_abi_version;
    uint32_t cpu_state_size;
    char game_id[8];
    uint32_t entry_point;

    int (*dispatch)(CPUState* state, uint32_t address);
    void (*on_state_loaded)(CPUState* state);

    const ModernGekkoRange* code_ranges;
    uint32_t num_code_ranges;
    const ModernGekkoRange* smc_ranges;
    uint32_t num_smc_ranges;
    const ModernGekkoRange* chunk_ranges;
    uint32_t num_chunk_ranges;
    const uint64_t* chunk_hashes;
    const ModernGekkoChunkFn* chunk_functions;

    const ModernGekkoRelModuleDesc* rel_modules;
    uint32_t num_rel_modules;
} ModernGekkoModuleDesc;

typedef const ModernGekkoModuleDesc* (*ModernGekkoGetModuleFn)(void);

typedef struct ModernGekkoHostEvent
{
    uint32_t id;
    uint32_t reserved;
    uint64_t guest_timebase;
    uint64_t core_ticks;
} ModernGekkoHostEvent;

typedef bool (*ModernGekkoTakeHostEventFn)(ModernGekkoHostEvent* event);

typedef ModernGekkoRange StaticRecompRange;
typedef ModernGekkoRelChunkFn StaticRecompRelChunkFn;
typedef ModernGekkoRelExecutableSection StaticRecompRelExecutableSection;
typedef ModernGekkoRelModuleDesc StaticRecompRelModuleDesc;
typedef ModernGekkoChunkFn StaticRecompChunkFn;
typedef ModernGekkoModuleDesc StaticRecompModuleDesc;
typedef ModernGekkoGetModuleFn StaticRecompGetModuleFn;
typedef ModernGekkoHostEvent StaticRecompHostEvent;
typedef ModernGekkoTakeHostEventFn StaticRecompTakeHostEventFn;

#define STATICRECOMP_ABI_VERSION MODERNGEKKO_MODULE_ABI_VERSION
#define STATICRECOMP_GET_MODULE_SYMBOL MODERNGEKKO_GET_MODULE_SYMBOL
#define MODERNGEKKO_TAKE_HOST_EVENT_SYMBOL "staticrecomp_take_host_event"
#define STATICRECOMP_TAKE_HOST_EVENT_SYMBOL MODERNGEKKO_TAKE_HOST_EVENT_SYMBOL

void moderngekko_module_signal_host_event(CPUState* state, uint32_t event_id);

#ifdef __cplusplus
}
#endif

#endif

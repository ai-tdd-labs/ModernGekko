#include "moderngekko/module.h"

#include <stddef.h>
#include <string.h>

#define COMPILE_ASSERT(name, expression) typedef char name[(expression) ? 1 : -1]

static int dispatch(CPUState* state, uint32_t address)
{
    state->gpr[0] = address;
    return 1;
}

static void chunk(CPUState* state)
{
    state->gpr[0] = state->pc;
}

static void rel_chunk(CPUState* state, uint32_t canonical_pc, intptr_t section_delta)
{
    state->gpr[0] = canonical_pc;
    state->gpr[1] = (uint32_t)section_delta;
}

static void on_state_loaded(CPUState* state)
{
    state->gpr[0] = 0;
}

int main(void)
{
    static const ModernGekkoRange code_ranges[] = {{0x80003100u, 0x80003200u}};
    static const uint64_t chunk_hashes[] = {0xCBF29CE484222325ull};
    static const ModernGekkoChunkFn chunk_functions[] = {chunk};
    static const ModernGekkoRange rel_chunk_ranges[] = {
        {0x90001000u, 0x90001010u},
        {0x90001010u, 0x90001020u},
        {0x90002000u, 0x90002010u},
    };
    static const ModernGekkoRelChunkFn rel_chunk_functions[] = {
        rel_chunk,
        rel_chunk,
        rel_chunk,
    };
    static const ModernGekkoRelExecutableSection rel_sections[] = {
        {1u, 0x0u, 0x20u, 0x90001000u, rel_chunk_ranges, 2u, NULL, NULL,
         rel_chunk_functions},
        {3u, 0x10u, 0x10u, 0x90002000u, rel_chunk_ranges + 2u, 1u, NULL, NULL,
         rel_chunk_functions + 2u},
    };
    static const ModernGekkoRelModuleDesc rel_modules[] = {
        {17u, rel_sections, 2u},
    };
    StaticRecompModuleDesc descriptor = {
        MODERNGEKKO_MODULE_ABI_VERSION,
        MODERNGEKKO_CPU_ABI_VERSION,
        (uint32_t)sizeof(CPUState),
        "TEST01",
        0x80003100u,
        dispatch,
        on_state_loaded,
        code_ranges,
        1u,
        NULL,
        0u,
        code_ranges,
        1u,
        chunk_hashes,
        chunk_functions,
        NULL,
        0u,
    };
    const ModernGekkoModuleRequirements requirements = {
        MODERNGEKKO_CPU_ABI_VERSION,
        (uint32_t)sizeof(CPUState),
        "TEST01",
    };
    CPUState state = {0};

    COMPILE_ASSERT(module_abi_version_is_four,
                   MODERNGEKKO_MODULE_ABI_VERSION == 4u);
    COMPILE_ASSERT(module_abi_version_v3_is_preserved,
                   MODERNGEKKO_MODULE_ABI_VERSION_V3 == 3u);
    COMPILE_ASSERT(game_id_storage_is_eight_bytes,
                   sizeof(descriptor.game_id) == 8u);
    COMPILE_ASSERT(dispatch_follows_entry_point,
                   offsetof(ModernGekkoModuleDesc, dispatch) >
                       offsetof(ModernGekkoModuleDesc, entry_point));
    COMPILE_ASSERT(rel_catalog_follows_chunk_functions,
                   offsetof(ModernGekkoModuleDesc, rel_modules) >
                       offsetof(ModernGekkoModuleDesc, chunk_functions));
    COMPILE_ASSERT(rel_chunk_function_has_signed_delta,
                   sizeof(intptr_t) >= sizeof(int32_t));

    if (descriptor.abi_version != STATICRECOMP_ABI_VERSION)
        return 1;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_OK)
        return 2;
    if (!descriptor.dispatch(&state, descriptor.entry_point))
        return 3;
    if (state.gpr[0] != descriptor.entry_point)
        return 4;
    descriptor.on_state_loaded(&state);
    if (state.gpr[0] != 0u)
        return 5;

    descriptor.abi_version = MODERNGEKKO_MODULE_ABI_VERSION_V3;
    descriptor.rel_modules = NULL;
    descriptor.num_rel_modules = 1u;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_OK)
    {
        return 6;
    }
    descriptor.abi_version = MODERNGEKKO_MODULE_ABI_VERSION + 1u;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_ABI_MISMATCH)
    {
        return 7;
    }
    descriptor.abi_version = MODERNGEKKO_MODULE_ABI_VERSION;
    descriptor.rel_modules = NULL;
    descriptor.num_rel_modules = 0u;

    descriptor.cpu_abi_version++;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_CPU_ABI_MISMATCH)
        return 8;
    descriptor.cpu_abi_version--;

    descriptor.cpu_state_size++;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_CPU_STATE_SIZE_MISMATCH)
        return 9;
    descriptor.cpu_state_size--;

    descriptor.entry_point = 0x80004000u;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_ENTRY_POINT_UNCOVERED)
        return 10;
    descriptor.entry_point = 0x80003100u;

    descriptor.chunk_ranges = NULL;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_INVALID_CHUNKS)
    {
        return 11;
    }
    descriptor.chunk_ranges = code_ranges;

    descriptor.chunk_functions = NULL;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_INVALID_CHUNKS)
    {
        return 12;
    }

    {
        static const ModernGekkoChunkFn null_chunk_functions[] = {NULL};
        descriptor.chunk_functions = null_chunk_functions;
        if (moderngekko_validate_module(&descriptor, &requirements) !=
            MODERNGEKKO_MODULE_INVALID_CHUNKS)
        {
            return 13;
        }
    }
    descriptor.chunk_functions = chunk_functions;

    descriptor.rel_modules = rel_modules;
    descriptor.num_rel_modules = 1u;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_OK)
    {
        return 14;
    }

    descriptor.rel_modules = NULL;
    if (moderngekko_validate_module(&descriptor, &requirements) !=
        MODERNGEKKO_MODULE_INVALID_REL_CATALOG)
    {
        return 15;
    }
    descriptor.rel_modules = rel_modules;

    {
        static const ModernGekkoRelExecutableSection invalid_rel_sections[] = {
            {1u, 0x0u, 0x20u, 0x90001000u, rel_chunk_ranges, 1u, NULL, NULL,
             rel_chunk_functions},
        };
        static const ModernGekkoRelModuleDesc invalid_rel_modules[] = {
            {17u, invalid_rel_sections, 1u},
        };
        descriptor.rel_modules = invalid_rel_modules;
        if (moderngekko_validate_module(&descriptor, &requirements) !=
            MODERNGEKKO_MODULE_INVALID_REL_CATALOG)
        {
            return 16;
        }
    }

    {
        static const ModernGekkoRange invalid_rel_chunk_ranges[] = {
            {0x90001008u, 0x90001020u},
        };
        static const ModernGekkoRelExecutableSection invalid_rel_sections[] = {
            {1u, 0x0u, 0x20u, 0x90001000u, invalid_rel_chunk_ranges, 1u, NULL,
             NULL, rel_chunk_functions},
        };
        static const ModernGekkoRelModuleDesc invalid_rel_modules[] = {
            {17u, invalid_rel_sections, 1u},
        };
        descriptor.rel_modules = invalid_rel_modules;
        if (moderngekko_validate_module(&descriptor, &requirements) !=
            MODERNGEKKO_MODULE_INVALID_REL_CATALOG)
        {
            return 17;
        }
    }

    return 0;
}

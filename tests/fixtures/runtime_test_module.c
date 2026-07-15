#include "moderngekko/module_abi.h"

static void chunk(CPUState* state)
{
    const uint32_t address = state->pc;
    if (address == 0x80003100u)
    {
        state->gpr[3] = 0x13579BDFu;
        state->ram[0x100] = 0x12u;
        state->ram[0x101] = 0x34u;
        state->ram[0x102] = 0x56u;
        state->ram[0x103] = 0x78u;
        state->pc = 0x80004000u;
        state->downcount -= 3;
        return;
    }

    if (address == 0x80003120u)
    {
        state->gpr[4] = ((uint32_t)state->ram[0x100] << 24) |
                        ((uint32_t)state->ram[0x101] << 16) |
                        ((uint32_t)state->ram[0x102] << 8) |
                        state->ram[0x103];
        state->pc = 0u;
        state->downcount -= 2;
        return;
    }
}

static int dispatch(CPUState* state, uint32_t address)
{
    if (address < 0x80003100u || address >= 0x80003140u)
        return 0;
    state->pc = address;
    chunk(state);
    return 1;
}

static const ModernGekkoRange code_ranges[] = {
    {0x80003100u, 0x80003140u},
};

static const uint64_t chunk_hashes[] = {
    0xCBF29CE484222325ull,
};

static const ModernGekkoChunkFn chunk_functions[] = {chunk};

static const ModernGekkoModuleDesc descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    (uint32_t)sizeof(CPUState),
    "TEST01",
    0x80003100u,
    dispatch,
    0,
    code_ranges,
    1u,
    0,
    0u,
    code_ranges,
    1u,
    chunk_hashes,
    chunk_functions,
};

MODERNGEKKO_MODULE_EXPORT const ModernGekkoModuleDesc* staticrecomp_get_module(void)
{
    return &descriptor;
}

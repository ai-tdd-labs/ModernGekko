# known_issues

## DolRecomp floating-point parity is not yet green

The standalone opcode oracle now builds a synthetic DOL through DolRecomp and
replays each native dispatch through Dolphin's interpreter from the same state.
The first full smoke run covers 142 blocks. Integer, branch, and memory blocks
run cleanly; the remaining divergences are in FPSCR/FPRF and paired-single edge
semantics (plus `subfme` XER.SO). They are useful, reproducible CPU defects,
not skipped fallbacks or unresolved guest-memory accesses.

Acceptance for the next emitter-hardening pass: the deterministic command
reports zero divergent blocks, zero instruction fallbacks, and a non-zero
dispatch count; MKDD remains strict-native afterward.

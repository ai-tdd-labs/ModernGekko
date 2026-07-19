# Run 20260719_141731

- Goal: Port the GekkoRecomp opcode bank into a DolRecomp-to-Dolphin differential gate.
- Command(s): repository/branch inventory and source inspection.
- Artifacts: `codex/known_issues.md`, `codex/decision_log.md`.
- Result: complete. The command now creates a DOL-only corpus, recompiles it
  with DolRecomp, and executes it through RecompCore's Dolphin interpreter
  lockstep. The 142-block/one-seed smoke run has 102 matching blocks and 40
  reproducible CPU divergences; it has no fallback skips, zero-cycle skips, or
  unresolved memory accesses.
- Next action: merge the Windows fuzzer-hardened FPSCR/paired-single emitter
  fixes, then use this gate as the cross-platform regression check.

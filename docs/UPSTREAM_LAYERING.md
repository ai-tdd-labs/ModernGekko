# Upstream layering

This tree treats the public ExpansionPak repositories as the product base. Our
work is an integration layer, not a replacement fork with an unrelated history.

## Layer order

1. **ExpansionPak/DolRecomp** provides the public static PowerPC recompiler.
2. **ai-tdd-labs/DolRecomp** adds small, isolated code-generation hooks and
   regressions needed by strict-native and parity work.
3. **ExpansionPak/RecompCore `moderngekko-runtime`** provides Dolphin's tested
   hardware, SDK, frontend, controller, audio, video, and netplay foundation.
4. **ai-tdd-labs/RecompCore** adds strict-native dispatch, CPU lockstep,
   low-memory boot behavior, trace/debug plumbing, and the pinned DolRecomp
   layer.
5. **ExpansionPak/ModernGekko** provides the public port and runtime workflow.
6. **ai-tdd-labs/ModernGekko** adds deterministic DTM playback, strict fallback
   policy, screenshots, symbol-aware tracing, performance instrumentation,
   sparse per-game patch support, reproducible workspaces, widescreen/FPS
   controls, and macOS compatibility.
7. Game repositories contain only game-specific symbols, patches, replay
   fixtures, profiles, and acceptance tests.

This direction matters: upstream controller, netplay, platform, and Dolphin
fixes should continue to arrive through normal merges. Generic local fixes go
in the lowest reusable layer; game-specific fixes do not belong here.

## Updating upstream

1. Fetch all three public upstream repositories.
2. Update and test DolRecomp first.
3. Update RecompCore, pin the tested DolRecomp revision, and run CPU/lockstep
   tests.
4. Merge public ModernGekko, pin the tested RecompCore revision, and run its
   frontend, netplay, build, replay, and strict-native tests.
5. Run at least one real game smoke test with fallback disabled. A rendered
   frame alone is not proof of native execution.
6. Update `PROVENANCE.md` whenever either submodule pin changes.

Do not copy upstream source into a second directory or silently reimplement an
upstream feature. Keep conflicts visible and resolve them as a union of the
public feature and the local instrumentation or policy.

## Required gates

- DolRecomp code-generation tests compile generated output.
- RecompCore strict-native dispatch continues after temporary non-native code
  and reports no permanent JIT takeover.
- ModernGekko frontend configuration and netplay protocol tests pass.
- Port output is reproducible for the same input DOL, symbols, patches, and
  tool revisions.
- A deterministic game replay completes with fallback disabled and records
  native dispatch, fallback, frame-time, and screenshot evidence.

## Known migration debt

The live RecompCore instruction lockstep remains active. The older generated
CPU fuzz/oracle bank from the pre-submodule DolRecomp integration is preserved
on the previous integration branch, but has not yet been converted into the
standalone DolRecomp test format. Do not delete that branch or claim equivalent
random-state coverage until the bank has been migrated and passes against both
DOL and REL emitters.

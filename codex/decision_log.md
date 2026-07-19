# decision_log

## 2026-07-19 — Keep the fuzzer independent of DolRecomp's own CPU helpers

The synthetic opcode generator belongs to DolRecomp, the randomized execution
and comparison driver belongs to RecompCore, and ModernGekko only orchestrates
the end-to-end gate. The oracle side is Dolphin's interpreter, not a duplicate
of DolRecomp's emitter formulas.

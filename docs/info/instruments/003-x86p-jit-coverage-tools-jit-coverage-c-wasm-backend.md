---
id: I003
kind: instrument
status: TRUSTED
created: 2026-09-19
validated_on: 2026-09-19
---

## Instrument

x86p_jit_coverage (tools/jit_coverage.c, wasm backend)

## Validated by

Showing the other answer on both halves it reports.

The condition census: over a corpus of 16,451 functions from a real title it
reports 53,601 of 55,305 conditions lowered inline. The same tool over the same
corpus, built against the x64 backend, reports **0 of 55,299** — that backend
inlines no condition, and says so in `jit_x64_cond.c`. One instrument, two
corpora-identical runs, opposite answers.

The exit census: over the same corpus it reports 167,287 exits from 112,889
blocks, 70.3% of them naming an address the translator already holds and 21.6%
of them loop backedges. The x64 build over the same bytes **REFUSES** it by
name, because that backend does not fill the counters and a row of zeros would
read like a binary without branches in it. One instrument, two corpora-identical
runs, an answer and a refusal.

The counters behind it are asserted in `tests/test_wasm_exits.c`, 28 checks over
eight shapes, each asserting the zeroes as well as the counts.

## Known failure modes

**It did not work at all under wasm until 2026-09-19, and it did not say so.**
Two defects, both of the shape this registry exists to catch:

- `char line[MAX_FN_BYTES * 2 + 64]` was automatic. At 128 KiB against a wasm
  build's 64 KiB default stack it overflowed before the first line was read, and
  surfaced as `RuntimeError: memory access out of bounds` inside the module with
  no line of C to blame. It is `static` now.
- The differential entered `blk.entry` without checking it. A WebAssembly
  backend hands back a module and leaves `entry` NULL by design, so the tool
  reported a DIVERGENCE for every block it reached and then faulted. It now
  counts those blocks as unenterable and REFUSES the differential by name;
  correctness for that backend is proven by the differential suites, not here.

**The exit census answered the wrong question until 2026-09-19, and its answer
looked plausible.** It walked each block's bytes and classified the LAST
instruction. A conditional branch does not end a block in this backend, so every
loop backedge was invisible: it reported ONE branch back to a block's own entry
in 113,272 blocks, over code containing a skinning loop measured (issue #165 in
the consuming title) to pay a dispatch per bone. The tell was that a near-zero
was impossible, not that anything failed. It now sums counters the lowering
fills as it emits each exit, and 54,015 exits that walk never saw came back.

**A tier of that census is an artefact of this tool, and is labelled.** Where a
block starts is decided by the walk, not by the guest: this tool splits each
function linearly from its first byte, while the running engine translates from
the address it dispatched to. So the same guest loop is a `to_entry` in the
product and a mid-block backedge here. `backward` is the only tier invariant
under that, and it is the one to quote.

**Still true, by design:** every block counts once. A hot loop and a function
that never runs weigh the same, so all three censuses are static ratios. The
dynamic ratio needs the running product's block histogram.

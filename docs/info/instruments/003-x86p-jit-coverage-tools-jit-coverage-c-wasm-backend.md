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

The exit census: 53.9% relative branch, 16.4% RET, 27.5% indirect. A classifier
that could not tell them apart would put everything in one row; these are three
rows with three different populations, and the x64 backend's shorter blocks move
the ratio to 69.3/11.2/18.7 on the same bytes, which is the response to a real
difference and not a constant.

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

**Still true, by design:** every block counts once. A hot loop and a function
that never runs weigh the same, so both censuses are static ratios. The dynamic
ratio needs the running product's block histogram.

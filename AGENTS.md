# x86port — agent guidance

The x86-32 platform framework: guest execution, the Win32 HLE, and the guest
graphics frontends, for `pc/xmen2`, `pc/lf2`, and the original Xbox path later.
It replaces `shared/recomp-x86`.

**Read `shared/jit-common/docs/migration.md` first** — it is the architecture
this repo sits inside, and `shared/jit-common/docs/project-state.md` (rows
S040–S047) is the tracker. `docs/` here holds only what is specific to x86.

## The thing this repo exists to stop doing

`shared/recomp-x86` translates the guest binary to C at build time. On
`pc/xmen2` that is 116,500 functions, 89 translation units, 307 MB of generated
C regenerated every build — and 8,234 instructions it could not translate, each
a loud abort if reached. See jit-common I004 for the full measurement.

Two facts from that measurement shape this repo:

- **`shared/recomp-x86` has no decoder.** Its front end is Ghidra: it consumes
  a JSON of disassembled *text* and lifts mnemonic strings, which is why its
  failures read `mnemonic PFMUL`. Its per-instruction semantics are worth
  seeding from; its decode does not exist to seed from. Ghidra is a
  maintainer-only tool and can never be a player prerequisite, so this framework
  needs a decoder of its own. **Settled 2026-09-01: Zydis, pinned at v4.1.1 in
  `vendor/zydis`** — see "Why Zydis, and how we know" below.
- **90% of those 8,234 holes are 3DNow!, and it is twenty opcodes.** That is why
  `src/x86port/three_dnow.c` is the first thing here: it is the largest single
  piece of coverage, and the only substantial one that does not depend on the
  decoder decision.

## Why Zydis, and how we know

Decode is mechanical, exhaustively specified, and has no opinion about memory
models, threading, or code caches — so embedding a proven implementation costs
nothing architecturally, unlike embedding a whole CPU core, which brings all
three. Zydis is MIT, allocation-free, and ships pre-generated tables, so it adds
no build-time code generation (G001). Semantics stay ours: S043's whole point is
that we are the authority on what correct means.

**The choice is measured, not argued.** `pc/xmen2`'s Ghidra export carries the
raw bytes of every instruction beside Ghidra's own reading of them, which is an
offline second opinion over the whole shipped corpus:

```sh
python3 tools/corpus_extract.py ~/repo/pc/xmen2/scratch/recomp | ./build/x86p_decode_diff
```

Result, 2026-09-01, over **2,168,629 instructions from 20 modules**:

- **0 failed to decode.**
- **37 length disagreements (0.0017%), and all 37 are the same convention**: the
  `0x9B` prefix, where Ghidra folds `FWAIT` into the following x87 instruction
  (`FSTSW` 24, `FSTCW` 11, `FSAVE` 2) and Zydis reports it as the separate
  instruction it architecturally is. Neither is wrong, and the literal reading
  is the one an interpreter wants, because the CPU really does execute two
  instructions there. **It is not special-cased**: an instrument that silences a
  category cannot tell you when a real defect joins it.
- 20,790 mnemonic spellings differ (0.96%), all benign synonyms — `JGE`/`JNL`,
  `JA`/`JNBE`, Ghidra folding `REP` into the mnemonic. Two are worth knowing:
  Zydis 4.1 renders `PFRSQRT` as `PFSQRT` and `PFRCPIT1` as `PFCPIT1`, each a
  letter short. `x86p_3dnow_parse` accepts both spellings because of this run.

## Flags are verified against silicon, not against the manual

`src/x86port/flags.{h,c}` is the authority on what an operation does to EFLAGS,
and it is the module with the strongest verification here on purpose: almost
every instruction writes flags, almost none are read, and a wrong one surfaces
as a branch taken differently a thousand instructions later.

`tests/test_flags.c` EXECUTES each instruction on the host CPU with a controlled
incoming EFLAGS and compares the real word — exhaustively at byte width, and
over deterministic boundary-crossed sweeps at 16 and 32 bits. **2,187,776
comparisons, 0 mismatches**, all six flags. On a non-x86 host it says loudly
that no oracle ran rather than passing on the hermetic cases alone.

That oracle found four defects no amount of reading found, and the lesson
generalises to anything else added here:

- **Compare the ISA-UNDEFINED flags too.** "Undefined" describes the
  specification, not the silicon. This CPU *clears* AF on AND/OR/XOR/TEST and
  *sets* it on every shift; the model preserved it in both cases, and guest code
  that saves and restores EFLAGS can see the difference.
- **Vary every input the model reads, or the sweep has no power.** The first
  version varied only the incoming CF and reported "0 differ" for the logic
  ops' AF — a clean bill of health it was in no position to give.
- **A rule that belongs to the instruction does not become a case in the
  derivation.** A shift by zero writes no flags, so the caller records nothing
  and `x86p_flags_set` refuses one; there is no preserving branch for four
  flags that have no correct value.

## Boundaries

- **Semantics are ours; decode may be borrowed.** The whole point of S043 is
  that we write the translator, so nothing else can say what correct means —
  the interpreter is the authority an emitter is checked against. A decoder is a
  different kind of problem: mechanical, well-specified, with no memory-model or
  threading opinions to fight, so embedding a proven one is legitimate where
  embedding a whole CPU core would not be.
- **No title knowledge.** X-Men Legends II's Alchemy engine, its save format,
  and its D3D8 usage belong in `pc/xmen2`. A D3D8 *implementation* belongs here;
  a fast path that makes one screen appear does not.
- **No build-time code generation, ever.** That is G001. If a change here needs
  a generator, it is the wrong change.

## Conventions

- C11, Clang for agent verification builds, tracked `.clang-format`.
- `-Wall -Wextra -Werror` on every target.
- **A refused instruction must be nameable.** "PFRCP, unimplemented" and
  "unknown instruction" are different facts about a run, and an engine that
  cannot tell them apart cannot be debugged. Every enum here ends in a `kCount`
  that is the denominator an exhaustive check counts against, and every "did it
  run?" returns a value the caller must consult.
- **Approximate is not a synonym for implemented.** `PFRCP` is not `1.0f/x`. An
  operation whose exact result this framework cannot state from a specification
  is refused by name, not guessed — a fast reciprocal that is subtly wrong
  surfaces as drift a thousand frames later, and the test that would catch it is
  the one nobody writes.
- Subagent allowance: **0** until the user assigns a count.

## Verification

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang && cmake --build build -j
ctest --test-dir build --output-on-failure
```

There is no `run.sh`: this is a library, not a product. The launcher contract
belongs to the consuming title.

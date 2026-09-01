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
  needs a decoder of its own. **That decision is still open (I004).**
- **90% of those 8,234 holes are 3DNow!, and it is twenty opcodes.** That is why
  `src/x86port/three_dnow.c` is the first thing here: it is the largest single
  piece of coverage, and the only substantial one that does not depend on the
  decoder decision.

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

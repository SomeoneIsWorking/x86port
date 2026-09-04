# Project goals

This document owns `x86port`'s durable outcomes. Current delivery state lives
in `project-state.md`, subsystem placement in `codemap.md`, and implementation
order and gates in `migration.md`.

## G001 — Gameplay products execute x86-32 through a native/dynarec hybrid

`x86port` enables games to keep verified native overrides while translating
every remaining guest instruction on demand from the user's original binary.

Why it matters: a runtime-owned execution path avoids enormous generated guest
corpora, preserves the binary as ground truth, and lets instruction fixes apply
to every consumer.

Success conditions:

- The consumer-facing library and gameplay executable default to the
  JIT/dynarec path with no explicit interpreter selector. A bounded fallback
  may execute only blocks whose compilation was refused or whose emitted code
  is unsafe, and reports its reason and execution counts.
- Native overrides are selected by complete runtime image identity and guest
  address, and can call the original guest body through the JIT without
  recursion.
- Runtime changes to executable memory or override decisions invalidate every
  affected translated path.
- No build, install, provisioning, or release step emits guest source, guest
  objects, generated dispatch tables, or a precompiled title substrate.
- Product conformance is demonstrated in representative interactive gameplay,
  not inferred from boot, a menu, or final CPU state alone.

Constraints and non-goals:

- Interpreter-only execution is permitted only as a separately built diagnostic
  oracle. A bounded product fallback is not an alternate engine or profiling
  pass and cannot establish gameplay/performance conformance.
- `x86port` contains no title-specific addresses, Win32 policy, or presentation
  behavior.
- Runtime native overrides are intentional product ownership, not an escape
  hatch for missing instruction semantics.

Contributing state items: S001–S009, S013–S015.

## G002 — Every declared host has a real product JIT backend

The same x86-32 product contract works on each released host architecture
with a real dynarec backend; bounded instruction fallback cannot substitute for
a missing backend.

Why it matters: host support is an executable-code-generation capability, not a
build-system label.

Success conditions:

- x64 and ARM64 backends translate all instruction paths required by each
  declared consumer.
- Executable-memory publication, instruction-cache coherence, ABI transitions,
  exceptions, interrupts, invalidation, and bounded exits are verified on each
  host class.
- An unavailable backend is reported by name and the product refuses to start;
  it never degrades to interpreter-only execution.
- Runtime-populated caches are disposable, identity-bound user data and are not
  required by a fresh install.

Contributing state items: S004, S005, S007, S008, S014, S015.

## G003 — The CPU model is faithful and independently testable

Decoding, CPU state, memory access, flags, integer/SIMD/x87 semantics,
exceptions, and timing-visible exits have one authoritative implementation and
can be checked against independent evidence.

Why it matters: a fast JIT that drifts silently from the guest machine is not a
porting foundation.

Success conditions:

- Decode and instruction semantics refuse unsupported behavior by name and
  report the number of cases examined.
- The separately linked interpreter oracle and hardware/emulator comparisons
  exercise the shipping decoder, state, memory, and semantic helpers rather
  than test-only copies.
- JIT differentials stop at the first whole-machine divergence, report fallback
  blocks/instructions by reason, and prove that the interval used for JIT or
  performance claims executed translated blocks.
- Configuration and mutable runtime state are instance-owned so multiple game
  instances cannot corrupt each other.

Contributing state items: S001–S004, S008, S009, S013.

## G004 — x86port remains a cohesive, self-contained platform framework

The repository owns its structure, configuration, diagnostics, and x86-only
runtime contracts without borrowing title architecture or inventing speculative
cross-platform abstractions.

Why it matters: consumers need one maintainable execution owner rather than a
new shared monolith or a set of title forks.

Success conditions:

- Modules have cohesive responsibilities, first-party source stays under the
  1,200-line structure limit, and the normal verifier enforces format, lint,
  structure, portability, and tests.
- Runtime configuration is typed and instance-owned; product behavior is not
  selected by environment variables or hidden process globals.
- Library diagnostics use one configurable sink and explicit status returns;
  tests and standalone tools own their own reporting boundaries.
- `jit-common` gains an abstraction only after two real platform frameworks
  demonstrate the same contract; x86-specific execution remains here.
- Consumers use the canonical repository directly rather than vendoring or
  copying its implementation.

Contributing state items: S006, S009–S012, S014, S015.

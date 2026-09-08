# Guest memory

`X86pMem` borrows memory. Zero-initialize it before setting fields: a null
`sparse` selects the existing contiguous `host`, `lo`, `size` mapping. A null
host base remains a valid identity mapping; configured size determines whether
it exists. Desktop JIT backends admit this contiguous mode only.

For wasm32, `memory_sparse.h` provides `X86pSparseMem`. Create one owner and put
it in `X86pMem.sparse`; the contiguous fields are then ignored. Register actual
allocations with `x86p_sparse_map(owner, guest, host, size)`. Exact byte spans
are used without page rounding; gaps are guest faults. Zero length, wrapping
ranges and guest overlaps refuse without changing existing mappings. The owner
borrows host allocations and neither unmap nor destroy frees them. Unmap takes
the exact registered guest address and size; partial unmapping refuses.

The scalar APIs and `x86p_mem_read_bytes`/`write_bytes` admit the complete access
before copying anything. Adjacent guest mappings can have unrelated host
backing; an unaligned scalar or multi-byte value crosses those mappings without
requiring one large host arena. A hole or guest-address wrap preserves the CPU
caller's output and all memory bytes. `x86p_mem_readable_span` reports the first
hole or the guest address-space end, never a successful empty scan.

Native consumers request pointers with `x86p_mem_resolve(memory, guest, size,
&host)`. The whole span must fit one registered allocation; an allocation
crossing refuses even if the guest ranges are adjacent. Use checked byte copies
when native contiguity is unavailable. `x86p_sparse_guest_address` provides the
reverse lookup for one borrowed native span, refusing ambiguous host aliases.
The string bulk-copy optimization refuses sparse memory because disjoint guest
ranges can alias the same host bytes; ordinary ordered string execution retains
the shared checked access contract.

Mapping owners and `X86pMem` stay alive throughout engine use. Mutations are
serialized against guest execution and native consumers. Invalidate affected
JIT guest ranges before mapping changes or native executable-byte writes. A
resolved native pointer bypasses write observers; it is not an invalidation
mechanism. Sparse WASM lowering bakes the `X86pMem` address into its plan and
calls checked memory imports, so mapping changes remain visible to later
entries. Native x64/ARM64 translation and engine creation refuse sparse mappings
explicitly rather than emitting contiguous host accesses against them.

`test_memory_sparse` compares scalar access admission, values and whole backing
bytes against contiguous memory across allocation boundaries. It also checks
holes, guest wrap, overlapping mappings, native span refusal, reverse alias
ambiguity, unmapping and observer-before-write ordering. `test_wasm_sparse`
executes generated modules through the shipping engine using scattered backing
at high guest addresses; it checks split-page stores/loads, typed faults with
unchanged state, cache reuse and executable replacement after invalidation.

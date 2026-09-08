#ifndef X86PORT_MEMORY_SPARSE_H
#define X86PORT_MEMORY_SPARSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact guest ranges backed by independently allocated host spans. The owner
 * borrows backing; destroy/unmap never free caller memory. Mutations are
 * serialized against all guest/native access. Invalidate translated guest
 * ranges before map/unmap and keep this owner alive as long as X86pMem uses it.
 * No page rounding: unregistered bytes remain holes, even inside a host page. */
typedef struct X86pSparseMem X86pSparseMem;

X86pSparseMem *x86p_sparse_create(void);
void x86p_sparse_destroy(X86pSparseMem *sparse);

/* Nonempty, nonwrapping, nonoverlapping guest spans only; host backing must
 * be nonnull and valid for size bytes. Failure leaves every mapping intact. */
int x86p_sparse_range_available(const X86pSparseMem *sparse, uint32_t guest, uint32_t size);
int x86p_sparse_map(X86pSparseMem *sparse, uint32_t guest, void *host, uint32_t size);
/* Access uses X86pMemAccess bits from cpu.h; zero reserves inaccessible backing.
 * The ordinary map API grants read and write. */
int x86p_sparse_map_access(X86pSparseMem *sparse, uint32_t guest, void *host, uint32_t size, unsigned access);
/* Atomic mutations, splitting existing ranges at the boundaries. Protect
 * requires every byte mapped; unmap ignores already-unmapped holes like munmap.
 * Wrap/allocation failure refuse unchanged. Protect preserves backing and
 * original-allocation provenance. */
int x86p_sparse_protect(X86pSparseMem *sparse, uint32_t guest, uint32_t size, unsigned access);
int x86p_sparse_unmap_range(X86pSparseMem *sparse, uint32_t guest, uint32_t size);
/* True while any mapped fragment still borrows bytes from this host allocation. */
int x86p_sparse_host_in_use(const X86pSparseMem *sparse, const void *host, size_t size);
uint32_t
x86p_sparse_span_access(const X86pSparseMem *sparse, uint32_t guest, uint32_t max, unsigned access, uint8_t **host);
int x86p_sparse_resolve(const X86pSparseMem *sparse, uint32_t guest, uint32_t size, unsigned access, uint8_t **host);

/* Remove exactly one registered mapping; partial removal refuses unchanged. */
int x86p_sparse_unmap(X86pSparseMem *sparse, uint32_t guest, uint32_t size);

/* Return the available bytes up to max in ONE registered backing span and
 * its host address; 0 on a hole (host untouched). Use x86p_mem_resolve for a
 * complete native span or x86p_mem_read/write_bytes to cross backing spans. */
uint32_t x86p_sparse_span(const X86pSparseMem *sparse, uint32_t guest, uint32_t max, uint8_t **host);

/* Resolve a borrowed native span back to its guest address. Reject ambiguous
 * host aliases and spans crossing separate registered allocations. */
int x86p_sparse_guest_address(const X86pSparseMem *sparse, const void *host, uint32_t size, uint32_t *guest);

#ifdef __cplusplus
}
#endif
#endif

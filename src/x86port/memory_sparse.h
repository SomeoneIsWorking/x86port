#ifndef X86PORT_MEMORY_SPARSE_H
#define X86PORT_MEMORY_SPARSE_H

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
int x86p_sparse_map(X86pSparseMem *sparse, uint32_t guest, void *host, uint32_t size);
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

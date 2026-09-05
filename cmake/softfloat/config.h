/* Minimal platform vocabulary for the unmodified Bochs SoftFloat sources. */
#ifndef X86P_SOFTFLOAT_CONFIG_H
#define X86P_SOFTFLOAT_CONFIG_H
#include <stdint.h>
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BX_LITTLE_ENDIAN 1
#endif
#define BX_CPP_INLINE inline
#define BX_CONST64(x) UINT64_C(x)
typedef int16_t Bit16s;
typedef uint16_t Bit16u;
typedef int32_t Bit32s;
typedef uint32_t Bit32u;
typedef int64_t Bit64s;
typedef uint64_t Bit64u;
#endif

#include "x87_memory.h"

static int width_supported(unsigned width, int integer) {
  return integer ? (width == 2 || width == 4 || width == 8) : (width == 4 || width == 8 || width == 10);
}

X86pX87MemoryStatus
x86p_x87_read_value(const X86pMem *mem, uint32_t address, unsigned width, int integer, long double *out) {
  uint8_t bytes[10];
  uint64_t bits = 0;
  if (!out || !width_supported(width, integer)) {
    return kX86pX87MemoryUnsupported;
  }
  if (!x86p_mem_read_bytes(mem, address, bytes, width)) {
    return kX86pX87MemoryFault;
  }
  if (width == 10) {
    *out = x86p_x87_from_f80(bytes);
  } else {
    for (unsigned i = 0; i < width; i++) {
      bits |= (uint64_t)bytes[i] << (8u * i);
    }
    *out = integer      ? x86p_x87_integer_value(bits, width)
           : width == 4 ? x86p_x87_from_f32((uint32_t)bits)
                        : x86p_x87_from_f64(bits);
  }
  return kX86pX87MemoryOk;
}

X86pX87MemoryStatus
x86p_x87_write_value(X86pX87 *f, const X86pMem *mem, uint32_t address, unsigned width, int integer, long double value) {
  uint8_t bytes[10];
  uint64_t bits;
  if (!f || !width_supported(width, integer)) {
    return kX86pX87MemoryUnsupported;
  }
  if (width == 10) {
    x86p_x87_to_f80(value, bytes);
  } else {
    if (integer) {
      bits = width == 2   ? x86p_x87_to_i16(f, value)
             : width == 4 ? x86p_x87_to_i32(f, value)
                          : x86p_x87_to_i64(f, value);
    } else {
      bits = width == 4 ? x86p_x87_to_f32(f, value) : x86p_x87_to_f64(f, value);
    }
    for (unsigned i = 0; i < width; i++) {
      bytes[i] = (uint8_t)(bits >> (8u * i));
    }
  }
  return x86p_mem_write_bytes(mem, address, bytes, width) ? kX86pX87MemoryOk : kX86pX87MemoryFault;
}

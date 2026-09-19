#include "x87_memory.h"

/* The widths that arrive as a little-endian integer's worth of bits, which is
   every width except the ten bytes of an 80-bit operand. */
static int bits_width_supported(unsigned width, int integer) {
  return integer ? (width == 2 || width == 4 || width == 8) : (width == 4 || width == 8);
}

static int width_supported(unsigned width, int integer) {
  return bits_width_supported(width, integer) || (!integer && width == 10);
}

X86pX87MemoryStatus x86p_x87_reg_from_operand_bits(uint64_t bits, unsigned width, int integer, X86pX87Reg *out) {
  if (!out || !bits_width_supported(width, integer)) {
    return kX86pX87MemoryUnsupported;
  }
  *out = integer      ? x86p_x87_reg_from_long_double(x86p_x87_integer_value(bits, width))
         : width == 4 ? x86p_x87_reg_from_f32_bits((uint32_t)bits)
                      : x86p_x87_reg_from_f64_bits(bits);
  return kX86pX87MemoryOk;
}

/*
 * The raw forms are the implementation and the long double forms are wrappers,
 * so there is one width table, one fault policy and one set of conversions.
 * An 80-bit operand is the interesting case: on a binary128 host the storage
 * already IS those ten bytes, so the load and the store are copies with
 * nothing to round -- which is also why the WASM backend no longer has to
 * refuse them.
 */
X86pX87MemoryStatus
x86p_x87_read_value_raw(const X86pMem *mem, uint32_t address, unsigned width, int integer, X86pX87Reg *out) {
  uint8_t bytes[10];
  uint64_t bits = 0;
  if (!out || !width_supported(width, integer)) {
    return kX86pX87MemoryUnsupported;
  }
  /* One walk per operand, and it belongs to the memory owner rather than to
     this caller -- x86p_mem_read_bytes resolves the whole span when there is
     one, so every bulk reader gets it and none of them repeats the policy. */
  if (!x86p_mem_read_bytes(mem, address, bytes, width)) {
    return kX86pX87MemoryFault;
  }
  if (width == 10) {
    *out = x86p_x87_reg_from_f80(bytes);
    return kX86pX87MemoryOk;
  }
  for (unsigned i = 0; i < width; i++) {
    bits |= (uint64_t)bytes[i] << (8u * i);
  }
  return x86p_x87_reg_from_operand_bits(bits, width, integer, out);
}

X86pX87MemoryStatus x86p_x87_write_value_raw(
    X86pX87 *f, const X86pMem *mem, uint32_t address, unsigned width, int integer, X86pX87Reg value) {
  uint8_t bytes[10];
  uint64_t bits;
  if (!f || !width_supported(width, integer)) {
    return kX86pX87MemoryUnsupported;
  }
  if (width == 10) {
    x86p_x87_reg_to_f80(value, bytes);
  } else {
    if (integer) {
      /* The integer conversions report their own invalid-operation results
         into the status word, so they keep the long double entry points. */
      const long double v = x86p_x87_reg_to_long_double(value);
      bits = width == 2 ? x86p_x87_to_i16(f, v) : width == 4 ? x86p_x87_to_i32(f, v) : x86p_x87_to_i64(f, v);
    } else {
      bits = width == 4 ? x86p_x87_reg_to_f32_bits(f, value) : x86p_x87_reg_to_f64_bits(f, value);
    }
    for (unsigned i = 0; i < width; i++) {
      bytes[i] = (uint8_t)(bits >> (8u * i));
    }
  }
  return x86p_mem_write_bytes(mem, address, bytes, width) ? kX86pX87MemoryOk : kX86pX87MemoryFault;
}

X86pX87MemoryStatus
x86p_x87_read_value(const X86pMem *mem, uint32_t address, unsigned width, int integer, long double *out) {
  X86pX87Reg raw;
  X86pX87MemoryStatus status;
  if (!out) {
    return kX86pX87MemoryUnsupported;
  }
  status = x86p_x87_read_value_raw(mem, address, width, integer, &raw);
  if (status == kX86pX87MemoryOk) {
    *out = x86p_x87_reg_to_long_double(raw);
  }
  return status;
}

X86pX87MemoryStatus
x86p_x87_write_value(X86pX87 *f, const X86pMem *mem, uint32_t address, unsigned width, int integer, long double value) {
  return x86p_x87_write_value_raw(f, mem, address, width, integer, x86p_x87_reg_from_long_double(value));
}

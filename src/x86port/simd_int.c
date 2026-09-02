/*
 * simd_int.c -- the packed-integer lanes of MMX and SSE2.
 *
 * Every operation here is exact: a packed add of eight bytes has one answer,
 * and so does a saturating one. There is nothing to approximate and therefore
 * nothing to refuse -- which is why this file has no refusal list and
 * simd_float.c does.
 *
 * THE WIDTH COMES FROM THE OPERAND, NOT FROM THE OPERATION. PADDW is the same
 * instruction at 64 and 128 bits and differs only in how many words it
 * touches, so each loop derives its count from `a->bytes`. A version that
 * assumed eight bytes would quietly process half of an XMM operand and leave
 * the top half holding the destination's old value, which looks like a subtle
 * arithmetic bug rather than a width bug.
 */
#include "simd_internal.h"

/* Saturation, written once per element type rather than per instruction: the
   clamp is what the S and US suffixes MEAN, and four instructions share it. */
static uint8_t sat_u8(int32_t v) {
  return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static int8_t sat_s8(int32_t v) {
  return (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
}

static uint16_t sat_u16(int32_t v) {
  return (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
}

static int16_t sat_s16(int32_t v) {
  return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

/*
 * A shift COUNT is read as a whole 64-bit value and is not masked.
 *
 * This is the rule people get wrong: unlike the integer shifts, a packed shift
 * by 64 or more produces ZERO (or, for the arithmetic right shifts, all sign
 * bits) rather than shifting by count mod width. Masking the count to five
 * bits -- the reflex from SHL -- turns a deliberate "clear this register" into
 * a no-op.
 */
static unsigned shift_count(const X86pVec *b) {
  uint64_t c = vec_u64(b, 0);
  return c > 255u ? 256u : (unsigned)c;
}

static void unpack(const X86pVec *a, const X86pVec *b, unsigned elem, int high, X86pVec *out) {
  /*
   * Interleave the destination's elements with the source's, taking them from
   * the LOW half of each operand -- or the HIGH half for the H forms.
   *
   * Done per 8-byte lane, which is what the 128-bit encodings do as well: they
   * interleave within each quadword rather than across the whole register, and
   * a version that treated the operand as one long vector would produce the
   * right answer at 64 bits and the wrong one at 128.
   */
  X86pVec r;
  unsigned lanes = a->bytes / 8u;
  unsigned per = 8u / elem; /* elements per 8-byte lane */
  unsigned lane;
  unsigned i;
  memset(&r, 0, sizeof r);
  r.bytes = a->bytes;
  for (lane = 0; lane < lanes; lane++) {
    unsigned base = lane * 8u;
    for (i = 0; i < per / 2u; i++) {
      unsigned from = base + (high ? (per / 2u) : 0u) * elem + i * elem;
      unsigned to = base + i * 2u * elem;
      memcpy(r.b + to, a->b + from, elem);
      memcpy(r.b + to + elem, b->b + from, elem);
    }
  }
  *out = r;
}

int x86p_simd_int(X86pSimdOp op, const X86pVec *a, const X86pVec *b, uint8_t imm, X86pVec *out) {
  X86pVec r;
  unsigned n;
  unsigned i;

  if (!a || !b || !out) {
    return 0;
  }
  memset(&r, 0, sizeof r);
  r.bytes = a->bytes;

  switch (op) {
  /* ---- bitwise: the width is irrelevant, so no lane loop at all ---- */
  case kX86pSimdPand:
  case kX86pSimdPandn:
  case kX86pSimdPor:
  case kX86pSimdPxor:
    for (i = 0; i < a->bytes; i++) {
      uint8_t x = a->b[i];
      uint8_t y = b->b[i];
      r.b[i] = (op == kX86pSimdPand)    ? (uint8_t)(x & y)
               : (op == kX86pSimdPandn) ? (uint8_t)((uint8_t)~x & y) /* NOT the destination, then AND */
               : (op == kX86pSimdPor)   ? (uint8_t)(x | y)
                                        : (uint8_t)(x ^ y);
    }
    break;

  /* ---- byte lanes ---- */
  case kX86pSimdPaddb:
  case kX86pSimdPsubb:
  case kX86pSimdPaddsb:
  case kX86pSimdPsubsb:
  case kX86pSimdPaddusb:
  case kX86pSimdPsubusb:
  case kX86pSimdPcmpeqb:
  case kX86pSimdPcmpgtb:
  case kX86pSimdPavgb:
  case kX86pSimdPminub:
  case kX86pSimdPmaxub:
    n = a->bytes;
    for (i = 0; i < n; i++) {
      int32_t x = (int32_t)vec_u8(a, i);
      int32_t y = (int32_t)vec_u8(b, i);
      int32_t sx = (int8_t)x;
      int32_t sy = (int8_t)y;
      uint8_t v;
      switch (op) {
      case kX86pSimdPaddb:
        v = (uint8_t)(x + y);
        break;
      case kX86pSimdPsubb:
        v = (uint8_t)(x - y);
        break;
      case kX86pSimdPaddsb:
        v = (uint8_t)sat_s8(sx + sy);
        break;
      case kX86pSimdPsubsb:
        v = (uint8_t)sat_s8(sx - sy);
        break;
      case kX86pSimdPaddusb:
        v = sat_u8(x + y);
        break;
      case kX86pSimdPsubusb:
        v = sat_u8(x - y);
        break;
      case kX86pSimdPcmpeqb:
        v = (uint8_t)(x == y ? 0xFFu : 0x00u);
        break;
      case kX86pSimdPcmpgtb:
        v = (uint8_t)(sx > sy ? 0xFFu : 0x00u);
        break;
      case kX86pSimdPavgb:
        /* Rounded, not truncated: +1 before the shift. */
        v = (uint8_t)((x + y + 1) >> 1);
        break;
      case kX86pSimdPminub:
        v = (uint8_t)(x < y ? x : y);
        break;
      default:
        v = (uint8_t)(x > y ? x : y);
        break;
      }
      vec_set_u8(&r, i, v);
    }
    break;

  /* ---- word lanes ---- */
  case kX86pSimdPaddw:
  case kX86pSimdPsubw:
  case kX86pSimdPaddsw:
  case kX86pSimdPsubsw:
  case kX86pSimdPaddusw:
  case kX86pSimdPsubusw:
  case kX86pSimdPcmpeqw:
  case kX86pSimdPcmpgtw:
  case kX86pSimdPavgw:
  case kX86pSimdPminsw:
  case kX86pSimdPmaxsw:
  case kX86pSimdPmullw:
  case kX86pSimdPmulhw:
  case kX86pSimdPmulhuw:
    n = a->bytes / 2u;
    for (i = 0; i < n; i++) {
      int32_t x = (int32_t)vec_u16(a, i);
      int32_t y = (int32_t)vec_u16(b, i);
      int32_t sx = (int16_t)x;
      int32_t sy = (int16_t)y;
      uint16_t v;
      switch (op) {
      case kX86pSimdPaddw:
        v = (uint16_t)(x + y);
        break;
      case kX86pSimdPsubw:
        v = (uint16_t)(x - y);
        break;
      case kX86pSimdPaddsw:
        v = (uint16_t)sat_s16(sx + sy);
        break;
      case kX86pSimdPsubsw:
        v = (uint16_t)sat_s16(sx - sy);
        break;
      case kX86pSimdPaddusw:
        v = sat_u16(x + y);
        break;
      case kX86pSimdPsubusw:
        v = sat_u16(x - y);
        break;
      case kX86pSimdPcmpeqw:
        v = (uint16_t)(x == y ? 0xFFFFu : 0u);
        break;
      case kX86pSimdPcmpgtw:
        v = (uint16_t)(sx > sy ? 0xFFFFu : 0u);
        break;
      case kX86pSimdPavgw:
        v = (uint16_t)((x + y + 1) >> 1);
        break;
      case kX86pSimdPminsw:
        v = (uint16_t)(sx < sy ? sx : sy);
        break;
      case kX86pSimdPmaxsw:
        v = (uint16_t)(sx > sy ? sx : sy);
        break;
      case kX86pSimdPmullw:
        v = (uint16_t)(sx * sy);
        break;
      case kX86pSimdPmulhw:
        /* The HIGH half of a SIGNED product. The unsigned form below is a
           different instruction, and using one for the other is right for
           every value with the top bit clear. */
        v = (uint16_t)(((sx * sy) >> 16) & 0xFFFF);
        break;
      default: /* PMULHUW */
        v = (uint16_t)(((uint32_t)(uint16_t)x * (uint32_t)(uint16_t)y) >> 16);
        break;
      }
      vec_set_u16(&r, i, v);
    }
    break;

  /* ---- dword lanes ---- */
  case kX86pSimdPaddd:
  case kX86pSimdPsubd:
  case kX86pSimdPcmpeqd:
  case kX86pSimdPcmpgtd:
    n = a->bytes / 4u;
    for (i = 0; i < n; i++) {
      uint32_t x = vec_u32(a, i);
      uint32_t y = vec_u32(b, i);
      uint32_t v;
      switch (op) {
      case kX86pSimdPaddd:
        v = x + y;
        break;
      case kX86pSimdPsubd:
        v = x - y;
        break;
      case kX86pSimdPcmpeqd:
        v = (x == y) ? 0xFFFFFFFFu : 0u;
        break;
      default:
        v = ((int32_t)x > (int32_t)y) ? 0xFFFFFFFFu : 0u;
        break;
      }
      vec_set_u32(&r, i, v);
    }
    break;

  /* ---- qword lanes ---- */
  case kX86pSimdPaddq:
  case kX86pSimdPsubq:
    n = a->bytes / 8u;
    for (i = 0; i < n; i++) {
      uint64_t x = vec_u64(a, i);
      uint64_t y = vec_u64(b, i);
      vec_set_u64(&r, i, op == kX86pSimdPaddq ? x + y : x - y);
    }
    break;

  /* ---- interleave ---- */
  case kX86pSimdPunpcklbw:
    unpack(a, b, 1u, 0, &r);
    break;
  case kX86pSimdPunpcklwd:
    unpack(a, b, 2u, 0, &r);
    break;
  case kX86pSimdPunpckldq:
    unpack(a, b, 4u, 0, &r);
    break;
  case kX86pSimdPunpckhbw:
    unpack(a, b, 1u, 1, &r);
    break;
  case kX86pSimdPunpckhwd:
    unpack(a, b, 2u, 1, &r);
    break;
  case kX86pSimdPunpckhdq:
    unpack(a, b, 4u, 1, &r);
    break;

  /* ---- pack, with saturation ---- */
  case kX86pSimdPackuswb:
  case kX86pSimdPacksswb:
    /* The destination's words fill the low half of the result and the
       source's the high half -- not interleaved. */
    n = a->bytes / 2u;
    for (i = 0; i < n; i++) {
      int32_t x = (int16_t)vec_u16(a, i);
      int32_t y = (int16_t)vec_u16(b, i);
      if (op == kX86pSimdPackuswb) {
        vec_set_u8(&r, i, sat_u8(x));
        vec_set_u8(&r, n + i, sat_u8(y));
      } else {
        vec_set_u8(&r, i, (uint8_t)sat_s8(x));
        vec_set_u8(&r, n + i, (uint8_t)sat_s8(y));
      }
    }
    break;
  case kX86pSimdPackssdw:
    n = a->bytes / 4u;
    for (i = 0; i < n; i++) {
      vec_set_u16(&r, i, (uint16_t)sat_s16((int32_t)vec_u32(a, i)));
      vec_set_u16(&r, n + i, (uint16_t)sat_s16((int32_t)vec_u32(b, i)));
    }
    break;

  /* ---- shifts ---- */
  case kX86pSimdPsllw:
  case kX86pSimdPsrlw:
  case kX86pSimdPsraw: {
    unsigned c = shift_count(b);
    n = a->bytes / 2u;
    for (i = 0; i < n; i++) {
      uint16_t x = vec_u16(a, i);
      uint16_t v;
      if (op == kX86pSimdPsraw) {
        unsigned s = c > 15u ? 15u : c; /* saturates to all sign bits */
        v = (uint16_t)((int16_t)x >> s);
      } else if (c > 15u) {
        v = 0u;
      } else {
        v = (uint16_t)(op == kX86pSimdPsllw ? (uint16_t)(x << c) : (uint16_t)(x >> c));
      }
      vec_set_u16(&r, i, v);
    }
    break;
  }
  case kX86pSimdPslld:
  case kX86pSimdPsrld:
  case kX86pSimdPsrad: {
    unsigned c = shift_count(b);
    n = a->bytes / 4u;
    for (i = 0; i < n; i++) {
      uint32_t x = vec_u32(a, i);
      uint32_t v;
      if (op == kX86pSimdPsrad) {
        unsigned s = c > 31u ? 31u : c;
        v = (uint32_t)((int32_t)x >> s);
      } else if (c > 31u) {
        v = 0u;
      } else {
        v = (op == kX86pSimdPslld) ? (x << c) : (x >> c);
      }
      vec_set_u32(&r, i, v);
    }
    break;
  }
  case kX86pSimdPsllq:
  case kX86pSimdPsrlq: {
    unsigned c = shift_count(b);
    n = a->bytes / 8u;
    for (i = 0; i < n; i++) {
      uint64_t x = vec_u64(a, i);
      uint64_t v = (c > 63u) ? 0u : (op == kX86pSimdPsllq ? (x << c) : (x >> c));
      vec_set_u64(&r, i, v);
    }
    break;
  }

  /* ---- multiply-add and sum-of-differences ---- */
  case kX86pSimdPmaddwd:
    n = a->bytes / 4u;
    for (i = 0; i < n; i++) {
      int32_t lo = (int32_t)(int16_t)vec_u16(a, i * 2u) * (int32_t)(int16_t)vec_u16(b, i * 2u);
      int32_t hi = (int32_t)(int16_t)vec_u16(a, i * 2u + 1u) * (int32_t)(int16_t)vec_u16(b, i * 2u + 1u);
      vec_set_u32(&r, i, (uint32_t)(lo + hi));
    }
    break;
  case kX86pSimdPsadbw:
    n = a->bytes / 8u;
    for (i = 0; i < n; i++) {
      unsigned k;
      uint32_t sum = 0u;
      for (k = 0; k < 8u; k++) {
        int32_t d = (int32_t)vec_u8(a, i * 8u + k) - (int32_t)vec_u8(b, i * 8u + k);
        sum += (uint32_t)(d < 0 ? -d : d);
      }
      vec_set_u64(&r, i, (uint64_t)sum);
    }
    break;

  /* ---- shuffles, which take their control from the immediate ---- */
  case kX86pSimdPshufw:
    for (i = 0; i < 4u; i++) {
      vec_set_u16(&r, i, vec_u16(b, (unsigned)((imm >> (i * 2u)) & 3u)));
    }
    r.bytes = 8u;
    break;
  case kX86pSimdPshufd:
    for (i = 0; i < 4u; i++) {
      vec_set_u32(&r, i, vec_u32(b, (unsigned)((imm >> (i * 2u)) & 3u)));
    }
    r.bytes = 16u;
    break;

  default:
    return 0;
  }

  *out = r;
  return 1;
}

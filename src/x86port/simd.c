/*
 * simd.c -- operand access and dispatch for MMX, SSE and 3DNow!.
 *
 * What lives here is everything that is ABOUT the instruction rather than
 * about the arithmetic: where its operands come from, how wide they are, which
 * register file they name, and which of the three families owns its meaning.
 * The lanes are in simd_int.c and simd_float.c, and 3DNow! is in three_dnow.c,
 * so this file has no arithmetic in it at all.
 *
 * WHICH REGISTER FILE. An MMX operand is a view of the x87 registers -- see
 * x87.h, where the aliasing is modelled rather than papered over -- and an XMM
 * operand is its own file in X86pCpu. That difference is the reason operand
 * access is a function here instead of a pointer handed to the lane loops:
 * reading MM3 is not a memory access, it is a mantissa field.
 */
#include "simd.h"

#include "simd_internal.h"
#include "three_dnow.h"
#include "x87.h"

#include <stdio.h>
#include <string.h>

static const char *kStatusNames[] = {"ok", "memory fault", "named but not implemented"};
_Static_assert((int)(sizeof kStatusNames / sizeof kStatusNames[0]) == (int)kX86pSimdStatusCount,
               "every X86pSimdStatus needs a name");

/*
 * The name table IS the parse table, read in the other direction.
 *
 * One table rather than two, because two would be two things to keep in step
 * and the failure mode -- a name that parses to a different operation than it
 * prints as -- is silent.
 */
typedef struct SimdName {
  X86pSimdOp op;
  const char *mnemonic;
} SimdName;

static const SimdName kNames[] = {
    {kX86pSimdMovq, "MOVQ"},
    {kX86pSimdMovd, "MOVD"},
    {kX86pSimdMovaps, "MOVAPS"},
    {kX86pSimdMovaps, "MOVUPS"},
    {kX86pSimdMovaps, "MOVDQA"},
    {kX86pSimdMovaps, "MOVDQU"},
    {kX86pSimdMovaps, "MOVAPD"},
    {kX86pSimdMovaps, "MOVUPD"},
    {kX86pSimdMovss, "MOVSS"},
    {kX86pSimdMovlps, "MOVLPS"},
    {kX86pSimdMovlps, "MOVLPD"},
    {kX86pSimdMovhps, "MOVHPS"},
    {kX86pSimdMovhps, "MOVHPD"},
    {kX86pSimdMovhlps, "MOVHLPS"},
    {kX86pSimdMovlhps, "MOVLHPS"},
    {kX86pSimdMovmskps, "MOVMSKPS"},
    {kX86pSimdPmovmskb, "PMOVMSKB"},
    {kX86pSimdPand, "PAND"},
    {kX86pSimdPandn, "PANDN"},
    {kX86pSimdPor, "POR"},
    {kX86pSimdPxor, "PXOR"},
    {kX86pSimdPaddb, "PADDB"},
    {kX86pSimdPaddw, "PADDW"},
    {kX86pSimdPaddd, "PADDD"},
    {kX86pSimdPaddq, "PADDQ"},
    {kX86pSimdPaddsb, "PADDSB"},
    {kX86pSimdPaddsw, "PADDSW"},
    {kX86pSimdPaddusb, "PADDUSB"},
    {kX86pSimdPaddusw, "PADDUSW"},
    {kX86pSimdPsubb, "PSUBB"},
    {kX86pSimdPsubw, "PSUBW"},
    {kX86pSimdPsubd, "PSUBD"},
    {kX86pSimdPsubq, "PSUBQ"},
    {kX86pSimdPsubsb, "PSUBSB"},
    {kX86pSimdPsubsw, "PSUBSW"},
    {kX86pSimdPsubusb, "PSUBUSB"},
    {kX86pSimdPsubusw, "PSUBUSW"},
    {kX86pSimdPcmpeqb, "PCMPEQB"},
    {kX86pSimdPcmpeqw, "PCMPEQW"},
    {kX86pSimdPcmpeqd, "PCMPEQD"},
    {kX86pSimdPcmpgtb, "PCMPGTB"},
    {kX86pSimdPcmpgtw, "PCMPGTW"},
    {kX86pSimdPcmpgtd, "PCMPGTD"},
    {kX86pSimdPunpcklbw, "PUNPCKLBW"},
    {kX86pSimdPunpcklwd, "PUNPCKLWD"},
    {kX86pSimdPunpckldq, "PUNPCKLDQ"},
    {kX86pSimdPunpckhbw, "PUNPCKHBW"},
    {kX86pSimdPunpckhwd, "PUNPCKHWD"},
    {kX86pSimdPunpckhdq, "PUNPCKHDQ"},
    {kX86pSimdPackuswb, "PACKUSWB"},
    {kX86pSimdPacksswb, "PACKSSWB"},
    {kX86pSimdPackssdw, "PACKSSDW"},
    {kX86pSimdPsllw, "PSLLW"},
    {kX86pSimdPslld, "PSLLD"},
    {kX86pSimdPsllq, "PSLLQ"},
    {kX86pSimdPsrlw, "PSRLW"},
    {kX86pSimdPsrld, "PSRLD"},
    {kX86pSimdPsrlq, "PSRLQ"},
    {kX86pSimdPsraw, "PSRAW"},
    {kX86pSimdPsrad, "PSRAD"},
    {kX86pSimdPmullw, "PMULLW"},
    {kX86pSimdPmulhw, "PMULHW"},
    {kX86pSimdPmulhuw, "PMULHUW"},
    {kX86pSimdPmaddwd, "PMADDWD"},
    {kX86pSimdPavgb, "PAVGB"},
    {kX86pSimdPavgw, "PAVGW"},
    {kX86pSimdPminub, "PMINUB"},
    {kX86pSimdPmaxub, "PMAXUB"},
    {kX86pSimdPminsw, "PMINSW"},
    {kX86pSimdPmaxsw, "PMAXSW"},
    {kX86pSimdPsadbw, "PSADBW"},
    {kX86pSimdPextrw, "PEXTRW"},
    {kX86pSimdPinsrw, "PINSRW"},
    {kX86pSimdPshufw, "PSHUFW"},
    {kX86pSimdPshufd, "PSHUFD"},
    {kX86pSimdAddps, "ADDPS"},
    {kX86pSimdSubps, "SUBPS"},
    {kX86pSimdMulps, "MULPS"},
    {kX86pSimdDivps, "DIVPS"},
    {kX86pSimdMinps, "MINPS"},
    {kX86pSimdMaxps, "MAXPS"},
    {kX86pSimdSqrtps, "SQRTPS"},
    {kX86pSimdAddss, "ADDSS"},
    {kX86pSimdSubss, "SUBSS"},
    {kX86pSimdMulss, "MULSS"},
    {kX86pSimdDivss, "DIVSS"},
    {kX86pSimdMinss, "MINSS"},
    {kX86pSimdMaxss, "MAXSS"},
    {kX86pSimdSqrtss, "SQRTSS"},
    {kX86pSimdAndps, "ANDPS"},
    {kX86pSimdAndnps, "ANDNPS"},
    {kX86pSimdOrps, "ORPS"},
    {kX86pSimdXorps, "XORPS"},
    {kX86pSimdCmpps, "CMPPS"},
    {kX86pSimdCmpss, "CMPSS"},
    {kX86pSimdComiss, "COMISS"},
    {kX86pSimdUcomiss, "UCOMISS"},
    {kX86pSimdShufps, "SHUFPS"},
    {kX86pSimdUnpcklps, "UNPCKLPS"},
    {kX86pSimdUnpckhps, "UNPCKHPS"},
    {kX86pSimdCvtsi2ss, "CVTSI2SS"},
    {kX86pSimdCvtss2si, "CVTSS2SI"},
    {kX86pSimdCvttss2si, "CVTTSS2SI"},
    {kX86pSimdCvtpi2ps, "CVTPI2PS"},
    {kX86pSimdCvtps2pi, "CVTPS2PI"},
    {kX86pSimdCvttps2pi, "CVTTPS2PI"},
    {kX86pSimdEmms, "EMMS"},
    {kX86pSimdEmms, "FEMMS"},
    {kX86pSimdLdmxcsr, "LDMXCSR"},
    {kX86pSimdStmxcsr, "STMXCSR"},
    {kX86pSimdFence, "SFENCE"},
    {kX86pSimdFence, "LFENCE"},
    {kX86pSimdFence, "MFENCE"},
    {kX86pSimdPrefetch, "PREFETCH"},
    {kX86pSimdPrefetch, "PREFETCHNTA"},
    {kX86pSimdPrefetch, "PREFETCHT0"},
    {kX86pSimdPrefetch, "PREFETCHT1"},
    {kX86pSimdPrefetch, "PREFETCHT2"},
    {kX86pSimdPrefetch, "PREFETCHW"},
    {kX86pSimdRcpps, "RCPPS"},
    {kX86pSimdRcpss, "RCPSS"},
    {kX86pSimdRsqrtps, "RSQRTPS"},
    {kX86pSimdRsqrtss, "RSQRTSS"},
};

/*
 * The operations that are named and deliberately NOT implemented.
 *
 * A list rather than a flag on each entry, so that adding an implementation
 * means deleting a line here -- and so that the count of refusals is a thing
 * this file can report rather than a property scattered through a switch.
 */
static int is_refused(X86pSimdOp op) {
  return op == kX86pSimdRcpps || op == kX86pSimdRcpss || op == kX86pSimdRsqrtps || op == kX86pSimdRsqrtss;
}

int x86p_simd_is_implemented(X86pSimdOp op) {
  return op >= 0 && op < kX86pSimdOpCount && !is_refused(op);
}

const char *x86p_simd_status_name(X86pSimdStatus s) {
  if (s < 0 || s >= kX86pSimdStatusCount) {
    return "?";
  }
  return kStatusNames[s];
}

const char *x86p_simd_op_name(X86pSimdOp op) {
  unsigned i;
  for (i = 0; i < sizeof kNames / sizeof kNames[0]; i++) {
    if (kNames[i].op == op) {
      return kNames[i].mnemonic;
    }
  }
  return "?";
}

int x86p_simd_parse(const char *mnemonic, X86pSimdOp *out) {
  unsigned i;
  if (!mnemonic || !out) {
    return 0;
  }
  for (i = 0; i < sizeof kNames / sizeof kNames[0]; i++) {
    if (strcmp(kNames[i].mnemonic, mnemonic) == 0) {
      *out = kNames[i].op;
      return 1;
    }
  }
  return 0;
}

/* ---- operand access ------------------------------------------------------ */

typedef struct Ctx {
  X86pCpu *cpu;
  const X86pMem *mem;
  int fault;
  uint32_t fault_addr;
} Ctx;

static uint32_t address_of(Ctx *c, const X86pOperand *o) {
  uint32_t addr = (uint32_t)o->disp;
  if (o->base >= 0) {
    addr += c->cpu->reg[o->base];
  }
  if (o->index >= 0) {
    addr += c->cpu->reg[o->index] * (uint32_t)o->scale;
  }
  if (o->seg == (uint8_t)kX86pSegFs) {
    addr += c->cpu->fs_base;
  } else if (o->seg == (uint8_t)kX86pSegGs) {
    addr += c->cpu->gs_base;
  }
  return addr;
}

/*
 * Read an operand as a vector of `bytes` bytes.
 *
 * `bytes` comes from the INSTRUCTION rather than from the operand, because a
 * memory operand's size is what the encoding says and several instructions
 * read fewer bytes than their register operand is wide -- MOVSS reads four
 * from memory into a sixteen-byte register, MOVLPS eight. Deriving the width
 * from the register would read past the end of the guest's data.
 */
static int read_vec(Ctx *c, const X86pOperand *o, unsigned bytes, X86pVec *out) {
  memset(out, 0, sizeof *out);
  out->bytes = bytes;
  switch (o->kind) {
  case kX86pOperandMmx: {
    uint64_t q = 0u;
    if (!x86p_x87_mmx_read(&c->cpu->x87, o->reg, &q)) {
      c->fault = kX86pSimdUnsupported;
      return 0;
    }
    memcpy(out->b, &q, sizeof q);
    return 1;
  }
  case kX86pOperandXmm:
    memcpy(out->b, c->cpu->xmm[o->reg], 16u);
    return 1;
  case kX86pOperandReg:
    /* A general register, for the conversions and the extract/insert forms. */
    vec_set_u32(out, 0, x86p_reg_read(c->cpu, o->reg, o->size));
    return 1;
  case kX86pOperandImm:
    vec_set_u32(out, 0, o->imm);
    return 1;
  case kX86pOperandMem: {
    uint32_t addr = address_of(c, o);
    if (!x86p_mem_read_bytes(c->mem, addr, out->b, bytes)) {
      c->fault = kX86pSimdFault;
      c->fault_addr = addr;
      return 0;
    }
    return 1;
  }
  default:
    c->fault = kX86pSimdUnsupported;
    return 0;
  }
}

static int write_vec(Ctx *c, const X86pOperand *o, unsigned bytes, const X86pVec *v) {
  switch (o->kind) {
  case kX86pOperandMmx: {
    uint64_t q;
    memcpy(&q, v->b, sizeof q);
    if (!x86p_x87_mmx_write(&c->cpu->x87, o->reg, q)) {
      c->fault = kX86pSimdUnsupported;
      return 0;
    }
    return 1;
  }
  case kX86pOperandXmm:
    /*
     * A write narrower than the register leaves the rest of it alone. That is
     * the rule for MOVSS register-to-register, MOVLPS and MOVHPS, and it is
     * the opposite of the rule for MOVSS loading from MEMORY, which zeroes.
     * The difference lives at the call site because it belongs to the
     * instruction, not to the register file.
     */
    memcpy(c->cpu->xmm[o->reg], v->b, bytes);
    return 1;
  case kX86pOperandReg:
    x86p_reg_write(c->cpu, o->reg, o->size, vec_u32(v, 0));
    return 1;
  case kX86pOperandMem: {
    uint32_t addr = address_of(c, o);
    if (!x86p_mem_write_bytes(c->mem, addr, v->b, bytes)) {
      c->fault = kX86pSimdFault;
      c->fault_addr = addr;
      return 0;
    }
    return 1;
  }
  default:
    c->fault = kX86pSimdUnsupported;
    return 0;
  }
}

/* How wide the operation's register operands are: an MMX instruction moves 8
   bytes and an XMM one 16, and the operands say which this is. */
static unsigned reg_width(const X86pInsn *in) {
  int i;
  for (i = 0; i < in->operands; i++) {
    if (in->operand[i].kind == kX86pOperandXmm) {
      return 16u;
    }
  }
  return 8u;
}

/* ---- dispatch ------------------------------------------------------------ */

static X86pSimdStatus finish(Ctx *c, uint32_t *fault_addr) {
  if (c->fault) {
    if (fault_addr) {
      *fault_addr = c->fault_addr;
    }
    return (X86pSimdStatus)c->fault;
  }
  return kX86pSimdOk;
}

X86pSimdStatus x86p_simd_execute(X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, uint32_t *fault_addr) {
  Ctx c;
  X86pSimdOp op;
  const X86pOperand *d;
  const X86pOperand *s;
  X86pVec a;
  X86pVec b;
  X86pVec r;
  unsigned w;
  uint8_t imm = 0u;

  if (fault_addr) {
    *fault_addr = 0u;
  }
  if (!cpu || !insn) {
    return kX86pSimdFault;
  }
  memset(&c, 0, sizeof c);
  c.cpu = cpu;
  c.mem = mem;
  op = (X86pSimdOp)insn->simd;
  if (op < 0 || op >= kX86pSimdOpCount || is_refused(op)) {
    return kX86pSimdUnsupported;
  }

  d = &insn->operand[0];
  s = &insn->operand[1];
  w = reg_width(insn);
  if (insn->operands >= 3 && insn->operand[2].kind == kX86pOperandImm) {
    imm = (uint8_t)insn->operand[2].imm;
  }

  switch (op) {
  case kX86pSimdEmms:
    x86p_x87_emms(&cpu->x87);
    return kX86pSimdOk;
  case kX86pSimdFence:
  case kX86pSimdPrefetch:
    /* Architecturally a hint with no result. There is nothing to do and,
       crucially, nothing to get wrong -- but it is still NAMED and counted
       rather than falling into a default that would also swallow a real
       instruction. */
    return kX86pSimdOk;
  case kX86pSimdStmxcsr:
    memset(&r, 0, sizeof r);
    r.bytes = 4u;
    vec_set_u32(&r, 0, cpu->mxcsr);
    (void)write_vec(&c, d, 4u, &r);
    return finish(&c, fault_addr);
  case kX86pSimdLdmxcsr:
    if (!read_vec(&c, d, 4u, &a)) {
      return finish(&c, fault_addr);
    }
    cpu->mxcsr = vec_u32(&a, 0);
    return kX86pSimdOk;

  /* ---- movement, where the WIDTH is the whole content of the instruction -- */
  case kX86pSimdMovq:
    if (!read_vec(&c, s, 8u, &b)) {
      return finish(&c, fault_addr);
    }
    b.bytes = 8u;
    if (d->kind == kX86pOperandXmm) {
      /* MOVQ into an XMM register ZEROES the upper 64 bits. Copying only the
         low half would leave whatever the register held, which is the shape of
         bug that survives every test using a freshly zeroed register. */
      memset(&r, 0, sizeof r);
      memcpy(r.b, b.b, 8u);
      r.bytes = 16u;
      (void)write_vec(&c, d, 16u, &r);
    } else {
      (void)write_vec(&c, d, 8u, &b);
    }
    return finish(&c, fault_addr);

  case kX86pSimdMovd:
    if (!read_vec(&c, s, 4u, &b)) {
      return finish(&c, fault_addr);
    }
    memset(&r, 0, sizeof r);
    vec_set_u32(&r, 0, vec_u32(&b, 0));
    if (d->kind == kX86pOperandReg || d->kind == kX86pOperandMem) {
      r.bytes = 4u;
      (void)write_vec(&c, d, 4u, &r);
    } else {
      /* Into a vector register: zero-extended to its full width. */
      r.bytes = (d->kind == kX86pOperandXmm) ? 16u : 8u;
      (void)write_vec(&c, d, r.bytes, &r);
    }
    return finish(&c, fault_addr);

  case kX86pSimdMovaps: {
    unsigned bytes = (d->kind == kX86pOperandXmm || s->kind == kX86pOperandXmm) ? 16u : 8u;
    if (!read_vec(&c, s, bytes, &b)) {
      return finish(&c, fault_addr);
    }
    b.bytes = bytes;
    (void)write_vec(&c, d, bytes, &b);
    return finish(&c, fault_addr);
  }

  case kX86pSimdMovss:
    if (!read_vec(&c, s, 4u, &b)) {
      return finish(&c, fault_addr);
    }
    if (d->kind == kX86pOperandXmm && s->kind == kX86pOperandMem) {
      /* From memory: the upper three lanes are ZEROED. From a register: they
         are preserved. Two rules for one mnemonic, and the one a test with a
         zeroed destination cannot tell apart. */
      memset(&r, 0, sizeof r);
      vec_set_u32(&r, 0, vec_u32(&b, 0));
      r.bytes = 16u;
      (void)write_vec(&c, d, 16u, &r);
    } else if (d->kind == kX86pOperandXmm) {
      memcpy(r.b, cpu->xmm[d->reg], 16u);
      r.bytes = 16u;
      vec_set_u32(&r, 0, vec_u32(&b, 0));
      (void)write_vec(&c, d, 16u, &r);
    } else {
      b.bytes = 4u;
      (void)write_vec(&c, d, 4u, &b);
    }
    return finish(&c, fault_addr);

  case kX86pSimdMovlps:
  case kX86pSimdMovhps: {
    unsigned half = (op == kX86pSimdMovhps) ? 8u : 0u;
    if (d->kind == kX86pOperandMem) {
      memcpy(r.b, cpu->xmm[s->reg] + half, 8u);
      r.bytes = 8u;
      (void)write_vec(&c, d, 8u, &r);
    } else {
      if (!read_vec(&c, s, 8u, &b)) {
        return finish(&c, fault_addr);
      }
      memcpy(r.b, cpu->xmm[d->reg], 16u);
      r.bytes = 16u;
      memcpy(r.b + half, b.b, 8u);
      (void)write_vec(&c, d, 16u, &r);
    }
    return finish(&c, fault_addr);
  }

  case kX86pSimdMovhlps:
  case kX86pSimdMovlhps:
    if (d->kind != kX86pOperandXmm || s->kind != kX86pOperandXmm) {
      return kX86pSimdUnsupported;
    }
    memcpy(r.b, cpu->xmm[d->reg], 16u);
    r.bytes = 16u;
    if (op == kX86pSimdMovhlps) {
      memcpy(r.b, cpu->xmm[s->reg] + 8u, 8u); /* dest low <- src high */
    } else {
      memcpy(r.b + 8u, cpu->xmm[s->reg], 8u); /* dest high <- src low */
    }
    (void)write_vec(&c, d, 16u, &r);
    return finish(&c, fault_addr);

  case kX86pSimdMovmskps:
  case kX86pSimdPmovmskb: {
    uint32_t mask = 0u;
    unsigned bytes = (s->kind == kX86pOperandXmm) ? 16u : 8u;
    unsigned i;
    if (!read_vec(&c, s, bytes, &b)) {
      return finish(&c, fault_addr);
    }
    if (op == kX86pSimdMovmskps) {
      for (i = 0; i < bytes / 4u; i++) {
        mask |= (uint32_t)((vec_u32(&b, i) >> 31) & 1u) << i;
      }
    } else {
      for (i = 0; i < bytes; i++) {
        mask |= (uint32_t)((vec_u8(&b, i) >> 7) & 1u) << i;
      }
    }
    x86p_reg_write(cpu, d->reg, 4, mask);
    return kX86pSimdOk;
  }

  case kX86pSimdPextrw: {
    unsigned bytes = (s->kind == kX86pOperandXmm) ? 16u : 8u;
    unsigned sel;
    if (!read_vec(&c, s, bytes, &b)) {
      return finish(&c, fault_addr);
    }
    /* The selector is masked to the number of words the source HAS -- three
       bits for MMX, four for XMM -- not to eight in both cases. */
    sel = (unsigned)imm & (bytes == 16u ? 7u : 3u);
    x86p_reg_write(cpu, d->reg, 4, vec_u16(&b, sel));
    return kX86pSimdOk;
  }

  case kX86pSimdPinsrw: {
    unsigned sel;
    if (!read_vec(&c, d, w, &a) || !read_vec(&c, s, 2u, &b)) {
      return finish(&c, fault_addr);
    }
    sel = (unsigned)imm & (w == 16u ? 7u : 3u);
    r = a;
    vec_set_u16(&r, sel, (uint16_t)vec_u32(&b, 0));
    (void)write_vec(&c, d, w, &r);
    return finish(&c, fault_addr);
  }

  /* ---- 3DNow!, whose meaning belongs to three_dnow.c ---- */
  case kX86pSimdPf: {
    X86pMm ma;
    X86pMm mb;
    X86pMm mr;
    if (!read_vec(&c, d, 8u, &a) || !read_vec(&c, s, 8u, &b)) {
      return finish(&c, fault_addr);
    }
    memcpy(&ma.q, a.b, sizeof ma.q);
    memcpy(&mb.q, b.b, sizeof mb.q);
    if (!x86p_3dnow_eval((X86pPfOp)insn->pf, ma, mb, &mr)) {
      /* An approximation instruction, refused by name in three_dnow.c. */
      return kX86pSimdUnsupported;
    }
    memset(&r, 0, sizeof r);
    r.bytes = 8u;
    memcpy(r.b, &mr.q, sizeof mr.q);
    (void)write_vec(&c, d, 8u, &r);
    return finish(&c, fault_addr);
  }

  default:
    break;
  }

  /*
   * The regular two-operand shape: read the destination, read the source at
   * the same width, compute, write back.
   *
   * The conversions are the exception, because their two operands are
   * different widths and different files; they carry their own widths.
   */
  {
    unsigned src_bytes = w;
    unsigned dst_bytes = w;
    switch (op) {
    case kX86pSimdCvtsi2ss:
      src_bytes = 4u;
      dst_bytes = 16u;
      break;
    case kX86pSimdCvtss2si:
    case kX86pSimdCvttss2si:
      src_bytes = 4u;
      dst_bytes = 4u;
      break;
    case kX86pSimdCvtpi2ps:
      src_bytes = 8u;
      dst_bytes = 16u;
      break;
    case kX86pSimdCvtps2pi:
    case kX86pSimdCvttps2pi:
      src_bytes = 16u;
      dst_bytes = 8u;
      break;
    case kX86pSimdAddss:
    case kX86pSimdSubss:
    case kX86pSimdMulss:
    case kX86pSimdDivss:
    case kX86pSimdMinss:
    case kX86pSimdMaxss:
    case kX86pSimdSqrtss:
    case kX86pSimdCmpss:
    case kX86pSimdComiss:
    case kX86pSimdUcomiss:
      /* A scalar operation reads only four bytes from MEMORY. Reading sixteen
         would fault on an operand the guest placed at the end of a page --
         a fault the guest never takes. */
      src_bytes = (s->kind == kX86pOperandMem) ? 4u : w;
      break;
    default:
      break;
    }

    if (!read_vec(&c, d, dst_bytes, &a) || !read_vec(&c, s, src_bytes, &b)) {
      return finish(&c, fault_addr);
    }
    a.bytes = dst_bytes;
    b.bytes = (src_bytes < dst_bytes) ? dst_bytes : src_bytes;

    if (x86p_simd_int(op, &a, &b, imm, &r)) {
      (void)write_vec(&c, d, r.bytes, &r);
      return finish(&c, fault_addr);
    }
    if (x86p_simd_float(op, &a, &b, imm, &r, &cpu->flags)) {
      if (op == kX86pSimdComiss || op == kX86pSimdUcomiss) {
        return kX86pSimdOk; /* writes flags only */
      }
      (void)write_vec(&c, d, r.bytes, &r);
      return finish(&c, fault_addr);
    }
  }

  /* Named, reached, and not implemented by either lane module: that is a
     defect in this file rather than in the guest, and it is reported. */
  return kX86pSimdUnsupported;
}

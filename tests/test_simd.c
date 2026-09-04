/*
 * test_simd.c -- MMX and SSE, checked against the silicon.
 *
 * These are lane loops, and lane loops are wrong in ways that read correctly.
 * Saturation clamps to the wrong bound. An interleave takes both halves from
 * the same operand. A packed shift masks its count the way SHL does, turning
 * "clear this register" into a no-op. PMULHW uses the unsigned high half,
 * which is right for every value with the top bit clear. None of those look
 * like mistakes on the page, and a hand-written expectation table would
 * enshrine whichever one the author believed.
 *
 * So this EXECUTES each instruction on the host CPU. The register pair is
 * loaded from a buffer, the instruction under test runs on it, and the
 * destination is stored back -- then the same inputs go through
 * x86p_simd_execute and the 128 or 64 bits are compared byte for byte.
 *
 * INPUTS ARE CHOSEN TO CROSS THE BOUNDARIES THE OPERATIONS HAVE. A sweep of
 * small positive numbers agrees under every wrong saturation rule, every wrong
 * signedness, and every wrong shift count, so the vectors below include
 * extremes at each element width, alternating signs, and shift counts at, one
 * below and one above every lane width.
 *
 * On a non-x86 host it says loudly that no oracle ran rather than passing.
 */
#include "cpu.h"
#include "decode.h"
#include "simd.h"
#include "x87.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__x86_64__)
#define HAVE_ORACLE 1
#else
#define HAVE_ORACLE 0
#endif

#if HAVE_ORACLE
static unsigned long g_checks;
static unsigned long g_failed;
static unsigned long g_oracle_runs;
#endif

/* How the instruction under test names its operands. */
typedef enum Form {
  kFormMmx,    /* mm0, mm1 */
  kFormXmm,    /* xmm0, xmm1 */
  kFormMmxImm, /* mm0, mm1, imm8 */
  kFormXmmImm  /* xmm0, xmm1, imm8 */
} Form;

typedef struct Case {
  const char *name;
  X86pSimdOp op;
  Form form;
  const uint8_t *bytes; /* the instruction, with mm0/xmm0 as destination */
  unsigned len;         /* not counting an immediate, which is appended */
} Case;

#define INSN(id, ...) static const uint8_t id[] = {__VA_ARGS__};

/* MMX: 0F xx C1 -- reg field 0 (mm0), rm field 1 (mm1). */
INSN(k_pand, 0x0Fu, 0xDBu, 0xC1u)
INSN(k_pandn, 0x0Fu, 0xDFu, 0xC1u)
INSN(k_por, 0x0Fu, 0xEBu, 0xC1u)
INSN(k_pxor, 0x0Fu, 0xEFu, 0xC1u)
INSN(k_paddb, 0x0Fu, 0xFCu, 0xC1u)
INSN(k_paddw, 0x0Fu, 0xFDu, 0xC1u)
INSN(k_paddd, 0x0Fu, 0xFEu, 0xC1u)
INSN(k_paddsb, 0x0Fu, 0xECu, 0xC1u)
INSN(k_paddsw, 0x0Fu, 0xEDu, 0xC1u)
INSN(k_paddusb, 0x0Fu, 0xDCu, 0xC1u)
INSN(k_paddusw, 0x0Fu, 0xDDu, 0xC1u)
INSN(k_psubb, 0x0Fu, 0xF8u, 0xC1u)
INSN(k_psubw, 0x0Fu, 0xF9u, 0xC1u)
INSN(k_psubd, 0x0Fu, 0xFAu, 0xC1u)
INSN(k_psubsb, 0x0Fu, 0xE8u, 0xC1u)
INSN(k_psubsw, 0x0Fu, 0xE9u, 0xC1u)
INSN(k_psubusb, 0x0Fu, 0xD8u, 0xC1u)
INSN(k_psubusw, 0x0Fu, 0xD9u, 0xC1u)
INSN(k_pcmpeqb, 0x0Fu, 0x74u, 0xC1u)
INSN(k_pcmpeqw, 0x0Fu, 0x75u, 0xC1u)
INSN(k_pcmpeqd, 0x0Fu, 0x76u, 0xC1u)
INSN(k_pcmpgtb, 0x0Fu, 0x64u, 0xC1u)
INSN(k_pcmpgtw, 0x0Fu, 0x65u, 0xC1u)
INSN(k_pcmpgtd, 0x0Fu, 0x66u, 0xC1u)
INSN(k_punpcklbw, 0x0Fu, 0x60u, 0xC1u)
INSN(k_punpcklwd, 0x0Fu, 0x61u, 0xC1u)
INSN(k_punpckldq, 0x0Fu, 0x62u, 0xC1u)
INSN(k_punpckhbw, 0x0Fu, 0x68u, 0xC1u)
INSN(k_punpckhwd, 0x0Fu, 0x69u, 0xC1u)
INSN(k_punpckhdq, 0x0Fu, 0x6Au, 0xC1u)
INSN(k_packuswb, 0x0Fu, 0x67u, 0xC1u)
INSN(k_packsswb, 0x0Fu, 0x63u, 0xC1u)
INSN(k_packssdw, 0x0Fu, 0x6Bu, 0xC1u)
INSN(k_psllw, 0x0Fu, 0xF1u, 0xC1u)
INSN(k_pslld, 0x0Fu, 0xF2u, 0xC1u)
INSN(k_psllq, 0x0Fu, 0xF3u, 0xC1u)
INSN(k_psrlw, 0x0Fu, 0xD1u, 0xC1u)
INSN(k_psrld, 0x0Fu, 0xD2u, 0xC1u)
INSN(k_psrlq, 0x0Fu, 0xD3u, 0xC1u)
INSN(k_psraw, 0x0Fu, 0xE1u, 0xC1u)
INSN(k_psrad, 0x0Fu, 0xE2u, 0xC1u)
INSN(k_pmullw, 0x0Fu, 0xD5u, 0xC1u)
INSN(k_pmulhw, 0x0Fu, 0xE5u, 0xC1u)
INSN(k_pmulhuw, 0x0Fu, 0xE4u, 0xC1u)
INSN(k_pmaddwd, 0x0Fu, 0xF5u, 0xC1u)
INSN(k_pavgb, 0x0Fu, 0xE0u, 0xC1u)
INSN(k_pavgw, 0x0Fu, 0xE3u, 0xC1u)
INSN(k_pminub, 0x0Fu, 0xDAu, 0xC1u)
INSN(k_pmaxub, 0x0Fu, 0xDEu, 0xC1u)
INSN(k_pminsw, 0x0Fu, 0xEAu, 0xC1u)
INSN(k_pmaxsw, 0x0Fu, 0xEEu, 0xC1u)
INSN(k_psadbw, 0x0Fu, 0xF6u, 0xC1u)
INSN(k_pshufw, 0x0Fu, 0x70u, 0xC1u)

/* SSE packed single: 0F xx C1 on xmm0, xmm1. */
INSN(k_addps, 0x0Fu, 0x58u, 0xC1u)
INSN(k_subps, 0x0Fu, 0x5Cu, 0xC1u)
INSN(k_mulps, 0x0Fu, 0x59u, 0xC1u)
INSN(k_divps, 0x0Fu, 0x5Eu, 0xC1u)
INSN(k_minps, 0x0Fu, 0x5Du, 0xC1u)
INSN(k_maxps, 0x0Fu, 0x5Fu, 0xC1u)
INSN(k_sqrtps, 0x0Fu, 0x51u, 0xC1u)
INSN(k_andps, 0x0Fu, 0x54u, 0xC1u)
INSN(k_andnps, 0x0Fu, 0x55u, 0xC1u)
INSN(k_orps, 0x0Fu, 0x56u, 0xC1u)
INSN(k_xorps, 0x0Fu, 0x57u, 0xC1u)
INSN(k_unpcklps, 0x0Fu, 0x14u, 0xC1u)
INSN(k_unpckhps, 0x0Fu, 0x15u, 0xC1u)
INSN(k_shufps, 0x0Fu, 0xC6u, 0xC1u)
INSN(k_cmpps, 0x0Fu, 0xC2u, 0xC1u)
/* Scalar single: F3 0F xx C1. */
INSN(k_addss, 0xF3u, 0x0Fu, 0x58u, 0xC1u)
INSN(k_subss, 0xF3u, 0x0Fu, 0x5Cu, 0xC1u)
INSN(k_mulss, 0xF3u, 0x0Fu, 0x59u, 0xC1u)
INSN(k_divss, 0xF3u, 0x0Fu, 0x5Eu, 0xC1u)
INSN(k_minss, 0xF3u, 0x0Fu, 0x5Du, 0xC1u)
INSN(k_maxss, 0xF3u, 0x0Fu, 0x5Fu, 0xC1u)
INSN(k_sqrtss, 0xF3u, 0x0Fu, 0x51u, 0xC1u)
INSN(k_movss, 0xF3u, 0x0Fu, 0x10u, 0xC1u)
INSN(k_movhlps, 0x0Fu, 0x12u, 0xC1u)
INSN(k_movlhps, 0x0Fu, 0x16u, 0xC1u)
INSN(k_movaps, 0x0Fu, 0x28u, 0xC1u)

#define C(n, o, f, b) {n, o, f, b, (unsigned)(sizeof b)}

static const Case kCases[] = {
    C("PAND", kX86pSimdPand, kFormMmx, k_pand),
    C("PANDN", kX86pSimdPandn, kFormMmx, k_pandn),
    C("POR", kX86pSimdPor, kFormMmx, k_por),
    C("PXOR", kX86pSimdPxor, kFormMmx, k_pxor),
    C("PADDB", kX86pSimdPaddb, kFormMmx, k_paddb),
    C("PADDW", kX86pSimdPaddw, kFormMmx, k_paddw),
    C("PADDD", kX86pSimdPaddd, kFormMmx, k_paddd),
    C("PADDSB", kX86pSimdPaddsb, kFormMmx, k_paddsb),
    C("PADDSW", kX86pSimdPaddsw, kFormMmx, k_paddsw),
    C("PADDUSB", kX86pSimdPaddusb, kFormMmx, k_paddusb),
    C("PADDUSW", kX86pSimdPaddusw, kFormMmx, k_paddusw),
    C("PSUBB", kX86pSimdPsubb, kFormMmx, k_psubb),
    C("PSUBW", kX86pSimdPsubw, kFormMmx, k_psubw),
    C("PSUBD", kX86pSimdPsubd, kFormMmx, k_psubd),
    C("PSUBSB", kX86pSimdPsubsb, kFormMmx, k_psubsb),
    C("PSUBSW", kX86pSimdPsubsw, kFormMmx, k_psubsw),
    C("PSUBUSB", kX86pSimdPsubusb, kFormMmx, k_psubusb),
    C("PSUBUSW", kX86pSimdPsubusw, kFormMmx, k_psubusw),
    C("PCMPEQB", kX86pSimdPcmpeqb, kFormMmx, k_pcmpeqb),
    C("PCMPEQW", kX86pSimdPcmpeqw, kFormMmx, k_pcmpeqw),
    C("PCMPEQD", kX86pSimdPcmpeqd, kFormMmx, k_pcmpeqd),
    C("PCMPGTB", kX86pSimdPcmpgtb, kFormMmx, k_pcmpgtb),
    C("PCMPGTW", kX86pSimdPcmpgtw, kFormMmx, k_pcmpgtw),
    C("PCMPGTD", kX86pSimdPcmpgtd, kFormMmx, k_pcmpgtd),
    C("PUNPCKLBW", kX86pSimdPunpcklbw, kFormMmx, k_punpcklbw),
    C("PUNPCKLWD", kX86pSimdPunpcklwd, kFormMmx, k_punpcklwd),
    C("PUNPCKLDQ", kX86pSimdPunpckldq, kFormMmx, k_punpckldq),
    C("PUNPCKHBW", kX86pSimdPunpckhbw, kFormMmx, k_punpckhbw),
    C("PUNPCKHWD", kX86pSimdPunpckhwd, kFormMmx, k_punpckhwd),
    C("PUNPCKHDQ", kX86pSimdPunpckhdq, kFormMmx, k_punpckhdq),
    C("PACKUSWB", kX86pSimdPackuswb, kFormMmx, k_packuswb),
    C("PACKSSWB", kX86pSimdPacksswb, kFormMmx, k_packsswb),
    C("PACKSSDW", kX86pSimdPackssdw, kFormMmx, k_packssdw),
    C("PSLLW", kX86pSimdPsllw, kFormMmx, k_psllw),
    C("PSLLD", kX86pSimdPslld, kFormMmx, k_pslld),
    C("PSLLQ", kX86pSimdPsllq, kFormMmx, k_psllq),
    C("PSRLW", kX86pSimdPsrlw, kFormMmx, k_psrlw),
    C("PSRLD", kX86pSimdPsrld, kFormMmx, k_psrld),
    C("PSRLQ", kX86pSimdPsrlq, kFormMmx, k_psrlq),
    C("PSRAW", kX86pSimdPsraw, kFormMmx, k_psraw),
    C("PSRAD", kX86pSimdPsrad, kFormMmx, k_psrad),
    C("PMULLW", kX86pSimdPmullw, kFormMmx, k_pmullw),
    C("PMULHW", kX86pSimdPmulhw, kFormMmx, k_pmulhw),
    C("PMULHUW", kX86pSimdPmulhuw, kFormMmx, k_pmulhuw),
    C("PMADDWD", kX86pSimdPmaddwd, kFormMmx, k_pmaddwd),
    C("PAVGB", kX86pSimdPavgb, kFormMmx, k_pavgb),
    C("PAVGW", kX86pSimdPavgw, kFormMmx, k_pavgw),
    C("PMINUB", kX86pSimdPminub, kFormMmx, k_pminub),
    C("PMAXUB", kX86pSimdPmaxub, kFormMmx, k_pmaxub),
    C("PMINSW", kX86pSimdPminsw, kFormMmx, k_pminsw),
    C("PMAXSW", kX86pSimdPmaxsw, kFormMmx, k_pmaxsw),
    C("PSADBW", kX86pSimdPsadbw, kFormMmx, k_psadbw),
    C("PSHUFW", kX86pSimdPshufw, kFormMmxImm, k_pshufw),
    C("ADDPS", kX86pSimdAddps, kFormXmm, k_addps),
    C("SUBPS", kX86pSimdSubps, kFormXmm, k_subps),
    C("MULPS", kX86pSimdMulps, kFormXmm, k_mulps),
    C("DIVPS", kX86pSimdDivps, kFormXmm, k_divps),
    C("MINPS", kX86pSimdMinps, kFormXmm, k_minps),
    C("MAXPS", kX86pSimdMaxps, kFormXmm, k_maxps),
    C("SQRTPS", kX86pSimdSqrtps, kFormXmm, k_sqrtps),
    C("ANDPS", kX86pSimdAndps, kFormXmm, k_andps),
    C("ANDNPS", kX86pSimdAndnps, kFormXmm, k_andnps),
    C("ORPS", kX86pSimdOrps, kFormXmm, k_orps),
    C("XORPS", kX86pSimdXorps, kFormXmm, k_xorps),
    C("UNPCKLPS", kX86pSimdUnpcklps, kFormXmm, k_unpcklps),
    C("UNPCKHPS", kX86pSimdUnpckhps, kFormXmm, k_unpckhps),
    C("SHUFPS", kX86pSimdShufps, kFormXmmImm, k_shufps),
    C("CMPPS", kX86pSimdCmpps, kFormXmmImm, k_cmpps),
    C("ADDSS", kX86pSimdAddss, kFormXmm, k_addss),
    C("SUBSS", kX86pSimdSubss, kFormXmm, k_subss),
    C("MULSS", kX86pSimdMulss, kFormXmm, k_mulss),
    C("DIVSS", kX86pSimdDivss, kFormXmm, k_divss),
    C("MINSS", kX86pSimdMinss, kFormXmm, k_minss),
    C("MAXSS", kX86pSimdMaxss, kFormXmm, k_maxss),
    C("SQRTSS", kX86pSimdSqrtss, kFormXmm, k_sqrtss),
    C("MOVSS", kX86pSimdMovss, kFormXmm, k_movss),
    C("MOVHLPS", kX86pSimdMovhlps, kFormXmm, k_movhlps),
    C("MOVLHPS", kX86pSimdMovlhps, kFormXmm, k_movlhps),
    C("MOVAPS", kX86pSimdMovaps, kFormXmm, k_movaps),
};

/*
 * The input vectors.
 *
 * Each pair is 32 bytes: sixteen for the destination and sixteen for the
 * source, so one table serves both the 64-bit and 128-bit forms. They are
 * chosen for what they CROSS, not for looking varied: signed extremes at byte,
 * word and dword width; values whose sums and differences leave every
 * saturation bound in both directions; float extremes including a NaN, an
 * infinity, a negative zero and a denormal; and shift counts at, just below
 * and just above 8, 16, 32 and 64.
 */
static const uint8_t kInputs[][32] = {
    {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x81, 0x7E, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x7F,
     0x01, 0x01, 0x01, 0x01, 0x7F, 0x7F, 0x7F, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80},
    {0xFF, 0x7F, 0x00, 0x80, 0x01, 0x02, 0x03, 0x04, 0x34, 0x12, 0xCD, 0xAB, 0x00, 0x00, 0xFF, 0xFF,
     0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0xC0, 0x7F,
     0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x80, 0x7F, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
     0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
     0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
     0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
     0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

#if HAVE_ORACLE
static uint8_t *g_code;

/* in[0..15] destination, in[16..31] source; out receives the destination. */
static void oracle(const Case *c, uint8_t imm, const uint8_t *in, uint8_t *out) {
  unsigned n = 0;
  int mmx = (c->form == kFormMmx || c->form == kFormMmxImm);
  int has_imm = (c->form == kFormMmxImm || c->form == kFormXmmImm);
  void (*fn)(const uint8_t *, uint8_t *);

  if (!g_code) {
    g_code = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_code == MAP_FAILED) {
      g_code = NULL;
      g_failed++;
      return;
    }
  }
  if (mmx) {
    /* movq mm0,[rdi] ; movq mm1,[rdi+16].
       Sixteen, not eight: the input table is one 16-byte destination followed
       by one 16-byte source so that a single table serves the 64- and 128-bit
       forms. Reading the source at +8 takes the top half of the DESTINATION,
       which for the identity shuffle looks like a plausible wrong answer. */
    g_code[n++] = 0x0Fu;
    g_code[n++] = 0x6Fu;
    g_code[n++] = 0x07u;
    g_code[n++] = 0x0Fu;
    g_code[n++] = 0x6Fu;
    g_code[n++] = 0x4Fu;
    g_code[n++] = 0x10u;
  } else {
    /* movups xmm0,[rdi] ; movups xmm1,[rdi+16] -- unaligned, so the caller's
       buffer needs no alignment guarantee to be part of this test. */
    g_code[n++] = 0x0Fu;
    g_code[n++] = 0x10u;
    g_code[n++] = 0x07u;
    g_code[n++] = 0x0Fu;
    g_code[n++] = 0x10u;
    g_code[n++] = 0x4Fu;
    g_code[n++] = 0x10u;
  }
  memcpy(g_code + n, c->bytes, c->len);
  n += c->len;
  if (has_imm) {
    g_code[n++] = imm;
  }
  if (mmx) {
    /* movq [rsi],mm0 ; emms */
    g_code[n++] = 0x0Fu;
    g_code[n++] = 0x7Fu;
    g_code[n++] = 0x06u;
    g_code[n++] = 0x0Fu;
    g_code[n++] = 0x77u;
  } else {
    /* movups [rsi],xmm0 */
    g_code[n++] = 0x0Fu;
    g_code[n++] = 0x11u;
    g_code[n++] = 0x06u;
  }
  g_code[n++] = 0xC3u;

  memcpy(&fn, &g_code, sizeof fn);
  memset(out, 0, 16);
  fn(in, out);
  g_oracle_runs++;
}
#endif

static void run_case(const Case *c, uint8_t imm, const uint8_t *in) {
#if HAVE_ORACLE
  uint8_t real[16];
  uint8_t got[16];
  X86pCpu cpu;
  X86pInsn insn;
  unsigned bytes = (c->form == kFormMmx || c->form == kFormMmxImm) ? 8u : 16u;
  int has_imm = (c->form == kFormMmxImm || c->form == kFormXmmImm);

  oracle(c, imm, in, real);

  memset(&insn, 0, sizeof insn);
  insn.op = (uint8_t)kX86pInsnSimd;
  insn.simd = (uint8_t)c->op;
  insn.operands = has_imm ? 3 : 2;
  insn.operand[0].kind = (uint8_t)(bytes == 8u ? kX86pOperandMmx : kX86pOperandXmm);
  insn.operand[0].reg = 0;
  insn.operand[0].size = (uint8_t)bytes;
  insn.operand[1].kind = insn.operand[0].kind;
  insn.operand[1].reg = 1;
  insn.operand[1].size = (uint8_t)bytes;
  if (has_imm) {
    insn.operand[2].kind = (uint8_t)kX86pOperandImm;
    insn.operand[2].imm = imm;
    insn.operand[2].size = 1;
  }

  x86p_cpu_reset(&cpu);
  if (bytes == 8u) {
    uint64_t a;
    uint64_t b;
    memcpy(&a, in, 8);
    memcpy(&b, in + 16, 8);
    if (!x86p_x87_mmx_write(&cpu.x87, 0, a) || !x86p_x87_mmx_write(&cpu.x87, 1, b)) {
      printf("    REFUSED %s: this host has no x87 mantissa to alias MMX onto\n", c->name);
      g_failed++;
      return;
    }
  } else {
    memcpy(cpu.xmm[0], in, 16);
    memcpy(cpu.xmm[1], in + 16, 16);
  }

  g_checks++;
  if (x86p_simd_execute(&cpu, NULL, &insn, NULL) != kX86pSimdOk) {
    g_failed++;
    printf("    FAIL %s imm=%02X: the model refused\n", c->name, imm);
    return;
  }

  memset(got, 0, sizeof got);
  if (bytes == 8u) {
    uint64_t q = 0u;
    (void)x86p_x87_mmx_read(&cpu.x87, 0, &q);
    memcpy(got, &q, 8);
  } else {
    memcpy(got, cpu.xmm[0], 16);
  }

  g_checks++;
  if (memcmp(real, got, bytes) != 0) {
    unsigned i;
    g_failed++;
    printf("    FAIL %s imm=%02X\n      cpu   =", c->name, imm);
    for (i = 0; i < bytes; i++) {
      printf(" %02X", real[i]);
    }
    printf("\n      model =");
    for (i = 0; i < bytes; i++) {
      printf(" %02X", got[i]);
    }
    printf("\n      in    =");
    for (i = 0; i < bytes; i++) {
      printf(" %02X", in[i]);
    }
    printf(" |");
    for (i = 0; i < bytes; i++) {
      printf(" %02X", in[16 + i]);
    }
    printf("\n");
  }
#else
  (void)c;
  (void)imm;
  (void)in;
#endif
}

int main(void) {
  unsigned i;
  unsigned v;
  static const uint8_t kImms[] = {0x00u, 0x1Bu, 0x93u, 0xE4u, 0x01u, 0x07u};

  printf("test MMX and SSE against the host CPU\n");
  for (i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
    for (v = 0; v < sizeof kInputs / sizeof kInputs[0]; v++) {
      unsigned m;
      for (m = 0; m < sizeof kImms / sizeof kImms[0]; m++) {
        run_case(&kCases[i], kImms[m], kInputs[v]);
        if (kCases[i].form == kFormMmx || kCases[i].form == kFormXmm) {
          break; /* the immediate is not part of the instruction */
        }
      }
    }
  }

#if HAVE_ORACLE
  if (g_oracle_runs == 0u) {
    printf("REFUSED: the host oracle never ran, so nothing here was verified\n");
    return 1;
  }
  printf(
      "%lu check(s), %lu failure(s); %lu instruction(s) executed on the host CPU\n", g_checks, g_failed, g_oracle_runs);
  return g_failed ? 1 : 0;
#else
  printf("REFUSED: this host is not x86-64, so no instruction was executed and\n"
         "nothing here was verified. MMX and SSE are UNCHECKED on this build.\n");
  return 1;
#endif
}

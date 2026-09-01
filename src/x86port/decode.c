/* decode.c -- see decode.h for why decode is borrowed and semantics are not. */
#include "decode.h"

#include <Zydis/Zydis.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * Zydis spells mnemonics in lower case; everything else in this framework (and
 * the Ghidra corpus the decoder is measured against) uses upper. Uppercasing at
 * every call site is how the two conventions come to disagree, so it happens
 * once, here, into a small cache of static storage -- the returned pointer must
 * outlive the call, and Zydis's own string is already static, so this only
 * changes the case.
 *
 * The cache is a fixed table indexed by Zydis's mnemonic enum, filled lazily.
 * Bounded, no allocation, and safe to hand out because an entry, once written,
 * never changes.
 */
#define MNEMONIC_CACHE_MAX ZYDIS_MNEMONIC_MAX_VALUE
static char g_mnemonic[MNEMONIC_CACHE_MAX + 1][32];

static const char *upper_mnemonic(ZydisMnemonic m) {
  const char *src;
  size_t i;
  if ((unsigned)m > (unsigned)MNEMONIC_CACHE_MAX) {
    return "UNKNOWN";
  }
  if (g_mnemonic[m][0] != '\0') {
    return g_mnemonic[m];
  }
  src = ZydisMnemonicGetString(m);
  if (!src) {
    return "UNKNOWN";
  }
  for (i = 0; i + 1 < sizeof g_mnemonic[0] && src[i]; i++) {
    g_mnemonic[m][i] = (char)toupper((unsigned char)src[i]);
  }
  g_mnemonic[m][i] = '\0';
  return g_mnemonic[m];
}

uint32_t x86p_decode(const uint8_t *bytes, size_t len, X86pInsn *out) {
  static ZydisDecoder decoder;
  static int decoder_ready;
  ZydisDecodedInstruction insn;

  if (!bytes || !out || len == 0) {
    return 0;
  }
  if (len > X86P_MAX_INSN_LEN) {
    len = X86P_MAX_INSN_LEN;
  }

  if (!decoder_ready) {
    /* 32-bit protected mode with a 32-bit stack: the guest is a Win32
       process, and there is no 16-bit or long-mode code in it. A decoder
       configured for the wrong mode reads the same bytes as different
       instructions rather than failing, so this is stated once and never
       parameterised by a caller. */
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32))) {
      return 0;
    }
    decoder_ready = 1;
  }

  if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, ZYAN_NULL, (const void *)bytes, len, &insn))) {
    return 0;
  }
  if (insn.length == 0) { /* cannot happen on success; a zero would loop a caller forever */
    return 0;
  }

  out->length = insn.length;
  out->mnemonic = upper_mnemonic(insn.mnemonic);
  out->operands = insn.operand_count_visible;
  return insn.length;
}

const char *x86p_decoder_id(void) {
  static char id[64];
  if (id[0] == '\0') {
    snprintf(id,
             sizeof id,
             "zydis %d.%d.%d",
             (int)((ZYDIS_VERSION >> 48) & 0xFFFF),
             (int)((ZYDIS_VERSION >> 32) & 0xFFFF),
             (int)((ZYDIS_VERSION >> 16) & 0xFFFF));
  }
  return id;
}

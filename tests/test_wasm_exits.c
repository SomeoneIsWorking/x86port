/*
 * test_wasm_exits.c -- the exit census, asserted shape by shape.
 *
 * WHY THIS SUITE EXISTS. The census these counters feed is what ranks block
 * chaining: how many of the places a translated block can leave to are
 * addresses the translator already holds, and how many of those are loop
 * backedges paying a dispatch per iteration. Nothing else reads them, so a
 * lowering that stopped counting would break no test and no build, and the
 * census would quietly report a binary with no branches in it.
 *
 * WHY A BLOCK HAS MORE THAN ONE EXIT. A conditional branch does not end a
 * block in this backend: its taken path is an exit emitted inside the body and
 * the run carries on past it. A suite that only checked single-exit blocks
 * would pass against a lowering that counted terminators alone -- which is
 * exactly the defect this census was written to replace.
 *
 * WHY EVERY CASE ASSERTS THE ZEROES TOO. `backward` is the number the ranking
 * turns on, so a forward branch asserting `backward == 0` is as much of the
 * test as a backward one asserting 1. A counter that only ever went up would
 * satisfy half of these.
 */
#include "decode.h"
#include "jit_wasm_lower.h"

#include <stdio.h>
#include <string.h>

enum { kGuestLo = 0x400000u, kArenaSize = 4096u, kModuleSize = 65536u };

static uint8_t g_arena[kArenaSize];
static uint8_t g_module[kModuleSize];
static unsigned g_checks;
static unsigned g_failures;
static const char *g_case;

static void check(int value, const char *message) {
  g_checks++;
  if (!value) {
    g_failures++;
    printf("FAIL: %s: %s\n", g_case, message);
  }
}

/*
 * The ADDRESSES, not just how many there are. A count of known successors
 * cannot be compared against where a run actually went, and the runtime chain
 * census does exactly that comparison, so the addresses the block recorded are
 * part of this contract.
 */
static int records_target(const X86pJitBlock *b, uint32_t target) {
  unsigned i;
  for (i = 0u; i < b->static_target_count; i++) {
    if (b->static_targets[i] == target) {
      return 1;
    }
  }
  return 0;
}

/*
 * Lower `code` as a block at kGuestLo. Unlike the conditions suite, NOTHING is
 * appended: every case here names its own terminator, because what terminates
 * the block and where it goes is the thing under test.
 */
static int lower(const uint8_t *code, unsigned size, X86pJitBlock *out) {
  X86pMem mem = {0};
  X86pWasmModule module;
  X86pWasmPlan plan = {0};
  char reason[256] = {0};

  memset(g_arena, 0, sizeof g_arena);
  memcpy(g_arena, code, size);
  mem.host = g_arena;
  mem.lo = kGuestLo;
  mem.size = sizeof g_arena;
  plan.base = 0;
  plan.lo = kGuestLo;
  plan.size = sizeof g_arena;
  x86p_wasm_module_init(&module, g_module, sizeof g_module, 1u);
  if (x86p_wasm_lower_block(&module, &mem, &plan, kGuestLo, NULL, NULL, NULL, out, reason, sizeof reason) !=
      kX86pJitOk) {
    printf("FAIL: %s: lowering refused: %s\n", g_case, reason);
    g_failures++;
    return 0;
  }
  return 1;
}

static void a_jump_to_itself_is_every_tier(void) {
  /* JMP $ -- the target is the instruction itself, which is also the block's
     first address, so this one shape must be counted in all three subsets. If
     `to_entry` were computed with an off-by-one against the NEXT address this
     is the case that catches it. */
  static const uint8_t code[] = {0xEB, 0xFE};
  X86pJitBlock b;

  g_case = "JMP $";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 1u, "did not count exactly one exit");
  check(b.exits_static == 1u, "a relative JMP was not counted as a known successor");
  check(b.exits_backward == 1u, "a jump to itself was not counted as backward");
  check(b.exits_loop == 1u, "a jump to itself was not counted as inside its own block");
  check(b.exits_self == 1u, "a jump to the block's own entry was not counted as such");
  check(b.static_target_count == 1u, "the one known successor's address was not recorded");
  check(records_target(&b, kGuestLo), "the recorded successor is not the block's own entry");
}

static void a_forward_jump_is_no_loop(void) {
  /* JMP +2, over the two bytes after it. Known successor, but forward: nothing
     in the loop tiers may count it. */
  static const uint8_t code[] = {0xEB, 0x02, 0x90, 0x90};
  X86pJitBlock b;

  g_case = "JMP forward";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 1u, "did not count exactly one exit");
  check(b.exits_static == 1u, "a relative JMP was not counted as a known successor");
  check(b.exits_backward == 0u, "a forward jump was counted as a loop backedge");
  check(b.exits_loop == 0u, "a forward jump was counted as inside its own block");
  check(b.exits_self == 0u, "a forward jump was counted as reaching the block's entry");
  check(b.static_target_count == 1u, "the one known successor's address was not recorded");
  check(records_target(&b, kGuestLo + 4u), "the recorded successor is not the jump's target");
}

static void a_conditional_does_not_end_the_block(void) {
  /*
   * 400000  cmp eax, ebx
   * 400002  jz  $-2        ; back to the CMP: a loop, and NOT the terminator
   * 400004  jmp $          ; what actually ends the block
   *
   * Two exits from one block. The Jcc's taken path is emitted inline and the
   * run continues to the JMP -- so a census that classified the block's last
   * instruction would see the JMP and miss the loop entirely. That is the
   * defect this suite exists to hold shut.
   */
  static const uint8_t code[] = {0x39, 0xD8, 0x74, 0xFC, 0xEB, 0xFE};
  X86pJitBlock b;

  g_case = "CMP; JZ backward; JMP $";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 2u, "a conditional branch did not add an exit of its own");
  check(b.exits_static == 2u, "both exits name an immediate and both should be known");
  /* The JZ goes back to the CMP, the JMP to itself: both backward, both inside
     the block, but only the JMP's own address is the block's entry... and the
     CMP's is too, since the block starts there. */
  check(b.exits_backward == 2u, "a backward Jcc inside the body was not counted");
  check(b.exits_loop == 2u, "an exit into the block's own code was not counted");
  check(b.exits_self == 1u, "the JZ targets the entry and the JMP does not; expected one");
  check(b.static_target_count == 2u, "two distinct known successors were not recorded");
  check(records_target(&b, kGuestLo), "the JZ's target was not recorded");
  check(records_target(&b, kGuestLo + 4u), "the JMP's own address was not recorded");
}

static void a_forward_conditional_adds_a_known_successor(void) {
  /*
   * 400000  jz  $+2        ; forward, over the NOPs
   * 400002  nop
   * 400003  nop
   * 400004  jmp $
   *
   * Two known successors, no loop. This is the other answer to the case above:
   * the same two-exit shape with `backward` at zero.
   */
  static const uint8_t code[] = {0x74, 0x02, 0x90, 0x90, 0xEB, 0xFE};
  X86pJitBlock b;

  g_case = "JZ forward; JMP $";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 2u, "a conditional branch did not add an exit of its own");
  check(b.exits_static == 2u, "both exits name an immediate and both should be known");
  check(b.exits_backward == 1u, "expected only the trailing JMP $ to be backward");
  check(b.exits_self == 0u, "no exit here names the block's first address");
}

static void a_return_is_not_a_known_successor(void) {
  /* RET reads its target from the guest stack, so it is the negative for
     `exits_static`: an exit that exists and that chaining can never reach. */
  static const uint8_t code[] = {0xC3};
  X86pJitBlock b;

  g_case = "RET";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 1u, "a RET did not count as an exit");
  check(b.exits_static == 0u, "a RET's successor was claimed to be known at translation time");
  check(b.static_target_count == 0u, "an address was recorded for a successor nobody knows");
  check(b.exits_backward == 0u, "a RET was counted as a loop backedge");
}

static void an_indirect_jump_is_not_a_known_successor(void) {
  /* JMP EAX -- the other computed form, so `exits_static == 0` here is not
     carried by RET's stack read alone. */
  static const uint8_t code[] = {0xFF, 0xE0};
  X86pJitBlock b;

  g_case = "JMP EAX";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 1u, "an indirect JMP did not count as an exit");
  check(b.exits_static == 0u, "an indirect JMP's successor was claimed to be known");
}

static void a_call_completes_rather_than_refuses(void) {
  /* A relative CALL ends the block at a known address -- the callee -- so it
     is a successor chaining could reach, and it is forward here. */
  static const uint8_t code[] = {0xE8, 0x00, 0x00, 0x00, 0x00};
  X86pJitBlock b;

  g_case = "CALL rel32";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 1u, "a relative CALL did not count as an exit");
  check(b.exits_static == 1u, "a relative CALL's target was not counted as known");
  check(b.exits_backward == 0u, "a CALL to the next address was counted as backward");
}

static void a_refusal_is_not_an_exit(void) {
  /*
   * An instruction with no lowering ends the block with kX86pJitExitUnsupported
   * and the guest EIP of the refusal. That writes an EIP and returns exactly as
   * a real exit does, and it is NOT somewhere the run continues from: counting
   * it would inflate the very ratio that decides whether chaining is worth
   * building. NOP then the refusal, so the block has lowered something and is
   * not rejected outright.
   */
  static const uint8_t code[] = {0x90, 0x0F, 0x0B}; /* NOP; UD2 */
  X86pJitBlock b;

  g_case = "NOP; UD2";
  if (!lower(code, sizeof code, &b)) {
    return;
  }
  check(b.exits == 0u, "a refusal to translate was counted as a place the block goes");
}

int main(void) {
  a_jump_to_itself_is_every_tier();
  a_forward_jump_is_no_loop();
  a_conditional_does_not_end_the_block();
  a_forward_conditional_adds_a_known_successor();
  a_return_is_not_a_known_successor();
  an_indirect_jump_is_not_a_known_successor();
  a_call_completes_rather_than_refuses();
  a_refusal_is_not_an_exit();

  printf("test_wasm_exits: %u check(s), %u failure(s)\n", g_checks, g_failures);
  if (g_checks == 0u) {
    printf("FAIL: no check ran, so this suite proved nothing\n");
    return 1;
  }
  return g_failures ? 1 : 0;
}

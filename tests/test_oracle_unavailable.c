/* Compile each actual hardware suite's unavailable branch on every test host.
 * This proves exit classification, not native ARM64 instruction execution. */
#define X86P_TEST_ORACLE_UNAVAILABLE 1
#define main x86p_unavailable_oracle_main
#include X86P_ORACLE_SOURCE
#undef main

int main(void) {
  int result = x86p_unavailable_oracle_main();
  if (result != 77) {
    printf("FAIL: unavailable hardware oracle returned %d, expected explicit skip 77\n", result);
    return 1;
  }
  printf("PASS: compiled unavailable-oracle branch reports skip, not semantic success\n");
  return 0;
}

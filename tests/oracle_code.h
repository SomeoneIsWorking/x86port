#ifndef X86PORT_TEST_ORACLE_CODE_H
#define X86PORT_TEST_ORACLE_CODE_H

#include "code_memory.h"
#include <stdio.h>
#include <stdlib.h>

/* Silicon fixtures use the shipping W^X owner and never execute a write view. */
static inline unsigned char *oracle_code_write(JcCodeRegion *region) {
  char reason[192] = {0};
  JcCodeStatus status =
      region->write ? jc_code_begin_write(region) : jc_code_region_create(4096u, region, reason, sizeof reason);
  if (status != kJcCodeOk) {
    printf("REFUSED: oracle code write: %s (%s)\n", jc_code_status_name(status), reason);
    exit(1);
  }
  return region->write;
}

static inline void *oracle_code_publish(JcCodeRegion *region, size_t size) {
  JcCodeStatus status = jc_code_publish(region, size);
  if (status != kJcCodeOk) {
    printf("REFUSED: oracle code publication: %s\n", jc_code_status_name(status));
    exit(1);
  }
  return region->exec;
}

#endif

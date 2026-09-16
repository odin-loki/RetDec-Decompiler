/**
 * @file scripts/ci/x86_cc_oracle.c
 * @brief Executes every SETcc against every combination of the six flags.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of FLAG-01. See scripts/ci/check_x86_flags.sh.
 */

#include <stdio.h>
#include <stdint.h>

// EFLAGS bit positions for CF PF AF ZF SF OF.
static const int BIT[6] = {0, 2, 4, 6, 7, 11};

#define SET(name, insn)                                                       \
  static unsigned name(uint64_t fl) {                                         \
    unsigned char r = 0;                                                      \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\torq %1, (%%rsp)\n\tpopfq\n\t" \
                     insn " %0"                                               \
                     : "=r"(r) : "r"(fl) : "cc");                             \
    return r;                                                                 \
  }

SET(s_o,   "seto")   SET(s_no,  "setno")  SET(s_b,   "setb")   SET(s_ae,  "setae")
SET(s_e,   "sete")   SET(s_ne,  "setne")  SET(s_be,  "setbe")  SET(s_a,   "seta")
SET(s_s,   "sets")   SET(s_ns,  "setns")  SET(s_p,   "setp")   SET(s_np,  "setnp")
SET(s_l,   "setl")   SET(s_ge,  "setge")  SET(s_le,  "setle")  SET(s_g,   "setg")

typedef unsigned (*Fn)(uint64_t);
static struct { const char* name; Fn fn; } TBL[] = {
  {"seto",s_o},{"setno",s_no},{"setb",s_b},{"setae",s_ae},
  {"sete",s_e},{"setne",s_ne},{"setbe",s_be},{"seta",s_a},
  {"sets",s_s},{"setns",s_ns},{"setp",s_p},{"setnp",s_np},
  {"setl",s_l},{"setge",s_ge},{"setle",s_le},{"setg",s_g},
};

int main(void)
{
  printf("# name cf pf af zf sf of result\n");
  for (size_t k = 0; k < sizeof TBL / sizeof TBL[0]; ++k) {
    for (unsigned m = 0; m < 64; ++m) {
      uint64_t fl = 0;
      unsigned b[6];
      for (int i = 0; i < 6; ++i) { b[i] = (m >> i) & 1; if (b[i]) fl |= 1ULL << BIT[i]; }
      unsigned r = TBL[k].fn(fl);
      printf("%s|%u|%u|%u|%u|%u|%u|%u\n", TBL[k].name, b[0],b[1],b[2],b[3],b[4],b[5], r & 1);
    }
  }
  return 0;
}

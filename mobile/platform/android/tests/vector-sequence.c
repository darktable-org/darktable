/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests Android ARM64 instruction execution, not RAW processing or engine
 * correctness. This checker is deliberately compiled at -O0; the separately
 * assembled instruction sequence remains unchanged. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void vector_sequence(float *table);

enum { COUNT = 65536, GUARD_WORDS = 16 };

int main(void)
{
  setbuf(stdout, NULL);
  uint32_t *allocation = malloc((COUNT + 2 * GUARD_WORDS) * sizeof(uint32_t));
  if(!allocation) return 2;
  for(int i = 0; i < COUNT + 2 * GUARD_WORDS; i++) allocation[i] = 0x7fabcd12;
  float *table = (float *)(allocation + GUARD_WORDS);
  printf("start exact-vector-sequence table=%p end=%p noOpenMP noLibm\n",
         (void *)table, (void *)(table + COUNT));
  vector_sequence(table);
  for(int i = 0; i < GUARD_WORDS; i++)
  {
    if(allocation[i] != 0x7fabcd12 || allocation[COUNT + GUARD_WORDS + i] != 0x7fabcd12)
    {
      printf("CANARY FAIL %d\n", i);
      free(allocation);
      return 3;
    }
  }
  for(int i = 0; i < COUNT; i++)
  {
    if(table[i] != (100.0f / COUNT) * (float)i)
    {
      printf("VALUE FAIL %d %g\n", i, (double)table[i]);
      free(allocation);
      return 4;
    }
  }
  printf("done exact-vector-sequence first=%g mid=%g last=%g "
         "canaries=preserved all65536values=exact\n",
         (double)table[0], (double)table[COUNT / 2], (double)table[COUNT - 1]);
  free(allocation);
  return 0;
}

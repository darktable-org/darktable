/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common/zonemap.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return EXIT_FAILURE; } } while(0)
int main(void)
{
  float automatic[25], map[24];
  for(int index = 0; index < 25; index++) automatic[index] = -1.f;
  for(int count = 4; count <= 24; count++)
  {
    dt_zonesystem_calculate_zonemap(automatic, count, map);
    CHECK(map[0] == 0.f && map[count - 1] == 1.f);
    for(int index = 1; index < count; index++)
      CHECK(isfinite(map[index]) && map[index] > map[index - 1]);
  }
  /* Explicit control points retain their values while each automatic run
   * receives the original evenly divided interpolation. */
  automatic[2] = 0.125f;
  automatic[4] = 0.75f;
  dt_zonesystem_calculate_zonemap(automatic, 6, map);
  CHECK(map[0] == 0.f && map[1] == 0.0625f && map[2] == 0.125f);
  CHECK(map[3] == 0.4375f && map[4] == 0.75f && map[5] == 1.f);
  return EXIT_SUCCESS;
}

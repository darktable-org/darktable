/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Shared original zone-system interpolation. The desktop module and mobile
 * controls use this same implementation to resolve automatic (-1) boundaries.
 * Callers validate size, storage, ordering and finite explicit values. */
static inline void dt_zonesystem_calculate_zonemap(const float *zone, const int size, float *zonemap)
{
  int steps = 0;
  int pk = 0;

  for(int k = 0; k < size; k++)
  {
    if((k > 0 && k < size - 1) && zone[k] == -1)
      steps++;
    else
    {
      /* First and last boundaries are fixed at zero and one. */
      zonemap[k] = k == 0 ? 0.0 : k == (size - 1) ? 1.0 : zone[k];
      for(int l = 1; l <= steps; l++)
        zonemap[pk + l] = zonemap[pk] + (((zonemap[k] - zonemap[pk]) / (steps + 1)) * l);
      pk = k;
      steps = 0;
    }
  }
}

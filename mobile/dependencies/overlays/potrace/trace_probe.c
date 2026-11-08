/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <potracelib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A binary rectangle must produce four geometric corners, irrespective of the
 * cyclic starting point chosen by the trace algorithm. Also check an empty
 * bitmap and that tracing never changes caller-owned raster words. */
static int trace_fixture(void)
{
  const int bits = 8 * (int)sizeof(potrace_word);
  potrace_word raster[9] = { 0 }, unchanged[9];
  potrace_bitmap_t bitmap = { 10, 9, 1, raster };
  potrace_param_t *parameters = potrace_param_default();
  potrace_state_t *state = NULL;
  int result = 1;
  if(!parameters || !strstr(potrace_version(), "1.16")) goto cleanup;
  parameters->turdsize = 0;
  parameters->alphamax = 0;
  parameters->opticurve = 0;
  state = potrace_trace(parameters, &bitmap);
  if(!state || state->status != POTRACE_STATUS_OK || state->plist) goto cleanup;
  potrace_state_free(state); state = NULL;
  for(int y = 2; y < 6; y++)
    for(int x = 2; x < 7; x++) raster[y] |= (potrace_word)1 << (bits - 1 - x);
  memcpy(unchanged, raster, sizeof(raster));
  state = potrace_trace(parameters, &bitmap);
  if(!state || state->status != POTRACE_STATUS_OK || !state->plist
     || memcmp(unchanged, raster, sizeof(raster))) goto cleanup;
  const potrace_path_t *path = state->plist;
  if(path->next || path->childlist || path->sign != '+' || path->area != 20 || path->curve.n != 4) goto cleanup;
  unsigned corners = 0;
  for(int k = 0; k < path->curve.n; k++)
  {
    if(path->curve.tag[k] != POTRACE_CORNER) goto cleanup;
    /* c[k][0] is unused for CORNER and is deliberately not inspected. */
    for(int control = 1; control < 3; control++)
    {
      const potrace_dpoint_t p = path->curve.c[k][control];
      if(!isfinite(p.x) || !isfinite(p.y) || p.x < 2 - 1e-9 || p.x > 7 + 1e-9
         || p.y < 2 - 1e-9 || p.y > 6 + 1e-9) goto cleanup;
    }
    const potrace_dpoint_t vertex = path->curve.c[k][1];
    if(fabs(vertex.x - 2) < 1e-9 && fabs(vertex.y - 2) < 1e-9) corners |= 1;
    if(fabs(vertex.x - 7) < 1e-9 && fabs(vertex.y - 2) < 1e-9) corners |= 2;
    if(fabs(vertex.x - 7) < 1e-9 && fabs(vertex.y - 6) < 1e-9) corners |= 4;
    if(fabs(vertex.x - 2) < 1e-9 && fabs(vertex.y - 6) < 1e-9) corners |= 8;
  }
  if(corners != 15) goto cleanup;
  result = 0;
cleanup:
  potrace_state_free(state);
  potrace_param_free(parameters);
  return result;
}

#ifdef POTRACE_ANDROID_LINK_PROBE
__attribute__((visibility("default"))) int potrace_android_trace_probe(void)
{
  return trace_fixture();
}
#else
int main(void)
{
  const int result = trace_fixture();
  if(result) fprintf(stderr, "Potrace rectangle/empty bitmap fixture failed\n");
  return result;
}
#endif

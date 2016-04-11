#pragma once

#include <stdint.h>
#include <assert.h>

// apply and undo a tone curve (L channel only),
// created from the 24 grey input patches from the it8.


typedef struct tonecurve_t
{
  double x[100];       // input L positions, max 100, assumed to be strictly monotonic x[i+1] > x[i]
  double y[100];       // output L values, max 100, assumed to be monotonic y[i+1] >= y[i]
  int32_t num;         // number of values, max 100
}
tonecurve_t;

static inline void tonecurve_create(
    tonecurve_t *c,
    const double *Lin,
    const double *Lout,
    const int32_t num)
{
  c->num = num;
  for(int i=0;i<num;i++)
  {
    c->x[i] = Lin[i];
    c->y[i] = Lout[i];
  }
}

static inline double _tonecurve_apply(
    const double *x,
    const double *y,
    const int32_t num,
    const double L)
{
  if(L <= 0.0 || L >= 100.0) return L;
  uint32_t min = 0, max = num;
  uint32_t t = max/2;
  while (t != min)
  {
    if(x[t] <= L) min = t;
    else max = t;
    t = (min + max)/2;
  }
  assert(t<num);
  // last step: decide between min and max one more time (min is rounding default),
  // but consider that if max is still out of bounds, it's invalid.
  // (L == 1.0 and a x[0]=1, num=1 would break otherwise)
  if(max < num && x[max] <= L) t = max;
  const double f = (x[t+1] - x[t] > 1e-6f) ? (L - x[t]) / (x[t+1] - x[t]) : 1.0f;
  if(t == num-1) return y[t];
  assert(x[t]   <= L);
  assert(x[t+1] >= L);
  return y[t+1] * f + y[t] * (1.0f-f);
}

static inline double tonecurve_apply(
    const tonecurve_t *c,
    const double L)
{
  return _tonecurve_apply(c->x, c->y, c->num, L);
}

static inline double tonecurve_unapply(
    const tonecurve_t *c,
    const double L)
{
  return _tonecurve_apply(c->y, c->x, c->num, L);
}

static inline void
hexify(uint8_t* out, const uint8_t* in, size_t len)
{
  static const char hex[] = "0123456789abcdef";
  for(int i=0; i<len; i++)
  {
    out[2*i  ] = hex[in[i] >> 4];
    out[2*i+1] = hex[in[i] & 15];
  }
  out[2*len] = '\0';
}

static inline void tonecurve_dump_preset(
    const tonecurve_t *c,
    const char *name)
{
  // hardcoded params v4 from tonecurve:
  typedef struct dt_iop_tonecurve_node_t
  {
    float x;
    float y;
  }
  dt_iop_tonecurve_node_t;
  typedef struct dt_iop_tonecurve_params_t
  {
    dt_iop_tonecurve_node_t tonecurve[3][20]; // three curves (L, a, b) with max number
    // of nodes
    int tonecurve_nodes[3];
    int tonecurve_type[3];
    int tonecurve_autoscale_ab;
    int tonecurve_preset;
    int tonecurve_unbound_ab;
  }
  dt_iop_tonecurve_params_t;

  dt_iop_tonecurve_params_t params;
  memset(&params, 0, sizeof(params));
  params.tonecurve_autoscale_ab = 0; // manual
  params.tonecurve_type[0] = 2; // MONOTONE_HERMITE
  params.tonecurve_type[1] = 2; // MONOTONE_HERMITE
  params.tonecurve_type[2] = 2; // MONOTONE_HERMITE
  params.tonecurve_nodes[0] = 20;
  params.tonecurve_nodes[1] = 2;
  params.tonecurve_nodes[2] = 2;
  params.tonecurve[1][0].x = 0.0f;
  params.tonecurve[1][0].y = 0.0f;
  params.tonecurve[1][1].x = 1.0f;
  params.tonecurve[1][1].y = 1.0f;
  params.tonecurve[2][0].x = 0.0f;
  params.tonecurve[2][0].y = 0.0f;
  params.tonecurve[2][1].x = 1.0f;
  params.tonecurve[2][1].y = 1.0f;

  char filename[1024];
  snprintf(filename, sizeof(filename), "tonecurve-%s.sh", name);
  FILE *f = fopen(filename, "wb");
  if(!f) return;
  fprintf(f, "#!/bin/sh\n");
  fprintf(f, "# to test your new tonecurve, copy/paste the following line into your shell.\n");
  fprintf(f, "# note that it is a smart idea to backup your database before messing with it on this level.\n\n");
  // for(int rev=0;rev<2;rev++)
  for(int rev=0;rev<1;rev++) // don't print reverse curve
  {
    for(int k=0;k<20;k++)
    {
      const double x = (k/19.0)*(k/19.0);
      params.tonecurve[0][k].x = x;
      params.tonecurve[0][k].y = rev ?
        tonecurve_unapply(c, 100.0*x)/100.0:
        tonecurve_apply(c, 100.0*x)/100.0;
    }

    if(rev) snprintf(filename, sizeof(filename), "%s reverse", name);
    else    snprintf(filename, sizeof(filename), "%s", name);
    uint8_t encoded[2048];
    hexify(encoded, (uint8_t*)&params, sizeof(params));
    fprintf(f, "echo \"INSERT OR REPLACE INTO presets (name,description,operation,op_version,op_params,enabled,blendop_params,blendop_version,multi_priority,multi_name,model,maker,lens,iso_min,iso_max,exposure_min,exposure_max,aperture_min,aperture_max,focal_length_min,focal_length_max,writeprotect,autoapply,filter,def,format) VALUES('%s','','tonecurve',%d,X'%s',1,X'00000000180000000000C842000000000000000000000000000000000000000000000000000000000000000000000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F',7,0,'','%%','%%','%%',0.0,51200.0,0.0,10000000.0,0.0,100000000.0,0.0,1000.0,0,0,0,0,2);\" | sqlite3 ~/.config/darktable/library.db\n",
        filename,
        4, encoded);
  }
  fclose(f);
}


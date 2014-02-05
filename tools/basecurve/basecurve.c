/*
    This file is part of darktable,
    copyright (c) 2013 Thorsten
                  2013 Johaness Hanika
                  2014 Edouard Gomez

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "../../src/common/curve_tools.c"

// copied from iop/basecurve.c:
typedef struct dt_iop_basecurve_node_t
{
  float x;
  float y;
}
dt_iop_basecurve_node_t;
#define MAXNODES 20
typedef struct dt_iop_basecurve_params_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
}
dt_iop_basecurve_params_t;

// copied from exif.cc:
// encode binary blob into text:
void text_encode (const unsigned char *input, char *output, const int len)
{
  const char hex[16] =
  {
    '0', '1', '2', '3', '4', '5', '6', '7', '8',
    '9', 'a', 'b', 'c', 'd', 'e', 'f'
  };
  for(int i=0; i<len; i++)
  {
    const int hi = input[i] >> 4;
    const int lo = input[i] & 15;
    output[2*i]   = hex[hi];
    output[2*i+1] = hex[lo];
  }
  output[2*len] = '\0';
}


static uint16_t*
read_ppm16(const char *filename, int *wd, int *ht)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return 0;
  fscanf(f, "P6\n%d %d\n%*[^\n]", wd, ht);
  fgetc(f); // eat only one newline

  uint16_t *p = (uint16_t *)malloc(sizeof(uint16_t)*3*(*wd)*(*ht));
  int rd = fread(p, sizeof(uint16_t)*3, (*wd)*(*ht), f);
  fclose(f);
  if(rd != (*wd)*(*ht))
  {
    fprintf(stderr, "[read_ppm] unexpected end of file! maybe you're loading an 8-bit ppm here instead of a 16-bit one? (%s)\n", filename);
    free(p);
    return 0;
  }
  return p;
}

static uint8_t*
read_ppm8(const char *filename, int *wd, int *ht)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return 0;
  fscanf(f, "P6\n%d %d\n%*[^\n]", wd, ht);
  fgetc(f); // eat only one newline

  uint8_t *p = (uint8_t *)malloc(sizeof(uint8_t)*3*(*wd)*(*ht));
  int rd = fread(p, sizeof(uint8_t)*3, (*wd)*(*ht), f);
  fclose(f);
  if(rd != (*wd)*(*ht))
  {
    fprintf(stderr, "[read_ppm] unexpected end of file! (%s)\n", filename);
    free(p);
    return 0;
  }
  return p;
}

static inline float get_error(CurveData *c, CurveSample *csample, float* basecurve, int* cnt)
{
  CurveDataSample(c, csample);
  float sqrerr = 0.0f;
  const float max = 1.0f, min = 0.0f;
  for(int k=0; k<0x10000; k++)
  {
    // too few samples? no error if we ignore it.
    if(cnt[k] > 8)
    {
      float d = (basecurve[k] - (min + (max-min)*csample->m_Samples[k]*(1.0f/0x10000)));
      // way more error for lower values of x:
      d *= 0x10000-k;
      if(k < 655) d *= 100;
      sqrerr += d*d;
    }
  }
  return sqrerr;
}

static inline void mutate(CurveData *c, CurveData *t, float* basecurve)
{
  for(int k=1;k<c->m_numAnchors-1;k++)
  {
    float min = (c->m_anchors[k-1].x + c->m_anchors[k].x)/2.0f;
    float max = (c->m_anchors[k+1].x + c->m_anchors[k].x)/2.0f;
    const float x = min + drand48()*(max-min);
    uint32_t pos = x*0x10000;
    if(pos >= 0x10000) pos = 0xffff;
    if(pos < 0) pos = 0;
    t->m_anchors[k].x = x;
    t->m_anchors[k].y = basecurve[pos];
  }
  t->m_anchors[0].x = 0.0f;
  t->m_anchors[0].y = 0.0f;//basecurve[0];
  t->m_anchors[t->m_numAnchors-1].x = 1.0f;
  t->m_anchors[t->m_numAnchors-1].y = 1.0f;//basecurve[0xffff];
}

static inline float linearize_sRGB(float val)
{
  if(val < 0.04045f)
  {
    val /= 12.92f;
  }
  else
  {
    val = powf(((val + 0.055f)/1.055f), 2.4f);
  }
  return val;
}

#define SQUARE(a) ((a)*(a))
#define CUBIC(a) ((a)*SQUARE((a)))

static inline float Lab(float val)
{
  static const float threshold = CUBIC(6.f) / CUBIC(29.f);
  if (val>threshold)
  {
    val = powf(val, 1.f / 3.f);
  }
  else
  {
    val = 4.f / 29.f + SQUARE(29.f) / (3.f * SQUARE(6.f)) * val;
  }
  return val;
}

static inline float RGB_to_L(float r, float g, float b)
{
  float y = 0.2126f*r + 0.7152f*g + 0.0722f*b;
  return (116.f*Lab(y) - 16.f) / 100.f;
}

enum module_type
{
  MODULE_BASECURVE = 0,
  MODULE_TONECURVE = 1,
  MODULE_MAX
};

static void
build_channel_basecurve(
  int width_jpeg, int height_jpeg, uint8_t* buf_jpeg,
  int offx_raw, int offy_raw, int width_raw, uint16_t* buf_raw,
  int ch, float* curve, int* cnt)
{
  for(int j=0;j<height_jpeg;j++)
  {
    for(int i=0;i<width_jpeg;i++)
    {
      // raw coordinate is offset:
      const int ri = offx_raw + i;
      const int rj = offy_raw + j;

      // grab channel from JPEG first
      float val = buf_jpeg[3*(width_jpeg*j + i) + ch]/255.f;

      // linearize the sRGB value
      // XXX: this supposes it is sRGB, support arbitrary colorspace with ICC lib ?
      float jpegVal = linearize_sRGB(val);

      // grab RGB from RAW
      float rawVal = (float)buf_raw[3*(width_raw*rj + ri) + ch]/65535.f;

      uint16_t raw = (uint16_t)((rawVal*65535.f) + 0.5f);
      curve[raw] = (curve[raw]*cnt[raw] + jpegVal)/(cnt[raw] + 1.0f);
      cnt[raw]++;
    }
  }
}

static void
build_tonecurve(
  int width_jpeg, int height_jpeg, uint8_t* buf_jpeg,
  int offx_raw, int offy_raw, int width_raw, uint16_t* buf_raw,
  int ch, float* curve, int* cnt)
{
  for(int j=0;j<height_jpeg;j++)
  {
    for(int i=0;i<width_jpeg;i++)
    {
      // raw coordinate is offset:
      const int ri = offx_raw + i;
      const int rj = offy_raw + j;

      // grab RGB from JPEG first
      float r = buf_jpeg[3*(width_jpeg*j + i) + 0]/255.f;
      float g = buf_jpeg[3*(width_jpeg*j + i) + 1]/255.f;
      float b = buf_jpeg[3*(width_jpeg*j + i) + 2]/255.f;

      // linearize the sRGB value (this supposes it is sRGB, TODO support arbitrary colorspace with ICC lib ?)
      r = linearize_sRGB(r);
      g = linearize_sRGB(g);
      b = linearize_sRGB(b);

      // Compute the JPEG L val
      float jpegL = RGB_to_L(r, g, b);

      // grab RGB from RAW
      r = (float)buf_raw[3*(width_raw*rj + ri) + 0]/65535.f;
      g = (float)buf_raw[3*(width_raw*rj + ri) + 1]/65535.f;
      b = (float)buf_raw[3*(width_raw*rj + ri) + 2]/65535.f;

      // Compute the RAW L val
      float rawL = RGB_to_L(r, g, b);

      uint16_t raw = (uint16_t)((rawL*65535.f) + 0.5f);
      curve[raw] = (curve[raw]*cnt[raw] + jpegL)/(cnt[raw] + 1.0f);
      cnt[raw]++;
    }
  }
}

static void
fit_curve(CurveData* best, int* nopt, float* minsqerr, CurveSample* csample, int num_nodes, float* curve, int* cnt)
{
  // now do the fitting:
  CurveData curr, tent;

  // type = 2 (monotone hermite)
  curr.m_spline_type = 2;
  curr.m_numAnchors = num_nodes;
  curr.m_min_x = 0.0;
  curr.m_max_x = 1.0;
  curr.m_min_y = 0.0;
  curr.m_max_y = 1.0;

  *best = tent = curr;
  *nopt = 0;
  *minsqerr = FLT_MAX;

  const float p_large = .0f;
  float curr_m = FLT_MIN;

  const int samples = 1000;
  for(int i=0;i<samples;i++)
  {
    if(i == 0 || drand48() < p_large)
    { // large step
      for(int k=0;k<tent.m_numAnchors;k++)
      {
        float x = k/(tent.m_numAnchors-1.0f);
        x *= x*x; // move closer to 0
        uint32_t pos = x*0x10000;
        if(pos >= 0x10000) pos = 0xffff;
        if(pos < 0) pos = 0;
        tent.m_anchors[k].x = x;
        tent.m_anchors[k].y = curve[pos];
      }
    }
    else
    { // mutate
      mutate(&curr, &tent, curve);
    }
    float m = get_error(&tent, csample, curve, cnt);
    if(m < *minsqerr)
    {
      (*nopt)++;
      *minsqerr = m;
      *best = tent;

    }
    // fittness: 1/MSE
    const float a = curr_m/m;
    if(drand48() < a || i == 0)
    { // accept new state
      curr = tent;
      curr_m = m;
    }
  }
}

static void
print_usage(
  const char* name)
{
  fprintf(stderr, "usage: %s inputraw.ppm (16-bit) inputjpg.ppm (8-bit) [num_nodes] [target module]\n", name);
  fprintf(stderr, "convert the raw with `dcraw -6 -W -g 1 1 -w input.raw'\n");
  fprintf(stderr, "and the jpg with `convert input.jpg output.ppm'\n");
  fprintf(stderr, "plot the results with `gnuplot plot'\n");
}

int
main(int argc, char** argv)
{
  int ret = 1;

  // raw related vars
  int raw_width = -1;
  int raw_height = -1;
  int raw_offx = -1;
  int raw_offy = -1;
  uint16_t *raw_buff = NULL;

  // jpeg related vars
  int jpeg_width = -1;
  int jpeg_height = -1;
  uint8_t *jpeg_raw = NULL;

  // file output for basecurve and fit data
  FILE* fb = NULL;
  FILE* ff = NULL;

  // curve related vars
  int ncurves = -1;
  float* curve = NULL;
  int* cnt = NULL;
  CurveData fit;
  int accepts = -1;
  float sqerr = -1.f;
  CurveSample csample = {0};
  const int res = 0x10000;

  // program options
  int num_nodes = 8;
  enum module_type module = MODULE_BASECURVE;

  if(argc < 3)
  {
    print_usage(argv[0]);
    goto exit;
  }

  fb = fopen("basecurve.dat", "wb");
  if (!fb)
  {
    fprintf(stderr, "error: could not open `basecurve.dat'\n");
    goto exit;
  }

  ff = fopen("fit.dat", "wb");
  if (!ff)
  {
    fprintf(stderr, "error: could not open `fit.dat'\n");
    goto exit;
  }

  if(argc > 3)
  {
    num_nodes = atol(argv[3]);
  }

  if (argc > 4)
  {
    module = atol(argv[4]);
  }

  if(num_nodes > 20)
  {
    // basecurve doesn't support more than that.
    num_nodes = 20;
  }

  // module can be one of basecurve or tonecurve
  module = (module < 0) ? 0 : (module > MODULE_MAX) ? MODULE_MAX : module;

  raw_buff = read_ppm16(argv[1], &raw_width, &raw_height);
  if(!raw_buff)
  {
    fprintf(stderr, "error: failed reading the RAW file data\n");
    goto exit;
  }

  // swap to host byte, PPM16 are BE
  static const union { uint8_t u8[4]; uint32_t u32;} byte_order __attribute__((aligned(4))) = { .u8 = {1,2,3,4} };
  if (byte_order.u32 != 0x01020304)
  {
    for (int k=0; k<3*raw_width*raw_height; k++)
    {
      raw_buff[k] = ((raw_buff[k]&0xff) << 8) | (raw_buff[k] >> 8);
    }
  }

  jpeg_raw = read_ppm8(argv[2], &jpeg_width, &jpeg_height);
  if(!jpeg_raw)
  {
    fprintf(stderr, "error: failed reading JPEG file\n");
    goto exit;
  }

  ncurves = module == MODULE_BASECURVE ? 3 : 1;
  curve = calloc(1, 0x10000*sizeof(float)*ncurves);
  if (!curve) {
    fprintf(stderr, "error: failed allocating curve\n");
    goto exit;
  }

  cnt = calloc(1, 0x10000*sizeof(int)*ncurves);
  if (!cnt) {
    fprintf(stderr, "error: failed allocating histogram\n");
    goto exit;
  }

  raw_offx = (raw_width - jpeg_width)/2;
  raw_offy = (raw_height - jpeg_height)/2;
  if(raw_offx < 0 || raw_offy < 0)
  {
    fprintf(stderr, "error: jpeg has a higher resolution than the raw ? (%dx%d vs %dx%d)\n", jpeg_width, jpeg_height, raw_width, raw_height);
    goto exit;
  }

  float* curve_to_approximate = NULL;
  int* cnt_for_approximation = NULL;

  if (module == MODULE_BASECURVE)
  {
    for (int ch=0; ch<3; ch++)
    {
      build_channel_basecurve(jpeg_width, jpeg_height, jpeg_raw, raw_offx, raw_offy, raw_width, raw_buff, ch, curve+ch*0x10000, cnt+ch*0x10000);
    }

    // output the histograms:
    fprintf(fb, "# basecurve-red basecurve-green basecurve-blue basecurve-avg cnt-red cnt-green cnt-blue\n");
    for(int k=0;k<0x10000;k++)
    {
      float ch0 = curve[k + 0*0x10000];
      float ch1 = curve[k + 1*0x10000];
      float ch2 = curve[k + 2*0x10000];
      int c0 = cnt[k + 0*0x10000];
      int c1 = cnt[k + 1*0x10000];
      int c2 = cnt[k + 2*0x10000];
      fprintf(fb, "%f %f %f %f %d %d %d\n", ch0, ch1, ch2, (ch0 + ch1 + ch2)/3.0f, c0, c1, c2);
    }

    // for now it seems more stable to work on G channel alone
    curve_to_approximate = curve + 0x10000;
    cnt_for_approximation = cnt + 0x10000;
  }
  else if (module == MODULE_TONECURVE)
  {
    build_tonecurve(jpeg_width, jpeg_height, jpeg_raw, raw_offx, raw_offy, raw_width, raw_buff, 1, curve, cnt);

    // output the histogram
    fprintf(fb, "# tonecurve-L cnt-L\n");
    for(int k=0;k<0x10000;k++)
    {
      fprintf(fb, "%f %d\n", curve[k], cnt[k]);
    }

    // for now it seems more stable to work on G channel alone
    curve_to_approximate = curve;
    cnt_for_approximation = cnt;
  }

  free(raw_buff);
  raw_buff = NULL;

  free(jpeg_raw);
  jpeg_raw = NULL;

  csample.m_samplingRes = res;
  csample.m_outputRes = 0x10000;
  csample.m_Samples = (uint16_t *)calloc(1, sizeof(uint16_t)*0x10000);
  fit_curve(&fit, &accepts, &sqerr, &csample, num_nodes, curve_to_approximate, cnt_for_approximation);

  if (module == MODULE_BASECURVE)
  {
    fprintf(ff, "# err %f improved %d times\n", sqerr, accepts);
    fprintf(ff, "# copy paste into iop/basecurve.c (be sure to insert name, maker, model, and set the last 0 to 1 if happy to filter it):\n");
    fprintf(ff, "# { \"new measured basecurve\", \"insert maker\", \"insert model\", 0, 51200,                        {{{");
    for(int k=0;k<fit.m_numAnchors;k++)
      fprintf(ff, "{%f, %f}%s", fit.m_anchors[k].x, fit.m_anchors[k].y, k<fit.m_numAnchors-1?", ":"}}, ");
    fprintf(ff, "{%d}, {m}}, 0, 0},\n", fit.m_numAnchors);
    CurveDataSample(&fit, &csample);
    for(int k=0; k<0x10000; k++)
      fprintf(ff, "%f %f\n", k*(1.0f/0x10000), 0.0 + (1.0f-0.0f)*csample.m_Samples[k]*(1.0f/0x10000));

    char encoded[2048];

    dt_iop_basecurve_params_t params;
    memset(&params, 0, sizeof(params));
    for(int k=0;k<fit.m_numAnchors;k++)
    {
      params.basecurve[0][k].x = fit.m_anchors[k].x;
      params.basecurve[0][k].y = fit.m_anchors[k].y;
    }
    params.basecurve_nodes[0] = fit.m_numAnchors;
    params.basecurve_type[0] = MONOTONE_HERMITE;

    text_encode ((uint8_t *)&params, encoded, sizeof(params));

    fprintf(stdout, "to test your new basecurve, copy/paste the following line into your shell.\n");
    fprintf(stdout, "note that it is a smart idea to backup your database before messing with it on this level.\n");
    fprintf(stdout, "(you have been warned :) )\n\n");
    // the big binary blob is a canonical blend mode option (switched off).
    fprintf(stdout, "echo \"INSERT INTO presets VALUES('measured basecurve','','basecurve',2,X'%s',1,X'00000000180000000000C842000000000000000000000000000000000000000000000000000000000000000000000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F',7,0,'','%%','%%','%%',0.0,51200.0,0.0,10000000.0,0.0,100000000.0,0.0,1000.0,0,0,0,0,2);\" | sqlite3 ~/.config/darktable/library.db\n", encoded);
  }
  else if (module == MODULE_TONECURVE)
  {
    fprintf(ff, "# err %f improved %d times\n", sqerr, accepts);
    fprintf(ff, "# ");
    for(int k=0;k<fit.m_numAnchors;k++)
      fprintf(ff, "{%f, %f}%s", fit.m_anchors[k].x, fit.m_anchors[k].y, k<fit.m_numAnchors-1?", ":"\n");
    fprintf(ff, "# {%d}\n", fit.m_numAnchors);
    CurveDataSample(&fit, &csample);
    for(int k=0; k<0x10000; k++)
      fprintf(ff, "%f %f\n", k*(1.0f/0x10000), 0.0 + (1.0f-0.0f)*csample.m_Samples[k]*(1.0f/0x10000));
    fprintf(stdout, "# still WIP, a dump of best fit curve in is fit.dat\n");
  }

exit:
  if (fb)
  {
    fclose(fb);
    fb = NULL;
  }
  if (ff)
  {
    fclose(ff);
    ff = NULL;
  }
  if (csample.m_Samples)
  {
    free(csample.m_Samples);
    csample.m_Samples = NULL;
  }
  if (curve)
  {
    free(curve);
    curve = NULL;
  }
  if (cnt)
  {
    free(cnt);
    cnt = NULL;
  }

  return ret;
}

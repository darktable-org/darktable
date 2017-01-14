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
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#include <errno.h>

/* --------------------------------------------------------------------------
 * curve and histogram resolution
 * ------------------------------------------------------------------------*/

#define CURVE_RESOLUTION 0x10000

extern int
exif_get_ascii_datafield(
  const char* filename,
  const char* key,
  char* buf,
  size_t buflen);

/* --------------------------------------------------------------------------
 * Curve code used for fitting the curves
 * ------------------------------------------------------------------------*/

#include "../../src/common/curve_tools.c"

/* --------------------------------------------------------------------------
 * Basecurve params
 * copied from iop/basecurve.c. fixed at specific revision on purpose
 * ------------------------------------------------------------------------*/

#define BASECURVE_PARAMS_VERSION 2

typedef struct dt_iop_basecurve_node_t
{
  float x;
  float y;
} dt_iop_basecurve_node_t;

#define DT_IOP_BASECURVE_MAXNODES 20
typedef struct dt_iop_basecurve_params_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][DT_IOP_BASECURVE_MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
} dt_iop_basecurve_params_t;

/* --------------------------------------------------------------------------
 * Tonecurve params
 * copied from iop/toncurve.h. fixed at specific revision on purpose
 * ------------------------------------------------------------------------*/

#define TONECURVE_PARAMS_VERSION 4

typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_t;

#define DT_IOP_TONECURVE_MAXNODES 20
typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES];  // three curves (L, a, b) with max number of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
  int tonecurve_unbound_ab;
} dt_iop_tonecurve_params_t;

/* --------------------------------------------------------------------------
 * utils
 * ------------------------------------------------------------------------*/

static void
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

static int
read_ppm_header(
  FILE *f,
  int* wd,
  int* ht)
{
  int r = 0;
  char buf[2];

  r = fseek(f, 0, SEEK_SET);
  if (r != 0) {
    r = -1;
    goto exit;
  }

  // read and check header
  r = fread(buf, 1, 2, f);
  if (r != 2 || buf[0] != 'P' || buf[1] != '6')
  {
    r = -1;
    goto exit;
  }

  // scan for width and height
  r = fscanf(f, "%*[^0-9]%d %d\n%*[^\n]", wd, ht);

  // read final newline
  fgetc(f);

  // finalize return value
  r = (r != 2) ? -1 : 0;

exit:
  return r;
}

static uint16_t*
read_ppm16(const char *filename, int *wd, int *ht)
{
  FILE *f = NULL;
  uint16_t *p = NULL;

  f = fopen(filename, "rb");
  if (!f)
  {
    goto exit;
  }

  if (read_ppm_header(f, wd, ht)) {
    goto exit;
  }

  p = (uint16_t *)malloc(sizeof(uint16_t)*3*(*wd)*(*ht));
  int rd = fread(p, sizeof(uint16_t)*3, (*wd)*(*ht), f);
  if(rd != (*wd)*(*ht))
  {
    fprintf(stderr, "[read_ppm] unexpected end of file! maybe you're loading an 8-bit ppm here instead of a 16-bit one? (%s)\n", filename);
    free(p);
    p = NULL;
  }

exit:
  if (f) {
    fclose(f);
    f = NULL;
  }

  return p;
}

static uint8_t*
read_ppm8(const char *filename, int *wd, int *ht)
{
  FILE* f = NULL;
  uint8_t *p = NULL;

  f = fopen(filename, "rb");
  if(!f) {
    goto exit;
  }

  if (read_ppm_header(f, wd, ht)) {
    goto exit;
  }

  p = (uint8_t *)malloc(sizeof(uint8_t)*3*(*wd)*(*ht));
  int rd = fread(p, sizeof(uint8_t)*3, (*wd)*(*ht), f);
  if(rd != (*wd)*(*ht))
  {
    fprintf(stderr, "[read_ppm] unexpected end of file! (%s)\n", filename);
    free(p);
    p  = NULL;
  }

exit:
  if (f) {
    fclose(f);
    f = NULL;
  }

  return p;
}

static inline float get_error(CurveData *c, CurveSample *csample, float* basecurve, uint32_t* cnt)
{
  CurveDataSample(c, csample);
  float sqrerr = 0.0f;
  const float max = 1.0f, min = 0.0f;
  for(int k=0; k<CURVE_RESOLUTION; k++)
  {
    // too few samples? no error if we ignore it.
    if(cnt[k] > 8)
    {
      float d = (basecurve[k] - (min + (max-min)*csample->m_Samples[k]*(1.0f/CURVE_RESOLUTION)));
      // way more error for lower values of x:
      d *= CURVE_RESOLUTION-k;
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
    uint32_t pos = x*CURVE_RESOLUTION;
    if(pos >= CURVE_RESOLUTION) pos = CURVE_RESOLUTION-1;
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

// NB: DT uses L*a*b* D50
static inline void
RGB2Lab(float* L, float* a, float*b, float R, float G, float B)
{
  // RGB to CIE 1931 XYZ @D50 first
  const float X = 0.4360747f*R + 0.3850649f*G + 0.1430804f*B;
  const float Y = 0.2225045f*R + 0.7168786f*G + 0.0606169f*B;
  const float Z = 0.0139322f*R + 0.0971045f*G + 0.7141733f*B;

  // Apply D50/ICC illuminant, then transform using the L*a*b* function
  const float fx = Lab(X/0.9642f);
  const float fy = Lab(Y);
  const float fz = Lab(Z/0.8249f);

  *L = 116.f*fy - 16.f;
  *a = 500.f*(fx - fy);
  *b = 200.f*(fy - fz);
}
static inline void
Lab2UnitCube(float* L, float* a, float*b)
{
  *L /= 100.f;
  *a = (*a + 128.f) / 256.f;
  *b = (*b + 128.f) / 256.f;
}

enum module_type
{
  MODULE_BASECURVE = 0,
  MODULE_TONECURVE = 1,
  MODULE_MAX
};

static void
linearize_8bit(
  int width, int height,
  uint8_t* _s,
  float* _d)
{
  // XXX support ICC profiles here ?
#pragma omp parallel for
  for (int y=0; y<height; y++) {
    float* d = _d + 3*width*y;
    uint8_t* s = _s + 3*width*y;
    for (int x=0; x<width; x++) {
      d[0] = linearize_sRGB((float)s[0]/255.f);
      d[1] = linearize_sRGB((float)s[1]/255.f);
      d[2] = linearize_sRGB((float)s[2]/255.f);
      d += 3;
      s += 3;
    }
  }
}

static void
linearize_16bit(
  int width, int height,
  uint16_t* _s,
  float* _d)
{
#pragma omp parallel for
  for (int y=0; y<height; y++) {
    float* d = _d + 3*width*y;
    uint16_t* s = _s + 3*width*y;
    for (int x=0; x<width; x++) {
      d[0] = (float)s[0]/65535.f;
      d[1] = (float)s[1]/65535.f;
      d[2] = (float)s[2]/65535.f;
      d += 3;
      s += 3;
    }
  }
}

static void
build_channel_basecurve(
  int width_jpeg, int height_jpeg, float* buf_jpeg,
  int offx_raw, int offy_raw, int width_raw, float* buf_raw,
  int ch, float* curve, uint32_t* cnt)
{
  for(int j=0;j<height_jpeg;j++)
  {
    for(int i=0;i<width_jpeg;i++)
    {
      // raw coordinate is offset:
      const int ri = offx_raw + i;
      const int rj = offy_raw + j;

      // grab channel from JPEG first
      float jpegVal = buf_jpeg[3*(width_jpeg*j + i) + ch];

      // grab RGB from RAW
      float rawVal = buf_raw[3*(width_raw*rj + ri) + ch];

      size_t raw = (size_t)((rawVal*(float)(CURVE_RESOLUTION-1)) + 0.5f);
      curve[raw] = (curve[raw]*cnt[raw] + jpegVal)/(cnt[raw] + 1.0f);
      cnt[raw]++;
    }
  }
}

static void
build_tonecurve(
  int width_jpeg, int height_jpeg, float* buf_jpeg,
  int offx_raw, int offy_raw, int width_raw, float* buf_raw,
  float* curve, uint32_t* hist)
{
  float* cL = curve;
  float* ca = cL + CURVE_RESOLUTION;
  float* cb = ca + CURVE_RESOLUTION;

  uint32_t* hL = hist;
  uint32_t* ha = hL + CURVE_RESOLUTION;
  uint32_t* hb = ha + CURVE_RESOLUTION;

  for(int j=0;j<height_jpeg;j++)
  {
    for(int i=0;i<width_jpeg;i++)
    {
      // raw coordinate is offset:
      const int ri = offx_raw + i;
      const int rj = offy_raw + j;

      // grab RGB from JPEG first
      float r = buf_jpeg[3*(width_jpeg*j + i) + 0];
      float g = buf_jpeg[3*(width_jpeg*j + i) + 1];
      float b = buf_jpeg[3*(width_jpeg*j + i) + 2];

      // Compute the JPEG L val
      float L_jpeg;
      float a_jpeg;
      float b_jpeg;
      RGB2Lab(&L_jpeg, &a_jpeg, &b_jpeg, r, g, b);
      Lab2UnitCube(&L_jpeg, &a_jpeg, &b_jpeg);

      // grab RGB from RAW
      r = buf_raw[3*(width_raw*rj + ri) + 0];
      g = buf_raw[3*(width_raw*rj + ri) + 1];
      b = buf_raw[3*(width_raw*rj + ri) + 2];

      // Compute the RAW L val
      float L_raw;
      float a_raw;
      float b_raw;
      RGB2Lab(&L_raw, &a_raw, &b_raw, r, g, b);
      Lab2UnitCube(&L_raw, &a_raw, &b_raw);

      size_t Li = (size_t)(L_raw*(float)(CURVE_RESOLUTION-1) + 0.5f);
      size_t ai = (size_t)(a_raw*(float)(CURVE_RESOLUTION-1) + 0.5f);
      size_t bi = (size_t)(b_raw*(float)(CURVE_RESOLUTION-1) + 0.5f);
      cL[Li] = (cL[Li]*hL[Li] + L_jpeg)/(hL[Li] + 1.0f);
      ca[ai] = (ca[ai]*ha[ai] + a_jpeg)/(ha[ai] + 1.0f);
      cb[bi] = (cb[bi]*hb[bi] + b_jpeg)/(hb[bi] + 1.0f);
      hL[Li]++;
      ha[ai]++;
      hb[bi]++;
    }
  }
}

static void
fit_curve(CurveData* best, int* nopt, float* minsqerr, CurveSample* csample, int num_nodes, float* curve, uint32_t* cnt)
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
        uint32_t pos = x*CURVE_RESOLUTION;
        if(pos >= CURVE_RESOLUTION) pos = CURVE_RESOLUTION-1;
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

static inline int
is_bigendian()
{
  // swap to host byte, PPM16 are BE
  static const union { uint8_t u8[4]; uint32_t u32;} byte_order __attribute__((aligned(4))) = { .u8 = {1,2,3,4} };
  return byte_order.u32 == 0x01020304;

}
struct options
{
  const char* filename_basecurve_fit;
  const char* filename_tonecurve_fit;
  const char* filename_basecurve;
  const char* filename_tonecurve;
  const char* filename_state;
  const char* filename_raw;
  const char* filename_jpeg;
  const char* filename_exif;
  int num_nodes;
  int finalize;
  int scale_ab;
};

static void
print_usage(
  const char* name)
{
  fprintf(stderr,
    "first pass, accumulate statistics (can be repeated to cover all tonal range):\n"
    "%s [OPTIONS] <inputraw.ppm (16-bit)> <inputjpg.ppm (8-bit)>\n"
    "\n"
    "second pass, compute the curves:\n"
    " %s -z [OPTIONS]\n"
    "\n"
    "OPTIONS:\n"
    " -n <integer>    Number of nodes for the curve\n"
    " -b <filename>   Basecurve output filename\n"
    " -c <filename>   Basecurve Fit curve output filename\n"
    " -t <filename>   Tonecurve output filename\n"
    " -u <filename>   Tonecurve Fit curve output filename\n"
    " -a              Tonecurve Fit the a* and b* channels\n"
    " -s <filename>   Save state\n"
    " -z              Compute the fitting curve\n"
    " -e <filename>   Retrieve model and make from file's Exif metadata\n"
    " -h              Print this help message\n"
    "\n"
    "convert the raw with `dcraw -6 -W -g 1 1 -w input.raw'\n"
    "and the jpg with `convert input.jpg output.ppm'\n"
    "plot the results with `gnuplot plot.(basecurve|tonecurve) depending on target module'\n"
    "\n"
    "first do a pass over a few images to accumulate data in the save state file, and then\n"
    "compute the fit curve using option -z\n",
    name, name);
}

static void
set_default_options(
  struct options* opts)
{
  static const char* default_filename_basecurve = "basecurve.dat";
  static const char* default_filename_basecurve_fit = "basecurve.fit.dat";
  static const char* default_filename_tonecurve = "tonecurve.dat";
  static const char* default_filename_tonecurve_fit = "tonecurve.fit.dat";
  static const char* default_state = "dt-curve-tool.bin";
  opts->filename_basecurve = default_filename_basecurve;
  opts->filename_basecurve_fit = default_filename_basecurve_fit;
  opts->filename_tonecurve = default_filename_tonecurve;
  opts->filename_tonecurve_fit = default_filename_tonecurve_fit;
  opts->filename_state = default_state;
  opts->filename_jpeg = NULL;
  opts->filename_raw = NULL;
  opts->num_nodes = 12;
  opts->finalize = 0;
  opts->filename_exif = NULL;
  opts->scale_ab = 0;
}

static int
parse_arguments(
  int argc,
  char** argv,
  struct options* opts)
{
  opterr = 1;

  int c;
  int ex = 0;
  while ((c = getopt(argc, argv, "hn:b:c:t:u:s:ze:a")) >= 0)
  {
    switch (c)
    {
    case 'n':
      opts->num_nodes = atoi(optarg);
      break;
    case 'b':
      opts->filename_basecurve = optarg;
      break;
    case 'c':
      opts->filename_basecurve_fit = optarg;
      break;
    case 't':
      opts->filename_tonecurve = optarg;
      break;
    case 'u':
      opts->filename_tonecurve_fit = optarg;
      break;
    case 's':
      opts->filename_state = optarg;
      break;
    case 'z':
      opts->finalize = 1;
      break;
    case 'e':
      opts->filename_exif = optarg;
      break;
    case 'a':
      opts->scale_ab = 1;
      break;
    case ':':
      fprintf(stderr, "missing argument for option -%c, ignored\n", optopt);
      print_usage(argv[0]);
      ex = 1;
      break;
    case '?':
      fprintf(stderr, "unknown option -%c\n", optopt);
      print_usage(argv[0]);
      ex = 1;
      break;
    case 'h':
      print_usage(argv[0]);
      ex = 1;
    }
  }

  if (!ex)
  {
    if (!opts->finalize)
    {
      if (optind < argc - 1)
      {
        opts->filename_raw = argv[optind];
        opts->filename_jpeg = argv[optind+1];
      }
      else
      {
        print_usage(argv[0]);
        ex = 1;
      }
    }
  }

  return ex;
}

void
read_curveset(
  FILE* f,
  float* curve,
  uint32_t* hist)
{
  int r = fread(curve, 1, 3*CURVE_RESOLUTION*sizeof(float), f);
  if (r != 3*CURVE_RESOLUTION*sizeof(float))
  {
    /* could not read save state, either missing stats in that save file or
     * corrupt data. both cases need to clean state */
    memset(curve, 0, 3*CURVE_RESOLUTION*sizeof(float));
  }
  else
  {
    r = fread(hist, 1, 3*CURVE_RESOLUTION*sizeof(uint32_t), f);
    if (r != 3*CURVE_RESOLUTION*sizeof(uint32_t))
    {
      /* could not read save state, either missing stats in that save file or
       * corrupt data. both cases need to clean state */
      memset(curve, 0, 3*CURVE_RESOLUTION*sizeof(float));
      memset(hist, 0, 3*CURVE_RESOLUTION*sizeof(uint32_t));
    }
  }
}

int
write_curveset(
  FILE* f,
  float* curve,
  uint32_t* hist)
{
  int ret = -1;

  int w = fwrite(curve, 1, 3*CURVE_RESOLUTION*sizeof(float), f);
  if (w != 3*CURVE_RESOLUTION*sizeof(float))
  {
    fprintf(stderr, "error: failed writing curves to save state file\n");
    goto exit;
  }

  w = fwrite(hist, 1, 3*CURVE_RESOLUTION*sizeof(uint32_t), f);
  if (w != 3*CURVE_RESOLUTION*sizeof(uint32_t))
  {
    fprintf(stderr, "error: failed writing histograms to save state file\n");
    goto exit;
  }

  ret = 0;

exit:

  return ret;
}

/* --------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------*/

int
main(int argc, char** argv)
{
  int ret = 1;

  // program options
  struct options opt;

  // raw related vars
  int raw_width = -1;
  int raw_height = -1;
  int raw_offx = -1;
  int raw_offy = -1;
  uint16_t *raw_buff = NULL;
  float* raw_buff_f = NULL;

  // jpeg related vars
  int jpeg_width = -1;
  int jpeg_height = -1;
  uint8_t *jpeg_buff = NULL;
  float* jpeg_buff_f = NULL;

  // all in one FILE handle
  FILE* f = NULL;

  // curve related vars
  float* curve = NULL;
  float* curve_base = NULL;
  float* curve_tone = NULL;
  uint32_t* hist = NULL;
  uint32_t* hist_base = NULL;
  uint32_t* hist_tone = NULL;
  CurveData fit;
  int accepts = -1;
  float sqerr = -1.f;
  CurveSample csample = {0};

  set_default_options(&opt);
  int shallexit = parse_arguments(argc, argv, &opt);
  if (shallexit)
  {
    goto exit;
  }

  if(opt.num_nodes > 20)
  {
    // basecurve and tonecurve do not support more than that.
    opt.num_nodes = 20;
  }

  curve = calloc(1, CURVE_RESOLUTION*sizeof(float)*6);
  if (!curve) {
    fprintf(stderr, "error: failed allocating curve\n");
    ret = -1;
    goto exit;
  }
  curve_base = curve;
  curve_tone = curve + 3*CURVE_RESOLUTION;

  hist = calloc(1, CURVE_RESOLUTION*sizeof(uint32_t)*6);
  if (!hist) {
    fprintf(stderr, "error: failed allocating histogram\n");
    ret = -1;
    goto exit;
  }
  hist_base = hist;
  hist_tone = hist + 3*CURVE_RESOLUTION;

  // read saved state if any
  f = fopen(opt.filename_state, "rb");
  if (f)
  {
    read_curveset(f, curve_base, hist_base);
    read_curveset(f, curve_tone, hist_tone);
    fclose(f);
    f = NULL;
  }

  if (opt.finalize)
  {
    goto fit;
  }

  // read the raw PPM file
  raw_buff = read_ppm16(opt.filename_raw, &raw_width, &raw_height);
  if(!raw_buff)
  {
    fprintf(stderr, "error: failed reading the raw file data `%s'\n", opt.filename_raw);
    goto exit;
  }

  // read the JPEG PPM file
  jpeg_buff = read_ppm8(opt.filename_jpeg, &jpeg_width, &jpeg_height);
  if(!jpeg_buff)
  {
    fprintf(stderr, "error: failed reading JPEG file\n");
    goto exit;
  }

  // discard rotated JPEGs for now
  raw_offx = (raw_width - jpeg_width)/2;
  raw_offy = (raw_height - jpeg_height)/2;
  if(raw_offx < 0 || raw_offy < 0)
  {
    fprintf(stderr, "error: jpeg has a higher resolution than the raw ? (%dx%d vs %dx%d)\n", jpeg_width, jpeg_height, raw_width, raw_height);
    goto exit;
  }

  // swap to host byte, PPM16 are BE
  if (!is_bigendian())
  {
    for (int k=0; k<3*raw_width*raw_height; k++)
    {
      raw_buff[k] = ((raw_buff[k]&0xff) << 8) | (raw_buff[k] >> 8);
    }
  }

  raw_buff_f = calloc(1, 3*raw_width*raw_height*sizeof(float));
  if (!raw_buff_f) {
    fprintf(stderr, "error: failed allocating raw file float buffer\n");
    goto exit;
  }

  // normalize to [0,1] cube once for all
  linearize_16bit(raw_width, raw_height, raw_buff, raw_buff_f);

  // get rid of original 16bit data
  free(raw_buff);
  raw_buff = NULL;

  jpeg_buff_f = calloc(1, 3*jpeg_width*jpeg_height*sizeof(float));
  if (!jpeg_buff_f) {
    fprintf(stderr, "error: failed allocating JPEG file float buffer\n");
    goto exit;
  }

  // linearize and normalize to unit cube
  linearize_8bit(jpeg_width, jpeg_height, jpeg_buff, jpeg_buff_f);

  // get rid of original 8bit data
  free(jpeg_buff);
  jpeg_buff = NULL;

  /* ------------------------------------------------------------------------
   * Overflow test, we test for worst case scenario, all pixels would be
   * concentrated on the sample with maximum histogram
   * ----------------------------------------------------------------------*/

  {
    uint32_t maxhist = 0;

    for (int i=0 ; i<6; i++)
    {
      for (int j=0; j<CURVE_RESOLUTION; j++)
      {
        if (maxhist < hist[i*CURVE_RESOLUTION + j])
        {
          maxhist = hist[i*CURVE_RESOLUTION + j];
        }
      }
    }

    if ((UINT32_MAX - maxhist) < (uint32_t)(jpeg_width*jpeg_height))
    {
      fprintf(stderr, "error: analyzing this image could overflow internal counters. Refusing to process\n");
      goto exit;
    }
  }

  /* ------------------------------------------------------------------------
   * Basecurve part
   * ----------------------------------------------------------------------*/

  f = fopen(opt.filename_basecurve, "wb");
  if (!f)
  {
    fprintf(stderr, "error: could not open '%s'\n", opt.filename_basecurve);
    goto exit;
  }

  for (int ch=0; ch<3; ch++)
  {
    build_channel_basecurve(jpeg_width, jpeg_height, jpeg_buff_f, raw_offx, raw_offy, raw_width, raw_buff_f, ch, curve_base+ch*CURVE_RESOLUTION, hist_base+ch*CURVE_RESOLUTION);
  }

  {
    // for writing easiness
    float* ch0 = &curve_base[0*CURVE_RESOLUTION];
    float* ch1 = &curve_base[1*CURVE_RESOLUTION];
    float* ch2 = &curve_base[2*CURVE_RESOLUTION];
    uint32_t* h0 = &hist_base[0*CURVE_RESOLUTION];
    uint32_t* h1 = &hist_base[1*CURVE_RESOLUTION];
    uint32_t* h2 = &hist_base[2*CURVE_RESOLUTION];

    // output the histograms:
    fprintf(f, "# basecurve-red basecurve-green basecurve-blue basecurve-avg cnt-red cnt-green cnt-blue\n");
    for(int k=0;k<CURVE_RESOLUTION;k++)
    {
      fprintf(f, "%f %f %f %f %d %d %d\n", ch0[k], ch1[k], ch2[k], (ch0[k] + ch1[k] + ch2[k])/3.0f, h0[k], h1[k], h2[k]);
    }
  }

  fclose(f);
  f = NULL;

  /* ------------------------------------------------------------------------
   * Tonecurve part
   * ----------------------------------------------------------------------*/

  f = fopen(opt.filename_tonecurve, "wb");
  if (!f)
  {
    fprintf(stderr, "error: could not open '%s'\n", opt.filename_tonecurve);
    goto exit;
  }

  build_tonecurve(jpeg_width, jpeg_height, jpeg_buff_f, raw_offx, raw_offy, raw_width, raw_buff_f, curve_tone, hist_tone);

  {
    float* ch0 = &curve_tone[0*CURVE_RESOLUTION];
    float* ch1 = &curve_tone[1*CURVE_RESOLUTION];
    float* ch2 = &curve_tone[2*CURVE_RESOLUTION];
    uint32_t* h0 = &hist_tone[0*CURVE_RESOLUTION];
    uint32_t* h1 = &hist_tone[1*CURVE_RESOLUTION];
    uint32_t* h2 = &hist_tone[2*CURVE_RESOLUTION];

    // output the histogram
    fprintf(f, "# tonecurve-L tonecurve-a tonecurve-b cnt-L cnt-a cnt-b\n");
    for(int k=0;k<CURVE_RESOLUTION;k++)
    {
      fprintf(f, "%f %f %f %d %d %d\n", ch0[k], ch1[k], ch2[k], h0[k], h1[k], h2[k]);
    }
  }

  fclose(f);
  f = NULL;

  free(raw_buff_f);
  raw_buff_f = NULL;

  free(jpeg_buff_f);
  jpeg_buff_f = NULL;

  /* ------------------------------------------------------------------------
   * Write save state w/ the gathered data
   * ----------------------------------------------------------------------*/

  f = fopen(opt.filename_state, "r+");
  if (!f && errno == ENOENT)
  {
    f = fopen(opt.filename_state, "w+");
  }
  if (f)
  {
    if (write_curveset(f, curve_base, hist_base))
    {
      goto exit;
    }
    if (write_curveset(f, curve_tone, hist_tone))
    {
      goto exit;
    }
  }
  else
  {
    fprintf(stdout, "failed opening save file errno=%d\n", errno);
    goto exit;
  }

  if (!opt.finalize)
  {
    goto exit;
  }

fit:;

  char maker[32];
  char model[32];

  if (opt.filename_exif)
  {
    exif_get_ascii_datafield(opt.filename_exif, "Exif.Image.Model", model, sizeof(model));
    exif_get_ascii_datafield(opt.filename_exif, "Exif.Image.Make", maker, sizeof(maker));
  }

  csample.m_samplingRes = CURVE_RESOLUTION;
  csample.m_outputRes = CURVE_RESOLUTION;
  csample.m_Samples = (uint16_t *)calloc(1, sizeof(uint16_t)*CURVE_RESOLUTION);

  /* ------------------------------------------------------------------------
   * Basecurve fit
   * ----------------------------------------------------------------------*/

  f = fopen(opt.filename_basecurve_fit, "w+b");
  if (!f)
  {
    fprintf(stderr, "error: could not open '%s'\n", opt.filename_basecurve_fit);
    goto exit;
  }

  // fit G channel curve only, this seems to be the best choice for now
  fit_curve(&fit, &accepts, &sqerr, &csample, opt.num_nodes, curve_base+CURVE_RESOLUTION, hist_base+CURVE_RESOLUTION);

  fprintf(f, "# err %f improved %d times\n", sqerr, accepts);
  fprintf(f, "# copy paste into iop/basecurve.c (be sure to insert name, maker, model, and set the last 0 to 1 if happy to filter it):\n");
  fprintf(f, "# { \"%s\", \"%s\", \"%s\", 0, FLT_MAX,                      {{{",
    opt.filename_exif ? model : "new measured basecurve",
    opt.filename_exif ? maker : "insert maker",
    opt.filename_exif ? model : "insert model");
  for(int k=0;k<fit.m_numAnchors;k++)
  {
    fprintf(f, "{%f, %f}%s", fit.m_anchors[k].x, fit.m_anchors[k].y, k<fit.m_numAnchors-1?", ":"}}, ");
  }
  fprintf(f, "{%d}, {m}}, 0, 0},\n", fit.m_numAnchors);
  CurveDataSample(&fit, &csample);
  for(int k=0; k<CURVE_RESOLUTION; k++)
  {
    fprintf(f, "%f %f\n", k*(1.0f/CURVE_RESOLUTION), 0.0 + (1.0f-0.0f)*csample.m_Samples[k]*(1.0f/CURVE_RESOLUTION));
  }

  fclose(f);
  f = NULL;

  {
    uint8_t encoded[2048];

    dt_iop_basecurve_params_t params;
    memset(&params, 0, sizeof(params));
    for(int k=0;k<fit.m_numAnchors;k++)
    {
      params.basecurve[0][k].x = fit.m_anchors[k].x;
      params.basecurve[0][k].y = fit.m_anchors[k].y;
    }
    params.basecurve_nodes[0] = fit.m_numAnchors;
    params.basecurve_type[0] = MONOTONE_HERMITE;

    hexify(encoded, (uint8_t *)&params, sizeof(params));

    fprintf(stdout, "#!/bin/sh\n");
    fprintf(stdout, "# to test your new basecurve, copy/paste the following line into your shell.\n");
    fprintf(stdout, "# note that it is a smart idea to backup your database before messing with it on this level.\n");
    fprintf(stdout, "# (you have been warned :) )\n\n");
    // the big binary blob is a canonical blend mode option (switched off).
    fprintf(stdout, "echo \"INSERT INTO presets (name,description,operation,op_version,op_params,enabled,blendop_params,blendop_version,multi_priority,multi_name,model,maker,lens,iso_min,iso_max,exposure_min,exposure_max,aperture_min,aperture_max,focal_length_min,focal_length_max,writeprotect,autoapply,filter,def,format) VALUES('%s','','basecurve',%d,X'%s',1,X'00000000180000000000C842000000000000000000000000000000000000000000000000000000000000000000000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F',7,0,'','%%','%%','%%',0.0,340282346638528859812000000000000000000,0.0,10000000.0,0.0,100000000.0,0.0,1000.0,0,0,0,0,2);\" | sqlite3 ~/.config/darktable/data.db\n",
      opt.filename_exif ? model : "new measured basecurve",
      BASECURVE_PARAMS_VERSION, encoded);

    fprintf(stdout, "\n\n\n"
            "# if it pleases you, then in iop/basecurve.c append the following line to the array basecurve_presets and modify its name\n"
            "# {\"%s\", \"%s\", \"%s\", 0, FLT_MAX, {{{",
            opt.filename_exif ? model : "new measured basecurve",
            opt.filename_exif ? maker : "<MAKER>",
            opt.filename_exif ? model : "<MODEL>");
    for(int k=0;k<fit.m_numAnchors;k++)
    {
      fprintf(stdout, "{%f, %f}%s", params.basecurve[0][k].x, params.basecurve[0][k].y, k==fit.m_numAnchors-1?"":", ");
    }
    fprintf(stdout, "}}, {%d}, {m}}, 0, 1},\n\n\n", fit.m_numAnchors);
  }

  /* ------------------------------------------------------------------------
   * Fit the tonecurve
   * ----------------------------------------------------------------------*/

  f = fopen(opt.filename_tonecurve_fit, "w+b");
  if (!f)
  {
    fprintf(stderr, "error: could not open '%s'\n", opt.filename_tonecurve_fit);
    goto exit;
  }

  {
    struct dt_iop_tonecurve_params_t params;
    memset(&params, 0, sizeof(params));

    for (int i=0; i<(opt.scale_ab ? 3 : 1); i++)
    {
      fit_curve(&fit, &accepts, &sqerr, &csample, opt.num_nodes, curve_tone+i*CURVE_RESOLUTION, hist_tone+i*CURVE_RESOLUTION);

      CurveDataSample(&fit, &csample);
      for(int k=0; k<CURVE_RESOLUTION; k++)
      {
        fprintf(f, "%f %f\n", k*(1.0f/CURVE_RESOLUTION), 0.0 + (1.0f-0.0f)*csample.m_Samples[k]*(1.0f/CURVE_RESOLUTION));
      }
      fprintf(f, "\n\n");

      for (int k=0; k<fit.m_numAnchors; k++)
      {
        params.tonecurve[i][k].x = fit.m_anchors[k].x;
        params.tonecurve[i][k].y = fit.m_anchors[k].y;
      }
      params.tonecurve_nodes[i] = fit.m_numAnchors;
      params.tonecurve_type[i] = 2; // monotone hermite
    }

    fclose(f);
    f = NULL;

    if (opt.scale_ab)
    {
      params.tonecurve_autoscale_ab = 0;
    }
    else
    {
      for (int i=1; i<3; i++)
      {
        for (int k=0; k<opt.num_nodes; k++)
        {
          params.tonecurve[i][k].x = (float)k/(float)opt.num_nodes;
          params.tonecurve[i][k].y = (float)k/(float)opt.num_nodes;
        }
        params.tonecurve_nodes[i] = opt.num_nodes;
        params.tonecurve_type[i] = 2; // monotone hermite
      }

      params.tonecurve_autoscale_ab = 1;
    }
    params.tonecurve_unbound_ab = 0;

    uint8_t encoded[2048];
    hexify(encoded, (uint8_t*)&params, sizeof(params));
    fprintf(stdout, "#!/bin/sh\n");
    fprintf(stdout, "# to test your new tonecurve, copy/paste the following line into your shell.\n");
    fprintf(stdout, "# note that it is a smart idea to backup your database before messing with it on this level.\n\n");
    fprintf(stdout, "echo \"INSERT INTO presets (name,description,operation,op_version,op_params,enabled,blendop_params,blendop_version,multi_priority,multi_name,model,maker,lens,iso_min,iso_max,exposure_min,exposure_max,aperture_min,aperture_max,focal_length_min,focal_length_max,writeprotect,autoapply,filter,def,format) VALUES('%s','','tonecurve',%d,X'%s',1,X'00000000180000000000C842000000000000000000000000000000000000000000000000000000000000000000000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F',7,0,'','%%','%%','%%',0.0,340282346638528859812000000000000000000,0.0,10000000.0,0.0,100000000.0,0.0,1000.0,0,0,0,0,2);\" | sqlite3 ~/.config/darktable/data.db\n",
            opt.filename_exif ? model : "new measured tonecurve",
            TONECURVE_PARAMS_VERSION, encoded);
    fprintf(stdout, "\n\n\n"
                    "# if it pleases you, then in iop/tonecurve.c append the following line to the array preset_camera_curves and modify its name\n"
                    "# {\"%s\", \"%s\", \"%s\", 0, FLT_MAX, {{",
                    opt.filename_exif ? model : "new measured tonecurve",
                    opt.filename_exif ? maker : "<MAKER>",
                    opt.filename_exif ? model : "<MODEL>");
    for (int i=0; i<3; i++)
    {
      fprintf(stdout, "{");
      for(int k=0;k<params.tonecurve_nodes[i];k++)
      {
        fprintf(stdout, "{%f, %f}%s", params.tonecurve[i][k].x, params.tonecurve[i][k].y, params.tonecurve_nodes[i]-1?", ":"");
      }
      fprintf(stdout, "},");
    }
    fprintf(stdout, "}, {%d, %d, %d}, {%d, %d, %d}, %d, 0, %d}},\n",
      params.tonecurve_nodes[0], params.tonecurve_nodes[1], params.tonecurve_nodes[2],
      params.tonecurve_type[0], params.tonecurve_type[1], params.tonecurve_type[2],
      params.tonecurve_autoscale_ab, params.tonecurve_unbound_ab);
  }

exit:
  if (f)
  {
    fclose(f);
    f = NULL;
  }
  if (raw_buff) {
    free(raw_buff);
    raw_buff = NULL;
  }
  if (jpeg_buff) {
    free(jpeg_buff);
    jpeg_buff = NULL;
  }
  if (raw_buff_f) {
    free(raw_buff_f);
    raw_buff_f = NULL;
  }
  if (jpeg_buff_f) {
    free(jpeg_buff_f);
    jpeg_buff_f = NULL;
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
  if (hist)
  {
    free(hist);
    hist = NULL;
  }

  return ret;
}

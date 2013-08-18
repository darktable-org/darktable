#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "../../src/common/curve_tools.c"

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

static inline float get_error(CurveData *c, CurveSample *csample, float (*basecurve)[3], int (*cnt)[3])
{
  CurveDataSample(c, csample);
  float sqrerr = 0.0f;
  const float max = 1.0f, min = 0.0f;
  for(int k=0; k<0x10000; k++)
  {
    // too few samples? no error if we ignore it.
    if(cnt[k][1] > 8)
    {
      float d = (basecurve[k][1] - (min + (max-min)*csample->m_Samples[k]*(1.0f/0x10000)));
      // way more error for lower values of x:
      d *= 0x10000-k;
      if(k < 655) d *= 100;
      sqrerr += d*d;
    }
  }
  return sqrerr;
}

static inline void mutate(CurveData *c, CurveData *t, float (*basecurve)[3])
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
    t->m_anchors[k].y = basecurve[pos][1];
  }
  t->m_anchors[0].x = 0.0f;
  t->m_anchors[0].y = 0.0f;//basecurve[0][1];
  t->m_anchors[t->m_numAnchors-1].x = 1.0f;
  t->m_anchors[t->m_numAnchors-1].y = 1.0f;//basecurve[0xffff][1];
}


int main(int argc, char *argv[])
{
  if(argc < 3)
  {
    fprintf(stderr, "usage: %s inputraw.ppm (16-bit) inputjpg.ppm (8-bit) [num_nodes]\n", argv[0]);
    fprintf(stderr, "convert the raw with `dcraw -6 -W -g 1 1 -w input.raw'\n");
    fprintf(stderr, "and the jpg with `convert input.jpg output.ppm'\n");
    fprintf(stderr, "plot the results with `gnuplot plot'\n");
    exit(1);
  }
  FILE *fb = fopen("basecurve.dat", "wb");
  FILE *ff = fopen("fit.dat", "wb");
  if(!fb || !ff)
  {
    fprintf(stderr, "could not open `basecurve.dat' or `fit.dat'\n");
    exit(1);
  }
  int num_nodes = 8;
  if(argc > 3)
    num_nodes = atol(argv[3]);
  if(num_nodes > 20) num_nodes = 20; // basecurve doesn't support more than that.
  int wd, ht, jpgwd, jpght;
  uint16_t *img = read_ppm16(argv[1], &wd, &ht);
  if(!img) exit(1);
  // swap silly byte order
  for(int k=0;k<3*wd*ht;k++) img[k] = ((img[k]&0xff) << 8) | (img[k] >> 8);
  uint8_t *jpg = read_ppm8(argv[2], &jpgwd, &jpght);
  if(!jpg) exit(1);

  // basecurve is three-channel rgb and has 16-bit entries (raw) and 8-bit output (jpg).
  // the output is averaged a couple of times, so we want to keep it as floats.
  float basecurve[0x10000][3];
  // keep a counter of how many samples are in each bin.
  int cnt[0x10000][3];
  memset(basecurve, 0, sizeof(basecurve));
  memset(cnt, 0, sizeof(cnt));

  int offx = (wd - jpgwd)/2;
  int offy = (ht - jpght)/2;
  if(offx < 0 || offy < 0)
  {
    fprintf(stderr, "jpeg is higher resolution than the raw? (%dx%d vs %dx%d)\n", jpgwd, jpght, wd, ht);
    fclose(fb);
    fclose(ff);
    exit(1);
  }

  for(int j=0;j<jpght;j++)
  {
    for(int i=0;i<jpgwd;i++)
    {
      // raw coordinate is offset:
      const int ri = offx + i, rj = offy + j;
      for(int k=0;k<3;k++)
      {
        // un-gamma the jpg file:
        float rgb = jpg[3*(jpgwd*j + i) + k]/255.0f;
        if(rgb < 0.04045f) rgb /= 12.92f;
        else rgb = powf(((rgb + 0.055f)/1.055f), 2.4f);
        // grab the raw color at this position:
        const uint16_t raw = img[3*(wd*rj + ri) + k];
        // accum the histogram:
        basecurve[raw][k] = (basecurve[raw][k]*cnt[raw][k] + rgb)/(cnt[raw][k] + 1.0f);
        cnt[raw][k]++;
      }
    }
  }

  // output the histograms:
  fprintf(fb, "# basecurve-red basecurve-green basecurve-blue basecurve-avg cnt-red cnt-green cnt-blue\n");
  for(int k=0;k<0x10000;k++)
  {
    fprintf(fb, "%f %f %f %f %d %d %d\n", basecurve[k][0], basecurve[k][1], basecurve[k][2], (basecurve[k][0] + basecurve[k][1] + basecurve[k][2])/3.0f, cnt[k][0], cnt[k][1], cnt[k][2]);
  }

  free(img);
  free(jpg);

  // now do the fitting:
  CurveData curr, tent, best;
  CurveSample csample;
  const int res = 0x10000;
  csample.m_samplingRes = res;
  csample.m_outputRes = 0x10000;
  csample.m_Samples = (uint16_t *)malloc(sizeof(uint16_t)*0x10000);

  // type = 2 (monotone hermite)
  curr.m_spline_type = 2;
  curr.m_numAnchors = num_nodes;
  curr.m_min_x = 0.0;
  curr.m_max_x = 1.0;
  curr.m_min_y = 0.0;
  curr.m_max_y = 1.0;

  best = tent = curr;

  float min = FLT_MAX;
  const float p_large = .0f;
  float curr_m = FLT_MIN;
  int accepts = 0;

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
        tent.m_anchors[k].y = basecurve[pos][1];
      }
    }
    else
    { // mutate
      mutate(&curr, &tent, basecurve);
    }
    float m = get_error(&tent, &csample, basecurve, cnt);
    if(m < min)
    {
      accepts ++;
      best = tent;
      min = m;
    }
    // fittness: 1/MSE
    const float a = curr_m/m;
    if(drand48() < a || i == 0)
    { // accept new state
      curr = tent;
      curr_m = m;
    }
  }

  // our best state is in `best'

  fprintf(ff, "# err %f improved %d times\n", min, accepts);
  fprintf(ff, "# copy paste into iop/basecurve.c (be sure to insert name, maker, model, and set the last 0 to 1 if happy to filter it):\n");
  fprintf(ff, "# { \"new measured basecurve\", \"insert maker\", \"insert model\", 0, 51200,                        {{{");
  for(int k=0;k<best.m_numAnchors;k++)
    fprintf(ff, "{%f, %f}%s", best.m_anchors[k].x, best.m_anchors[k].y, k<best.m_numAnchors-1?", ":"}}, ");
  fprintf(ff, "{%d}, {m}}, 0, 0},\n", best.m_numAnchors);
  CurveDataSample(&best, &csample);
  for(int k=0; k<0x10000; k++)
    fprintf(ff, "%f %f\n", k*(1.0f/0x10000), 0.0 + (1.0f-0.0f)*csample.m_Samples[k]*(1.0f/0x10000));

  fclose(fb);
  fclose(ff);

  exit(0);
}

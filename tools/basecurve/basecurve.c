#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

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


int main(int argc, char *argv[])
{
  if(argc < 3)
  {
    fprintf(stderr, "usage: %s inputraw.ppm (16-bit) inputjpg.ppm (8-bit)\n", argv[0]);
    fprintf(stderr, "convert the raw with `dcraw -6 -W -g 1 1 -w input.raw'\n");
    fprintf(stderr, "and the jpg with `convert input.jpg output.ppm'\n");
    exit(1);
  }
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
  fprintf(stdout, "# basecurve-red basecurve-green basecurve-blue basecurve-avg cnt-red cnt-green cnt-blue\n");
  for(int k=0;k<0x10000;k++)
  {
    fprintf(stdout, "%f %f %f %f %d %d %d\n", basecurve[k][0], basecurve[k][1], basecurve[k][2], (basecurve[k][0] + basecurve[k][1] + basecurve[k][2])/3.0f, cnt[k][0], cnt[k][1], cnt[k][2]);
  }

  free(img);
  free(jpg);
  exit(0);
}

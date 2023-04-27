#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "../../src/common/dttypes.h"

typedef float elem_type;
#define ELEM_SWAP(a,b) { elem_type t=(a);(a)=(b);(b)=t; }

/*---------------------------------------------------------------------------
Function :   kth_smallest()
In       :   array of elements, # of elements in the array, rank k
Out      :   one element
Job      :   find the kth smallest element in the array
Notice   :   use the median() macro defined below to get the median.

Reference:

Author: Wirth, Niklaus
Title: Algorithms + data structures = programs
Publisher: Englewood Cliffs: Prentice-Hall, 1976
Physical description: 366 p.
Series: Prentice-Hall Series in Automatic Computation

---------------------------------------------------------------------------*/

static elem_type
kth_smallest(elem_type a[], int n, int k)
{
  int i,j,l,m ;
  elem_type x ;

  l=0 ; m=n-1 ;
  while (l<m) {
    x=a[2*k+1] ;
    i=l ;
    j=m ;
    do {
      while (a[2*i+1]<x) i++ ;
      while (x<a[2*j+1]) j-- ;
      if (i<=j) {
        ELEM_SWAP(a[2*i+1],a[2*j+1]) ;
        i++ ; j-- ;
      }
    } while (i<=j) ;
    if (j<k) l=i ;
    if (k<i) m=j ;
  }
  return a[2*k+1] ;
}

#define median(a,n) kth_smallest(a,n,(((n)&1)?((n)/2):(((n)/2)-1)))



static float*
read_pfm(const char *filename, int *wd, int*ht)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return 0;
  (void) fscanf(f, "PF\n%d %d\n%*[^\n]", wd, ht);
  fgetc(f); // eat only one newline

  float *p = (float *)malloc(sizeof(float)*3*(*wd)*(*ht));
  (void) fread(p, sizeof(float)*3, (*wd)*(*ht), f);
  for(int k=0;k<3*(*wd)*(*ht);k++) p[k] = fmaxf(0.0f, p[k]);
  fclose(f);
  return p;
}

static float*
read_histogram(const char *filename, int *bins)
{
  *bins = 0;
  FILE *f = fopen(filename, "rb");
  if(!f) return 0;

  while(!feof(f))
  {
    (void) fscanf(f, "%*f %*f %*f %*f %*f %*f %*f %*f %*f %*f\n");
    (*bins) ++;
  }
  fseek(f, 0, SEEK_SET);
  // second round, alloc and read
  float *hist = (float *)malloc(sizeof(float) * 3 * (*bins));
  int k=0;
  while(!feof(f))
  {
    (void) fscanf(f, "%*f %*f %*f %*f %*f %*f %*f %f %f %f\n", hist + 3*k, hist+3*k+1, hist+3*k+2);
    k++;
  }

  fclose(f);
  return hist;
}

static void
invert_histogram(
    const float *const hist,
    float *const inv_hist,
    const int bins)
{
  // invert non-normalised accumulated hist
  for(int c=0;c<3;c++)
  {
    int last = 0;
    for(int i=last+1; i<bins; i++)
    {
      for(int k=last; k<bins; k++)
      {
        if(hist[3*k+c] >= i/(float)bins)
        {
          last = k;
          inv_hist[3*i+c] = k/(float)bins;
          break;
        }
      }
    }
  }
}

#if 0
static void
write_pfm(const char *filename, float *buf, int wd, int ht)
{
  FILE *f = fopen(filename, "wb");
  if(!f) return;
  fprintf(f, "PF\n%d %d\n-1.0\n", wd, ht);
  fwrite(buf, sizeof(float)*3, wd*ht, f);
  fclose(f);
}
#endif


#define MIN(a,b) ((a>b)?b:a)
#define MAX(a,b) ((a>b)?a:b)

#define N 300

static inline float
clamp(float f, float m, float M)
{
  return MAX(MIN(f, M), m);
}

int compare_llhh(const void *a, const void *b)
{
  return (int)clamp(((float *)a)[0]*N, 0, N-1) - (int)clamp(((float *)b)[0]*N, 0, N-1);
}

int main(int argc, char *arg[])
{
  if(argc < 2)
  {
    fprintf(stderr, "usage: %s input.pfm [-c a1 b1]\n", arg[0]);
    exit(1);
  }
  int wd, ht;
  float *input = read_pfm(arg[1], &wd, &ht);
  float max = 0.0f;
  // sanity checks:
  // for(int k=0;k<3*wd*ht;k++) input[k] = clamp(input[k], 0.0f, 1.0f);

  // correction requested?
  if(argc >= 9 && !strcmp(arg[2], "-c"))
  {
    const dt_aligned_pixel_t a = { atof(arg[3]), atof(arg[4]), atof(arg[5]) },
                             b = { atof(arg[6]), atof(arg[7]), atof(arg[8]) };
    // const float m[3] = {1, 1, 1};
    //   2.0f*sqrt(a[0]*1.0f+b[0])/a[0],
    //   2.0f*sqrt(a[1]*1.0f+b[1])/a[1],
    //   2.0f*sqrt(a[2]*1.0f+b[2])/a[2]};
#if 1
    // dump curves:
    for(int k=0;k<N;k++)
    {
      for(int c=0;c<3;c++)
      {
        // const float y = k/(N-1.0f);
        // const float x = m[c]*m[c]*a[c]*y*y/4.0f - b[c]/a[c];
        float x = k/(N-1.0f)/a[c];
        const float d = fmaxf(0.0f, x + 3./8. + (b[c]/a[c])*(b[c]/a[c]));
        x = 2.0f*sqrtf(d);
        fprintf(stderr, "%f ", x);
      }
      fprintf(stderr, "\n");
    }
#endif
    for(int k=0;k<wd*ht;k++)
    {
      for(int c=0;c<3;c++)
      {
        // input[3*k+c] = 2.0f*sqrtf(a[c]*input[3*k+c]+b[c])/(a[c]*m[c]);
        input[3*k+c] = input[3*k+c] / a[c];
        const float d = fmaxf(0.0f, input[3*k+c] + 3./8. + (b[c]/a[c])*(b[c]/a[c]));
        input[3*k+c] = 2.0f*sqrtf(d);
        max = fmaxf(max, input[3*k+c]);
      }
    }
    for(int k=0;k<3*wd*ht;k++) input[k] /= max;
  }
  else if(argc >= 4 && !strcmp(arg[2], "-h"))
  {
    int bins = 0;
    float *hist = read_histogram(arg[3], &bins);
    float *inv_hist = (float *)malloc(sizeof(float) * 3 * bins);
    invert_histogram(hist, inv_hist, bins);
#if 1
    // output curves and their inverse:
    for(int k=0;k<bins;k++)
      // fprintf(stderr, "%f %f %f %f %f %f %f\n", k/(float)bins, hist[3*k], hist[3*k+1], hist[3*k+2], inv_hist[3*k], inv_hist[3*k+1], inv_hist[3*k+2]);
      fprintf(stderr, "%f %f %f\n", inv_hist[3*k], inv_hist[3*k+1], inv_hist[3*k+2]);
    // fprintf(stderr,"scanned %d bins\n", bins);
#endif
    for(int k=0;k<wd*ht;k++)
    {
      for(int c=0;c<3;c++)
      {
        float f = clamp(input[3*k+c]*bins, 0, bins-2);
        const int bin = (int)f;
        f -= bin;
        input[3*k+c] = (1.0f-f)*inv_hist[3*bin+c] + f*inv_hist[3*(bin+1)+c];
      }
    }
  }

  float std[N][3] = {{0.0f}};
  float cnt[N][3] = {{0.0f}};

  // one level haar decomposition, separable, decimated, lifting scheme
  for(int j=0;j<ht;j++)
  {
    for(int i=0;i<wd-1;i+=2)
    {
      float *buf = input + 3*(wd*j + i);
      for(int c=0;c<3;c++)
      {
        buf[c] += buf[3+c];
        buf[c] *= .5f;
        buf[3+c] -= buf[c];
      }
      // buf += 3;
    }
  }
  for(int i=0;i<wd;i++)
  {
    for(int j=0;j<ht-1;j+=2)
    {
      float *buf = input + 3*(wd*j + i);
      for(int c=0;c<3;c++)
      {
        buf[c] += buf[3*wd+c];
        buf[c] *= .5f;
        buf[3*wd+c] -= buf[c];
      }
      // buf += 3*wd;
    }
  }

#if 0
  // debug: write full wavelet transform:
  write_pfm("wt.pfm", input, wd, ht);
  // debug: write LL
  float *out = (float *)malloc(sizeof(float)*3*wd/2*ht/2);
  for(int j=0;j<ht-1;j+=2)
  {
    for(int i=0;i<wd-1;i+=2)
    {
      for(int c=0;c<3;c++)
      {
        out[3*((wd/2)*(j/2)+(i/2))+c] = input[3*(wd*j+i)+c];
      }
    }
  }
  write_pfm("LL.pfm", out, wd/2, ht/2);
  free(out);
#endif

  // sort pairs (LL,HH) for each color channel:
  float *llhh = (float *)malloc(sizeof(float)*wd*ht/2);
  for(int c=0;c<3;c++)
  {
    int k = 0;
    for(int j=0;j<ht-1;j+=2)
    {
      for(int i=0;i<wd-1;i+=2)
      {
        llhh[2*k]   = input[3*(wd*j+i)+c];
        llhh[2*k+1] = fabsf(input[3*(wd*(j+1)+(i+1))+c]);
        k++;
      }
    }
    qsort(llhh, k, 2*sizeof(float), compare_llhh);
    // estimate std deviation for every bin we've got:
    for(int begin=0;begin<k;)
    {
      // LL is used to estimate brightness:
      const int bin = (int)clamp(llhh[2*begin]*N, 0, N-1);
      int end = begin+1;
      while((end < k) && ((int)clamp(llhh[2*end]*N, 0, N-1) == bin))
        end++;
      assert(end >= k || bin <= (int)clamp(llhh[2*end]*N, 0, N-1));
      // fprintf(stderr, "from %d (%d) -- %d (%d)\n", begin, bin, end, (int)clamp(llhh[2*end]*N, 0, N-1));

      // estimate noise by robust statistic (assumes zero mean of HH band):
      // MAD: median(|Y - med(Y)|) = 0.6745 sigma
      // if(end - begin > 10)
        // fprintf(stdout, "%d %f %d\n", bin, median(llhh+2*begin, end-begin)/0.6745, end - begin);
      std[bin][c] += median(llhh+2*begin, end-begin)/0.6745;
      cnt[bin][c] = end - begin;

      begin = end;
    }
  }

#if 0
  // recover noise curve:
  for(int k=0;k<wd*ht;k++)
  {
    for(int c=0;c<3;c++)
    {
      const int i = clamp(ref[3*k+c]*N, 0, N-1);
      cnt[i][c] ++;
      const float diff = input[3*k+c] - ref[3*k+c];
      // assume zero mean:
      var[i][c] += diff*diff; // - E(X^2)
    }
  }
#endif
#if 0

  // normalize
  for(int i=0;i<N;i++)
    for(int c=0;c<3;c++)
      if(cnt[i][c] > 0.0f)
        std[i][c] /= cnt[i][c];
      else
        std[i][c] = 0.0f;
#endif

  // scale back in case we needed to bin it down:
  if(max > 0.0f)
    for(int i=0;i<N;i++)
      for(int k=0;k<3;k++) std[i][k] *= max;
  // output variance per brightness level:
  // fprintf(stdout, "# bin std_r std_g std_b hist_r hist_g hist_b cdf_r cdf_g cdf_b\n");
  dt_aligned_pixel_t sum = {0.0f};
  for(int i=0;i<N;i++)
    for(int k=0;k<3;k++) sum[k] += std[i][k];
  dt_aligned_pixel_t cdf = {0.0f};
  for(int i=0;i<N;i++)
  {
    fprintf(stdout, "%f %f %f %f %f %f %f %f %f %f\n", i/(float)N, std[i][0], std[i][1], std[i][2],
        cnt[i][0], cnt[i][1], cnt[i][2],
        cdf[0]/sum[0], cdf[1]/sum[1], cdf[2]/sum[2]);
        // cdf[0], cdf[1], cdf[2]);
    for(int k=0;k<3;k++) cdf[k] += std[i][k];
  }

  free(llhh);
  free(input);
  exit(0);
}


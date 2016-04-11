#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tonecurve.h"
#include "thinplate.h"

static inline int read_spec(
    const char *filename,
    double **target_L_ptr,
    double **target_a_ptr,
    double **target_b_ptr,
    double **reference_Lab_ptr)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return -1;
  int N = 0;
  int r = 0;
  while(fscanf(f, "%*[^\n]\n") != EOF) N++;
  fseek(f, 0, SEEK_SET);

  double *target_L = (double *)calloc(sizeof(double),(N+4));
  double *target_a = (double *)calloc(sizeof(double),(N+4));
  double *target_b = (double *)calloc(sizeof(double),(N+4));
  double *reference_Lab = (double *)calloc(3*sizeof(double),N);
  *target_L_ptr = target_L;
  *target_a_ptr = target_a;
  *target_b_ptr = target_b;
  *reference_Lab_ptr = reference_Lab;

  for(int i=0;i<N;i++)
  {
    r += fscanf(f, "%*s %lg %lg %lg %lg %lg %lg\n",
        reference_Lab+3*i, reference_Lab+3*i+1, reference_Lab+3*i+2,
        target_L+i, target_a+i, target_b+i);
  }

  if(r == 0) fprintf(stderr, "just keeping compiler happy\n");
  fclose(f);
  return N;
}


int main(int argc, char *argv[])
{
  double *target_L;
  double *target_a;
  double *target_b;
  double *colorchecker_Lab;
  if(argc < 2)
  {
    fprintf(stderr, "usage: %s input_spec.csv\n", argv[0]);
    exit(1);
  }

  const int N = read_spec(
      argv[1],
      &target_L, &target_a, &target_b,
      &colorchecker_Lab);
  if(N <= 0)
  {
    fprintf(stderr, "failed to read patches!\n");
    exit(1);
  }

  char *basename = argv[1];
  char *c = basename + strlen(basename);
  while(*c!='.'&&c>basename)c--;
  if(c>basename) *c=0;

  fprintf(stderr, "read %d patches\n", N);

  // extract tonecurve first:
  int num_tonecurve = (N==24?6 : (N==288?24 : 0)) + 2; // 24+2 for it8, 6+2 for colorchecker
  tonecurve_t tonecurve;
  double cx[num_tonecurve], cy[num_tonecurve];
  cx[0] = cy[0] = 0.0;                               // fix black
  cx[num_tonecurve-1] = cy[num_tonecurve-1] = 100.0; // fix white
  for(int k=1;k<num_tonecurve-1;k++) cx[num_tonecurve-1-k] = colorchecker_Lab[3*(N-num_tonecurve+2+k-1)];
  for(int k=1;k<num_tonecurve-1;k++) cy[num_tonecurve-1-k] = target_L[N-num_tonecurve+2+k-1];
  tonecurve_create(&tonecurve, cx, cy, num_tonecurve);

  for(int k=0;k<num_tonecurve;k++) fprintf(stderr, "L[%g] = %g\n", 100.0 * k/(num_tonecurve-1.0f),
      tonecurve_apply(&tonecurve, 100.0f * k/(num_tonecurve-1.0f)));

  // unapply from target data, we will apply it later in the pipe
  // and want to match the colours only:
  for(int k=0;k<N;k++)
    target_L[k] = tonecurve_unapply(&tonecurve, target_L[k]);

  int sparsity = 28;
  const double *target[3] = {target_L, target_a, target_b};
  double coeff_L[N+4], coeff_a[N+4], coeff_b[N+4];
  double *coeff[] = {coeff_L, coeff_a, coeff_b};
  int perm[N+4];
  thinplate_match(&tonecurve, 3, N, colorchecker_Lab, target, sparsity, perm, coeff);

  int sp = 0;
  int cperm[300];
  for(int k=0;k<sparsity;k++)
    if(perm[k] < N) // skip polynomial parts
      cperm[sp++] = perm[k];

  fprintf(stderr, "found %d basis functions:\n", sp);
  for(int k=0;k<sp;k++)
    fprintf(stderr, "perm[%d] = %d source %g %g %g\n", k, cperm[k],
       colorchecker_Lab[3*cperm[k]],
       colorchecker_Lab[3*cperm[k]+1],
       colorchecker_Lab[3*cperm[k]+2]);

  // output
  tonecurve_dump_preset(&tonecurve, basename);
  thinplate_dump_preset(basename, sp, colorchecker_Lab, target, cperm);

  free(target_L);
  free(target_a);
  free(target_b);
  free(colorchecker_Lab);

  return 0;
}

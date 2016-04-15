#pragma once

#ifndef MAX
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#endif
#ifndef MIN
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#endif
#include "../../src/iop/svd.h"
#include "../../src/lut/deltaE.h"
#include "tonecurve.h"
#include <float.h>

#define REPLACEMENT // either broken code or doesn't help at all
#define EXACT       // use full solve instead of dot in inner loop

// thinplate spline kernel \phi(r)
static inline double thinplate_kernel(const double *x, const double *y)
{
  const double r = sqrt(
      (x[0]-y[0])*(x[0]-y[0])+
      (x[1]-y[1])*(x[1]-y[1])+
      (x[2]-y[2])*(x[2]-y[2]));
  return r*r*log(MAX(1e-10, r));
}

static inline double compute_error(
    const tonecurve_t *c,
    const double **target,
    const double *residual_L,
    const double *residual_a,
    const double *residual_b,
    const int wd)
{
  // compute error:
  double err = 0.0;
  for(int i=0;i<wd;i++)
  {
#if 0 // max rmse
    // err = MAX(err, residual_L[i]*residual_L[i] +
    //     residual_a[i]*residual_a[i] + residual_b[i]*residual_b[i]);
#elif 0 // total rmse
    err += (residual_L[i]*residual_L[i] +
        residual_a[i]*residual_a[i] + residual_b[i]*residual_b[i])/wd;
#elif 1 // delta e 2000
    const double Lt = target[0][i];
    const double L0 = tonecurve_apply(c, Lt);
    const double L1 = tonecurve_apply(c, Lt + residual_L[i]);
    float Lab0[3] = {L0, target[1][i], target[2][i]};
    float Lab1[3] = {L1, target[1][i], target[2][i]};
    err += dt_colorspaces_deltaE_2000(Lab0, Lab1);
#else
    const double Lt = target[0][i];
    const double L = tonecurve_apply(c, Lt + residual_L[i]);
    const double dL = L - tonecurve_apply(c, Lt);
    err = MAX(err, dL*dL +
        residual_a[i]*residual_a[i] + residual_b[i]*residual_b[i]);
#endif
  }
  return sqrt(err);
}

static inline int solve(
    double *As,
    double *w,
    double *v,
    const double *b,
    double *coeff,
    int wd,
    int s,
    int S)
{
  // A'[wd][s+1] = u[wd][s+1] diag(w[s+1]) v[s+1][s+1]^t
  //
  // svd to solve for c:
  // A * c = b
  // A = u w v^t => A-1 = v 1/w u^t
  dsvd(As, wd, s+1, S, w, v); // As is wd x s+1 but row stride S.
  if(w[s] < 1e-3) // if the smallest singular value becomes too small, we're done
    return 1;
  double tmp[S];
  for(int i=0;i<=s;i++) // compute tmp = u^t * b
  {
    tmp[i] = 0.0;
    for(int j=0;j<wd;j++)
      tmp[i] += As[j*S+i] * b[j];
  }
  for(int i=0;i<=s;i++) // apply singular values:
    tmp[i] /= w[i];
  for(int j=0;j<=s;j++)
  { // compute first s output coefficients coeff[j]
    coeff[j] = 0.0;
    for(int i=0;i<=s;i++)
      coeff[j] += v[j*(s+1)+i] * tmp[i];
  }
  return 0;
}

// returns sparsity <= S
static inline int thinplate_match(
    const tonecurve_t *curve, // tonecurve to apply after this (needed for error estimation)
    int dim,                  // dimensionality of points
    int N,                    // number of points
    const double *point,      // dim-strided points
    const double **target,    // target values, one pointer per dimension
    int S,                    // desired sparsity level, actual result will be returned
    int *permutation,         // pointing to original order of points, to identify correct output coeff
    double **coeff)           // output coefficient arrays for each dimension, ordered according to permutation[dim]
{
  const int wd = N+4;
  double A[wd*wd];
  // construct system matrix A such that:
  // A c = f
  //
  // | R   P | |c| = |f|
  // | P^t 0 | |d|   |0|
  //
  // to interpolate values f_i with the radial basis function system matrix R and a polynomial term P.
  // P is a 3D linear polynomial a + b x + c y + d z
  //
  // radial basis function part R
  for(int j=0;j<N;j++)
    for(int i=j;i<N;i++)
      A[j*wd+i] = A[i*wd+j] = thinplate_kernel(point+3*i, point+3*j);

  // polynomial part P: constant + 3x linear
  for(int i=0;i<N;i++) A[i*wd+N+0] = A[(N+0)*wd+i] = 1.0f;
  for(int i=0;i<N;i++) A[i*wd+N+1] = A[(N+1)*wd+i] = point[3*i+0];
  for(int i=0;i<N;i++) A[i*wd+N+2] = A[(N+2)*wd+i] = point[3*i+1];
  for(int i=0;i<N;i++) A[i*wd+N+3] = A[(N+3)*wd+i] = point[3*i+2];

  for(int j=N;j<wd;j++) for(int i=N;i<wd;i++) A[j*wd+i] = 0.0f;

  // precompute normalisation factors for columns of A
  double norm[wd];
  for(int i=0;i<wd;i++)
  {
    norm[i] = 0.0;
    for(int j=0;j<wd;j++) norm[i] += A[j*wd+i]*A[j*wd+i];
    norm[i] = 1.0/sqrt(norm[i]);
  }

  // XXX do we need these explicitly?
  // residual = target vector
  double r[dim][wd];
  const double *b[dim];
  for(int k=0;k<dim;k++) b[k] = target[k];
  for(int k=0;k<dim;k++) memcpy(r[k], b[k], sizeof(r[0]));

  double w[S], v[S*S], As[wd*S];
  memset(As, 0, sizeof(As));
  // for rank from 0 to sparsity level
  int s = 0, patches = 0;
  double olderr = FLT_MAX;
  // in case of replacement, iterate all the way to wd
  for(;s<wd;s++)
  {
#ifndef REPLACEMENT
    if(patches >= S-4) return sparsity;
#endif
    const int sparsity = MIN(s, S-1);
    // find column a_m by m = argmax_t{ a_t^t r . norm_t}
    // by searching over all three residuals
    double maxdot = 0.0;
    int maxcol = 0;
    for(int t=0;t<wd;t++)
    {
      double dot = 0.0;
      if(norm[t] > 0.0)
      {
#ifdef EXACT // use full solve
        permutation[sparsity] = t;
        for(int ch=0;ch<dim;ch++)
        {
          // re-init columns in As
          // svd will destroy its contents:
          for(int i=0;i<=sparsity;i++) for(int j=0;j<wd;j++)
            As[j*S + i] = A[j*wd+permutation[i]];

          if(solve(As, w, v, b[ch], coeff[ch], wd, sparsity, S))
            return sparsity;

          // compute tentative residual:
          // r = b - As c
          for(int j=0;j<wd;j++)
          {
            r[ch][j] = b[ch][j];
            for(int i=0;i<=sparsity;i++)
              r[ch][j] -= A[j*wd + permutation[i]] * coeff[ch][i];
          }
        }

        // compute error:
        const double err = compute_error(curve, target,
            r[0], r[1], r[2], wd);
        dot = 1./err; // searching for smallest error or largest dot
#else // use dot product
        for(int ch=0;ch<dim;ch++)
        {
          double chdot = 0.0;
          for(int j=0;j<wd;j++)
            chdot += A[j*wd+t] * r[ch][j];
          dot += fabs(chdot);
        }
        dot *= norm[t];
#endif
      }
      // fprintf(stderr, "dot %d = %g\n", i, dot);
      if(dot > maxdot)
      {
        maxcol = t;
        maxdot = dot;
      }
    }

    if(patches < S-4)
    {
      // remember which column that was, we'll need it to evaluate later:
      permutation[s] = maxcol;
      if(maxcol < N) patches++;
      // make sure we won't choose it again:
      norm[maxcol] = 0.0;
    }
    else
    { // already have S columns, now do the replacement:
      int mincol = 0;
      double minerr = FLT_MAX;
      for(int t=0;t<S;t++)
      {
        // find already chosen column t with min error reduction when replacing
        // permutation[t] = maxcol
        int oldperm = permutation[t];
        permutation[t] = maxcol;
#ifdef EXACT
        for(int ch=0;ch<dim;ch++)
        {
          // re-init all columns in As
          for(int i=0;i<=sparsity;i++) for(int j=0;j<wd;j++)
            As[j*S + i] = A[j*wd+permutation[i]];

          if(solve(As, w, v, b[ch], coeff[ch], wd, sparsity, S))
            return s;

          // compute tentative residual:
          // r = b - As c
          for(int j=0;j<wd;j++)
          {
            r[ch][j] = b[ch][j];
            for(int i=0;i<=sparsity;i++)
              r[ch][j] -= A[j*wd + permutation[i]] * coeff[ch][i];
          }
        }

        // compute error:
        const double err = compute_error(curve, target,
            r[0], r[1], r[2], wd);
#else
        double dot = 0.0;
        for(int ch=0;ch<dim;ch++)
        {
          double chdot = 0.0;
          for(int j=0;j<wd;j++)
            chdot += A[j*wd+t] * r[ch][j];
          dot += fabs(chdot);
        }
        double n = 0.0; // recompute column norm
        for(int j=0;j<wd;j++) n += A[j*wd+mincol]*A[j*wd+mincol];
        dot *= n;
        double err = 1./dot;
#endif

        if(err < minerr)
        {
          mincol = t;
          minerr = err;
        }
        permutation[t] = oldperm;
      }
      if(minerr >= 1./maxdot) return sparsity+1;
      // replace column
      permutation[mincol] = maxcol;
      // reset norm[] of discarded column to something > 0
#ifdef EXACT
      norm[mincol] = 1.0;
#else
      norm[mincol] = 0.0;
      for(int j=0;j<wd;j++) norm[mincol] += A[j*wd+mincol]*A[j*wd+mincol];
      norm[mincol] = 1.0/sqrt(norm[mincol]);
#endif
      norm[maxcol] = 0.0;
    }

#ifdef EXACT
    double err = 1./maxdot;
#else
    // solve linear least squares for sparse c for every output channel:
    for(int ch=0;ch<dim;ch++)
    {
      // re-init all of the previous columns in As since
      // svd will destroy its contents:
      for(int i=0;i<=sparsity;i++) for(int j=0;j<wd;j++)
        As[j*S + i] = A[j*wd+permutation[i]];

      if(solve(As, w, v, b[ch], coeff[ch], wd, sparsity, S))
        return sparsity;

      // compute new residual:
      // r = b - As c
      for(int j=0;j<wd;j++)
      {
        r[ch][j] = b[ch][j];
        for(int i=0;i<=sparsity;i++)
          r[ch][j] -= A[j*wd + permutation[i]] * coeff[ch][i];
      }
    }

    const double err = compute_error(curve, target,
        r[0], r[1], r[2], wd);
#endif
    // residual is max CIE76 delta E now
    // everything < 2 is usually considired a very good approximation:
    fprintf(stderr, "rank %d/%d error DE %g\n", sparsity+1, patches, err);
    if(s>=S && err >= olderr) return sparsity+1;
    if(err < 2.0) return sparsity+1;
    olderr = err;
  }
  return -1;
}

static inline float _thinplate_color_pos(
    float L, float a, float b)
{
  const float pi = 3.14153f; // clearly true.
  const float h = atan2f(b, a) + pi;
  // const float C = sqrtf(a*a + b*b);
  const int sector = 4.0f * h / (2.0f * pi);
  return 256.0*sector + L;//C;
}

static inline void thinplate_dump_preset(
    const char *name,
    int num,                // number of points
    const double *point,    // dim-strided points
    const double **target,  // target values, one pointer per dimension
    int *permutation)       // pointing to original order of points, to identify correct output coeff
{
  // hardcoded v2 of the module
#define MAX_PATCHES 50
  typedef struct dt_iop_colorchecker_params_t
  {
    float source_L[MAX_PATCHES];
    float source_a[MAX_PATCHES];
    float source_b[MAX_PATCHES];
    float target_L[MAX_PATCHES];
    float target_a[MAX_PATCHES];
    float target_b[MAX_PATCHES];
    int32_t num_patches;
  } dt_iop_colorchecker_params_t;

  dt_iop_colorchecker_params_t params;
  memset(&params, 0, sizeof(params));
  num = MIN(24, num); // XXX currently the gui doesn't fare well with other numbers
  assert(num <= MAX_PATCHES);
  params.num_patches = num;

  for(int k=0;k<num;k++)
  {
    params.source_L[k] = point[3*permutation[k]];
    params.source_a[k] = point[3*permutation[k]+1];
    params.source_b[k] = point[3*permutation[k]+2];
    params.target_L[k] = target[0][permutation[k]];
    params.target_a[k] = target[1][permutation[k]];
    params.target_b[k] = target[2][permutation[k]];
  }

#define SWAP(a,b) {const float tmp=(a); (a)=(b); (b)=tmp;}
  // bubble sort by octant and brightness:
  for(int k=0;k<num-1;k++) for(int j=0;j<num-k-1;j++)
  {
    if(_thinplate_color_pos(params.source_L[j],
          params.source_a[j], params.source_b[j]) <
        _thinplate_color_pos(params.source_L[j+1],
          params.source_a[j+1], params.source_b[j+1]))
    {
      SWAP(params.source_L[j], params.source_L[j+1]);
      SWAP(params.source_a[j], params.source_a[j+1]);
      SWAP(params.source_b[j], params.source_b[j+1]);
      SWAP(params.target_L[j], params.target_L[j+1]);
      SWAP(params.target_a[j], params.target_a[j+1]);
      SWAP(params.target_b[j], params.target_b[j+1]);
    }
  }
#undef SWAP

  char filename[1024];
  snprintf(filename, sizeof(filename), "colorchecker-%s.sh", name);
  FILE *f = fopen(filename, "wb");
  if(!f) return;

  fprintf(f, "#!/bin/sh\n");
  fprintf(f, "# to test your new colour lut, copy/paste the following line into your shell.\n");
  fprintf(f, "# note that it is a smart idea to backup your database before messing with it on this level.\n\n");
  uint8_t encoded[2048];
  hexify(encoded, (uint8_t*)&params, sizeof(params));
  fprintf(f, "echo \"INSERT OR REPLACE INTO presets (name,description,operation,op_version,op_params,enabled,blendop_params,blendop_version,multi_priority,multi_name,model,maker,lens,iso_min,iso_max,exposure_min,exposure_max,aperture_min,aperture_max,focal_length_min,focal_length_max,writeprotect,autoapply,filter,def,format) VALUES('%s','','colorchecker',%d,X'%s',1,X'00000000180000000000C842000000000000000000000000000000000000000000000000000000000000000000000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F00000000000000000000803F0000803F',7,0,'','%%','%%','%%',0.0,51200.0,0.0,10000000.0,0.0,100000000.0,0.0,1000.0,0,0,0,0,2);\" | sqlite3 ~/.config/darktable/library.db\n",
      name,
      2, encoded);
  fclose(f);
}


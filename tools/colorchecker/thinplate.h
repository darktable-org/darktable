#pragma once
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#include "../../src/iop/svd.h"

// thinplate spline kernel \phi(r)
static inline double thinplate_kernel(const double *x, const double *y)
{
  const double r = sqrt(
      (x[0]-y[0])*(x[0]-y[0])+
      (x[1]-y[1])*(x[1]-y[1])+
      (x[2]-y[2])*(x[2]-y[2]));
  return r*r*log(MAX(1e-10, r));
}

// returns sparsity <= S
static inline int thinplate_match(
    int dim,                // dimensionality of points
    int N,                  // number of points
    const double *point,    // dim-strided points
    const double **target,  // target values, one pointer per dimension
    int S,                  // desired sparsity level, actual result will be returned
    int *permutation,       // pointing to original order of points, to identify correct output coeff
    double **coeff)         // output coefficient arrays for each dimension, ordered according to permutation[dim]
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

  double w[S], v[S*S], tmp[S], As[wd*S];
  memset(As, 0, sizeof(As));
  // for rank from 0 to sparsity level
  int s = 0;
  for(;s<S;s++)
  {
    // find column a_m by m = argmax_i{ a_i^t r . norm_i}
    // by searching over all three residuals
    double maxdot = 0.0;
    int maxcol = 0;
    for(int i=0;i<wd;i++)
    {
      double dot = 0.0;
      if(norm[i] > 0.0)
      {
        // TODO: try to use a full solve here instead
        // TODO: also try to correct for tonecurve here!
        for(int j=0;j<wd;j++) 
          for(int ch=0;ch<dim;ch++)
            dot += A[j*wd+i] * r[ch][j];
        dot *= norm[i];
        dot = fabs(dot);
      }
      // fprintf(stderr, "dot %d = %g\n", i, dot);
      if(dot > maxdot)
      {
        maxcol = i;
        maxdot = dot;
      }
    }

    // remember which column that was, we'll need it to evaluate later:
    permutation[s] = maxcol;
    // make sure we won't choose it again:
    norm[maxcol] = 0.0;

    // solve linear least squares for sparse c for every output channel:
    for(int ch=0;ch<dim;ch++)
    {
      // need to re-init all of the previous columns in As since
      // svd will destroy its contents:
      for(int i=0;i<s;i++) for(int j=0;j<wd;j++)
        As[j*S + i] = A[j*wd+permutation[i]];

      // append this max column of A to the sparse matrix A':
      for(int j=0;j<wd;j++)
        As[j*S + s] = A[j*wd+maxcol];
      // A'[wd][s+1] = u[wd][s+1] diag(w[s+1]) v[s+1][s+1]^t
      //
      // svd to solve for c:
      // A * c = b
      // A = u w v^t => A-1 = v 1/w u^t
      dsvd(As, wd, s+1, S, w, v); // As is wd x s+1 but row stride S.
      if(w[s] < 1e-3) // if the smallest singular value becomes too small, we're done
        return s;
      for(int i=0;i<=s;i++) // compute tmp = u^t * b
      {
        tmp[i] = 0.0;
        for(int j=0;j<wd;j++)
          tmp[i] += As[j*S+i] * b[ch][j];
      }
      for(int i=0;i<=s;i++) // apply singular values:
        tmp[i] /= w[i];
      for(int j=0;j<=s;j++)
      { // compute first s output coefficients coeff[ch][j]
        coeff[ch][j] = 0.0;
        for(int i=0;i<=s;i++)
          coeff[ch][j] += v[j*(s+1)+i] * tmp[i];
      }

      // compute new residual:
      // r = b - As c
      for(int j=0;j<wd;j++)
      {
        r[ch][j] = b[ch][j];
        for(int i=0;i<=s;i++)
          r[ch][j] -= A[j*wd + permutation[i]] * coeff[ch][i];
      }
    }

    // TODO: compute post-tonecurve error here!
    // TODO: choose residual component based on post-tonecurve error, too!
    double err = 0.0;
    for(int i=0;i<wd;i++)
      err = MAX(err, r[0][i]*r[0][i] + r[1][i]*r[1][i] + r[2][i]*r[2][i]);
    err = sqrt(err);
    // residual is max CIE76 delta E now
    // everything < 2 is usually considired a very good approximation:
    fprintf(stderr, "rank %d error DE %g\n", s+1, err);
    if(err < 2.0) break;
  }
  return s+1;
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
  // bubble sort by brightness
  for(int k=0;k<num-1;k++) for(int j=0;j<num-k-1;j++)
  {
    if(params.source_L[j] > params.source_L[j+1])
    {
      SWAP(params.source_L[j], params.source_L[j+1]);
      SWAP(params.source_a[j], params.source_a[j+1]);
      SWAP(params.source_b[j], params.source_b[j+1]);
      SWAP(params.target_L[j], params.target_L[j+1]);
      SWAP(params.target_a[j], params.target_a[j+1]);
      SWAP(params.target_b[j], params.target_b[j+1]);
    }
  }
  // bubble sort by saturation
  for(int k=0;k<num-1;k++) for(int j=0;j<num-k-1;j++)
  {
    if(params.source_a[j]*params.source_a[j]+params.source_b[j]*params.source_b[j] <
       params.source_a[j+1]*params.source_a[j+1]+params.source_b[j+1]*params.source_b[j+1])
    {
      SWAP(params.source_L[j], params.source_L[j+1]);
      SWAP(params.source_a[j], params.source_a[j+1]);
      SWAP(params.source_b[j], params.source_b[j+1]);
      SWAP(params.target_L[j], params.target_L[j+1]);
      SWAP(params.target_a[j], params.target_a[j+1]);
      SWAP(params.target_b[j], params.target_b[j+1]);
    }
  }

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


/* --------------------------------------------------------------------------
    This file is part of darktable,
    copyright (c) 2014 Stéphane Gimenez <dev@gim.name>

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
* ------------------------------------------------------------------------*/

static inline int
lin_size(int dimension, int d)
{
  if (dimension == 1)
    return d + 1;
  if (dimension == 2)
    return (d + 1)*(d + 2)/2;
  return -1;
}

static inline int
lin_solve(int dn, float *mt, float *vt, float *st)
{
// returns the number of unsolvable unknowns (0 if solved uniquely)
// dn - system dimension
// mt - matrix with coefficients
// vt - vector with free members
// st - vector with system solution
// mt and vt are changed after function call

  int r[dn], c[dn]; // order of row and column processing
  for (int k = 0; k < dn; k++)
    c[k] = r[k] = k;

  for (int k = 0; k < dn; k++) // base row of matrix
  {
    // search of max element
    int m = 0, n = 0;
    float fmax = -1;
    for (int i = k; i < dn; i++)
      for (int j = k; j < dn; j++)
      {
        float fcmp = fabsf(mt[r[i]*dn + c[j]]);
        if (fmax < fcmp) { fmax = fcmp; m = i; n = j; }
      }

    // singularity
    if (fmax == 0.0)
      return dn - k;

    // swap indexes
    int R = r[m]; r[m] = r[k]; r[k] = R;
    int C = c[n]; c[n] = c[k]; c[k] = C;

    // triangulation of matrix with coefficients
    for (int j = k + 1; j < dn; j++)
    {
      float p = mt[r[j]*dn + c[k]] / mt[r[k]*dn + c[k]];
      for (int i = k + 1; i < dn; i++) // starts at k+1 as an optimisation
        mt[r[j]*dn + c[i]] -= p * mt[r[k]*dn + c[i]];
      vt[r[j]] -= p * vt[r[k]];
    }
  }

  for (int k = dn - 1; k >= 0; k--)
  {
    st[c[k]] = vt[r[k]];
    for (int i = k + 1; i < dn; i++)
      st[c[k]] -= mt[r[k]*dn + c[i]] * st[c[i]];
    st[c[k]] /= mt[r[k]*dn + c[k]];
  }

  return 0;
}

static inline void
lin_zero(int dn, float *mt, float *vt)
{
  for (int i = 0; i < dn*dn; i++)
    mt[i] = 0.0;
  for (int i = 0; i < dn; i++)
    vt[i] = 0.0;
}

static inline void
lin_add(int dn, float *mt, float *vt, float *mtd, float *vtd)
{
  for (int i = 0; i < dn*dn; i++)
    mtd[i] += mt[i];
  for (int i = 0; i < dn; i++)
    vtd[i] += vt[i];
}

static inline void
lin_push1(
  int dn, int d, float *mt, float *vt,
  float x, float k, float v)
{
  int b = 0;
  float pix = 1.0; // pow_i(x)
  for (int i = 0; i <= d; i++, pix *= x)
  {
    float o = k*pix;
    int a = 0;
    float pmx = 1.0; // pow_m(x)
    for (int n = 0; n <= d; n++, pmx *= x)
    {
      mt[b*dn+a] += o*pmx;
      a++;
    }
    vt[b] += o*v;
    b++;
  }
}

static inline void
lin_push2(
  int dn, int d, float *mt, float *vt,
  float x, float y, float k, float v)
{
  int b = 0;
  float piy = 1.0; // pow_i(y)
  for (int i = 0; i <= d; i++, piy *= y)
  {
    float pjx = 1.0; // pow_j(x)
    for (int j = 0; j <= d - i; j++, pjx *= x)
    {
      int a = 0;
      float o = k*piy*pjx;
      float pmy = 1.0; // pow_m(y)
      for (int m = 0; m <= d; m++, pmy *= y)
      {
        float pnx = 1.0; // pow_n(x)
        for (int n = 0; n <= d - m; n++, pnx *= x)
        {
          mt[b*dn+a] += o*pmy*pnx;
          a++;
        }
      }
      vt[b] += o*v;
      b++;
    }
  }
}

static inline float
lin_get1(int d, float *st, float x)
{
  float v = 0.0;
  int a = 0;
  float pmx = 1.0; // pow_m(x)
  for (int m = 0; m <= d; m++, pmx *= x)
  {
    v += pmx*st[a];
    a++;
  }
  return v;
}

static inline float
lin_get2(int d, float *st, float x, float y)
{
  float v = 0.0;
  int a = 0;
  float pmy = 1.0; // pow_m(y)
  for (int m = 0; m <= d; m++, pmy *= y)
  {
    float pnx = 1.0; // pow_n(x)
    for (int n = 0; n <= d - m; n++, pnx *= x)
    {
      v += pmy*pnx*st[a];
      a++;
    }
  }
  return v;
}

static inline void
lin_print(int degn, float *mt, float *vt)
{
  for (int j = 0; j < degn; j++)
  {
    for (int i = 0; i < degn; i++)
      printf("%+08.3f ", mt[degn*j+i]);
    printf("= %+08.3f\n", vt[j]);
  }
}

/*
  This file is part of darktable,
  Copyright (C) 2016-2020 darktable developers.

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

/* For parameter optimization we are using the Nelder-Mead simplex method
 * implemented by Michael F. Hutt.
 * Changes versus the original code:
 *      do not include "nmsimplex.h" (not needed)
 *      renamed configuration variables to NMS_*
 *      add additional argument to objfun for arbitrary parameters
 *      simplex() returns number of used iterations instead of min value
 *      maximum number of iterations as function parameter
 *      make interface function simplex() static
 *      initialize i and j to avoid compiler warnings
 *      comment out printing of status inormation
 *      reformat according to darktable's clang standards
 */

/*==================================================================================
 * begin nmsimplex code downloaded from http://www.mikehutt.com/neldermead.html
 * on February 6, 2016
 *==================================================================================*/
/*
 * Program: nmsimplex.c
 * Author : Michael F. Hutt
 * http://www.mikehutt.com
 * 11/3/97
 *
 * An implementation of the Nelder-Mead simplex method.
 *
 * Copyright (c) 1997-2011 <Michael F. Hutt>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Jan. 6, 1999
 * Modified to conform to the algorithm presented
 * in Margaret H. Wright's paper on Direct Search Methods.
 *
 * Jul. 23, 2007
 * Fixed memory leak.
 *
 * Mar. 1, 2011
 * Added constraints.
 */

//#include "nmsimplex.h"

static int simplex(double (*objfunc)(double[], void *params), double start[], int n, double EPSILON, double scale,
                   int maxiter, void (*constrain)(double[], int n), void *params)
{

  int vs; /* vertex with smallest value */
  int vh; /* vertex with next smallest value */
  int vg; /* vertex with largest value */

  int i = 0, j = 0, m, row;
  int itr; /* track the number of iterations */

  double **v;    /* holds vertices of simplex */
  double pn, qn; /* values used to create initial simplex */
  double *f;     /* value of function at each vertex */
  double fr;     /* value of function at reflection point */
  double fe;     /* value of function at expansion point */
  double fc;     /* value of function at contraction point */
  double *vr;    /* reflection - coordinates */
  double *ve;    /* expansion - coordinates */
  double *vc;    /* contraction - coordinates */
  double *vm;    /* centroid - coordinates */
  //double min;

  double fsum, favg, s, cent;

  /* dynamically allocate arrays */

  /* allocate the rows of the arrays */
  v = (double **)malloc(sizeof(double *) * (n + 1));
  f = (double *)malloc(sizeof(double) * (n + 1));
  vr = (double *)malloc(sizeof(double) * n);
  ve = (double *)malloc(sizeof(double) * n);
  vc = (double *)malloc(sizeof(double) * n);
  vm = (double *)malloc(sizeof(double) * n);

  /* allocate the columns of the arrays */
  for(i = 0; i <= n; i++)
  {
    v[i] = (double *)malloc(sizeof(double) * n);
  }

  /* create the initial simplex */
  /* assume one of the vertices is 0,0 */

  pn = scale * (sqrt(n + 1) - 1 + n) / (n * sqrt(2));
  qn = scale * (sqrt(n + 1) - 1) / (n * sqrt(2));

  for(i = 0; i < n; i++)
  {
    v[0][i] = start[i];
  }

  for(i = 1; i <= n; i++)
  {
    for(j = 0; j < n; j++)
    {
      if(i - 1 == j)
      {
        v[i][j] = pn + start[j];
      }
      else
      {
        v[i][j] = qn + start[j];
      }
    }
  }

  if(constrain != NULL)
  {
    constrain(v[j], n);
  }
  /* find the initial function values */
  for(j = 0; j <= n; j++)
  {
    f[j] = objfunc(v[j], params);
  }

#if 0
  /* print out the initial values */
  printf("Initial Values\n");
  for(j = 0; j <= n; j++)
  {
    for(i = 0; i < n; i++)
    {
      printf("%f %f\n", v[j][i], f[j]);
    }
  }
#endif

  /* begin the main loop of the minimization */
  for(itr = 1; itr <= maxiter; itr++)
  {
    /* find the index of the largest value */
    vg = 0;
    for(j = 0; j <= n; j++)
    {
      if(f[j] > f[vg])
      {
        vg = j;
      }
    }

    /* find the index of the smallest value */
    vs = 0;
    for(j = 0; j <= n; j++)
    {
      if(f[j] < f[vs])
      {
        vs = j;
      }
    }

    /* find the index of the second largest value */
    vh = vs;
    for(j = 0; j <= n; j++)
    {
      if(f[j] > f[vh] && f[j] < f[vg])
      {
        vh = j;
      }
    }

    /* calculate the centroid */
    for(j = 0; j <= n - 1; j++)
    {
      cent = 0.0;
      for(m = 0; m <= n; m++)
      {
        if(m != vg)
        {
          cent += v[m][j];
        }
      }
      vm[j] = cent / n;
    }

    /* reflect vg to new vertex vr */
    for(j = 0; j <= n - 1; j++)
    {
      /*vr[j] = (1+NMS_ALPHA)*vm[j] - NMS_ALPHA*v[vg][j];*/
      vr[j] = vm[j] + NMS_ALPHA * (vm[j] - v[vg][j]);
    }
    if(constrain != NULL)
    {
      constrain(vr, n);
    }
    fr = objfunc(vr, params);

    if(fr < f[vh] && fr >= f[vs])
    {
      for(j = 0; j <= n - 1; j++)
      {
        v[vg][j] = vr[j];
      }
      f[vg] = fr;
    }

    /* investigate a step further in this direction */
    if(fr < f[vs])
    {
      for(j = 0; j <= n - 1; j++)
      {
        /*ve[j] = NMS_GAMMA*vr[j] + (1-NMS_GAMMA)*vm[j];*/
        ve[j] = vm[j] + NMS_GAMMA * (vr[j] - vm[j]);
      }
      if(constrain != NULL)
      {
        constrain(ve, n);
      }
      fe = objfunc(ve, params);

      /* by making fe < fr as opposed to fe < f[vs],
         Rosenbrocks function takes 63 iterations as opposed
         to 64 when using double variables. */

      if(fe < fr)
      {
        for(j = 0; j <= n - 1; j++)
        {
          v[vg][j] = ve[j];
        }
        f[vg] = fe;
      }
      else
      {
        for(j = 0; j <= n - 1; j++)
        {
          v[vg][j] = vr[j];
        }
        f[vg] = fr;
      }
    }

    /* check to see if a contraction is necessary */
    if(fr >= f[vh])
    {
      if(fr < f[vg] && fr >= f[vh])
      {
        /* perform outside contraction */
        for(j = 0; j <= n - 1; j++)
        {
          /*vc[j] = NMS_BETA*v[vg][j] + (1-NMS_BETA)*vm[j];*/
          vc[j] = vm[j] + NMS_BETA * (vr[j] - vm[j]);
        }
        if(constrain != NULL)
        {
          constrain(vc, n);
        }
        fc = objfunc(vc, params);
      }
      else
      {
        /* perform inside contraction */
        for(j = 0; j <= n - 1; j++)
        {
          /*vc[j] = NMS_BETA*v[vg][j] + (1-NMS_BETA)*vm[j];*/
          vc[j] = vm[j] - NMS_BETA * (vm[j] - v[vg][j]);
        }
        if(constrain != NULL)
        {
          constrain(vc, n);
        }
        fc = objfunc(vc, params);
      }


      if(fc < f[vg])
      {
        for(j = 0; j <= n - 1; j++)
        {
          v[vg][j] = vc[j];
        }
        f[vg] = fc;
      }
      /* at this point the contraction is not successful,
         we must halve the distance from vs to all the
         vertices of the simplex and then continue.
         10/31/97 - modified to account for ALL vertices.
      */
      else
      {
        for(row = 0; row <= n; row++)
        {
          if(row != vs)
          {
            for(j = 0; j <= n - 1; j++)
            {
              v[row][j] = v[vs][j] + (v[row][j] - v[vs][j]) / 2.0;
            }
          }
        }
        if(constrain != NULL)
        {
          constrain(v[vg], n);
        }
        f[vg] = objfunc(v[vg], params);
        if(constrain != NULL)
        {
          constrain(v[vh], n);
        }
        f[vh] = objfunc(v[vh], params);
      }
    }
#if 0
    /* print out the value at each iteration */
    printf("Iteration %d\n", itr);
    for(j = 0; j <= n; j++)
    {
      for(i = 0; i < n; i++)
      {
        printf("%f %f\n", v[j][i], f[j]);
      }
    }
#endif
    /* test for convergence */
    fsum = 0.0;
    for(j = 0; j <= n; j++)
    {
      fsum += f[j];
    }
    favg = fsum / (n + 1);
    s = 0.0;
    for(j = 0; j <= n; j++)
    {
      s += pow((f[j] - favg), 2.0) / (n);
    }
    s = sqrt(s);
    if(s < EPSILON) break;
  }
  /* end main loop of the minimization */

  /* find the index of the smallest value */
  vs = 0;
  for(j = 0; j <= n; j++)
  {
    if(f[j] < f[vs])
    {
      vs = j;
    }
  }
#if 0
  printf("The minimum was found at\n");
  for(j = 0; j < n; j++)
  {
    printf("%e\n", v[vs][j]);
    start[j] = v[vs][j];
  }
  double min = objfunc(v[vs], params);
  k++;
  printf("The minimum value is %f\n", min);
  printf("%d Function Evaluations\n", k);
  printf("%d Iterations through program\n", itr);
#else
  for(j = 0; j < n; j++)
  {
    start[j] = v[vs][j];
  }
#endif
  free(f);
  free(vr);
  free(ve);
  free(vc);
  free(vm);
  for(i = 0; i <= n; i++)
  {
    free(v[i]);
  }
  free(v);
  return itr;
}

/*==================================================================================
 * end of nmsimplex code
 *==================================================================================*/

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


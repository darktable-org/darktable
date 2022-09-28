/*
    This file is part of darktable,
    Copyright (C) 2011-2022 darktable developers.

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


#include "develop/tiling.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/pixelpipe.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>


/* this defines an additional alignment requirement for opencl image width.
   It can have strong effects on processing speed. Reasonable values are a
   power of 2. set to 1 for no effect. */
#define CL_ALIGNMENT ((piece->pipe->dsc.filters != 9u) ? 4 : 1)

/* parameter RESERVE for extended roi_in sizes due to inaccuracies when doing
   roi_out -> roi_in estimations.
   Needs to be increased if tiling fails due to insufficient buffer sizes. */
#define RESERVE 5

/* greatest common divisor */
static unsigned _gcd(unsigned a, unsigned b)
{
  unsigned t;
  while(b != 0)
  {
    t = b;
    b = a % b;
    a = t;
  }
  return MAX(a, 1);
}

/* least common multiple */
static unsigned _lcm(unsigned a, unsigned b)
{
  return (((unsigned long)a * b) / _gcd(a, b));
}


static inline int _min(int a, int b)
{
  return a < b ? a : b;
}

static inline int _max(int a, int b)
{
  return a > b ? a : b;
}


static inline int _align_up(int n, int a)
{
  return n + a - (n % a);
}
static inline int _align_down(int n, int a)
{
  return n - (n % a);
}
static inline int _align_close(int n, int a)
{
  const int off = n % a;
  const int shift = (off > a/2) ? a - off : -off;
  return n + shift;
}

/* 
  _maximum_number_tiles is the assumed maximum sane number of tiles
  if during tiling this number is exceeded darktable assumes that tiling is not possible and falls back
  to untiled processing - with all system memory limits taking full effect.
  For huge images like stitched panos the user might choose resourcelevel="unrestricted", in that
  case the allowed number of tiles is practically unlimited
*/
static inline int _maximum_number_tiles()
{
  return (darktable.dtresources.level == 3) ? 0x40000000 : 10000;
}

static inline void _print_roi(const dt_iop_roi_t *roi, const char *label)
{
  if((darktable.unmuted & DT_DEBUG_VERBOSE) && (darktable.unmuted & DT_DEBUG_TILING))
    fprintf(stderr,"     {%5d %5d ->%5d %5d (%5dx%5d)  %.6f } %s\n",
         roi->x, roi->y, roi->x + roi->width, roi->y + roi->height, roi->width, roi->height, roi->scale, label);
}


#if 0
static void
_nm_constraints(double x[], int n)
{
  x[0] = fabs(x[0]);
  x[1] = fabs(x[1]);
  x[2] = fabs(x[2]);
  x[3] = fabs(x[3]);

  if(x[0] > 1.0) x[0] = 1.0 - x[0];
  if(x[1] > 1.0) x[1] = 1.0 - x[1];
  if(x[2] > 1.0) x[2] = 1.0 - x[2];
  if(x[3] > 1.0) x[3] = 1.0 - x[3];

}
#endif

static double _nm_fitness(double x[], void *rest[])
{
  struct dt_iop_module_t *self = (struct dt_iop_module_t *)rest[0];
  struct dt_dev_pixelpipe_iop_t *piece = (struct dt_dev_pixelpipe_iop_t *)rest[1];
  struct dt_iop_roi_t *iroi = (struct dt_iop_roi_t *)rest[2];
  struct dt_iop_roi_t *oroi = (struct dt_iop_roi_t *)rest[3];

  dt_iop_roi_t oroi_test = *oroi;
  oroi_test.x = x[0] * piece->iwidth;
  oroi_test.y = x[1] * piece->iheight;
  oroi_test.width = x[2] * piece->iwidth;
  oroi_test.height = x[3] * piece->iheight;

  dt_iop_roi_t iroi_probe = *iroi;
  self->modify_roi_in(self, piece, &oroi_test, &iroi_probe);

  double fitness = 0.0;

  fitness += (double)(iroi_probe.x - iroi->x) * (iroi_probe.x - iroi->x);
  fitness += (double)(iroi_probe.y - iroi->y) * (iroi_probe.y - iroi->y);
  fitness += (double)(iroi_probe.width - iroi->width) * (iroi_probe.width - iroi->width);
  fitness += (double)(iroi_probe.height - iroi->height) * (iroi_probe.height - iroi->height);

  return fitness;
}


/* We use a Nelder-Mead simplex algorithm based on an implementation of Michael F. Hutt.
   It is covered by the following copyright notice: */
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
 */

#define ALPHA 1.0   /* reflection coefficient */
#define BETA 0.5    /* contraction coefficient */
#define GAMMA 2.0   /* expansion coefficient */

static int _simplex(double (*objfunc)(double[], void *[]), double start[], int n, double EPSILON,
                    double scale, int maxiter, void (*constrain)(double[], int n), void *rest[])
{

  int vs; /* vertex with smallest value */
  int vh; /* vertex with next smallest value */
  int vg; /* vertex with largest value */

  int i, j = 0, m, row;
  int k;   /* track the number of function evaluations */
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
    f[j] = objfunc(v[j], rest);
  }

  k = n + 1;

#if 0
  /* print out the initial values */
  printf ("Initial Values\n");
  for(j = 0; j <= n; j++)
  {
    for(i = 0; i < n; i++)
    {
      printf ("%f %f\n", v[j][i], f[j]);
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
      /*vr[j] = (1+ALPHA)*vm[j] - ALPHA*v[vg][j]; */
      vr[j] = vm[j] + ALPHA * (vm[j] - v[vg][j]);
    }
    if(constrain != NULL)
    {
      constrain(vr, n);
    }
    fr = objfunc(vr, rest);
    k++;

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
        /*ve[j] = GAMMA*vr[j] + (1-GAMMA)*vm[j]; */
        ve[j] = vm[j] + GAMMA * (vr[j] - vm[j]);
      }
      if(constrain != NULL)
      {
        constrain(ve, n);
      }
      fe = objfunc(ve, rest);
      k++;

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
          /*vc[j] = BETA*v[vg][j] + (1-BETA)*vm[j]; */
          vc[j] = vm[j] + BETA * (vr[j] - vm[j]);
        }
        if(constrain != NULL)
        {
          constrain(vc, n);
        }
        fc = objfunc(vc, rest);
        k++;
      }
      else
      {
        /* perform inside contraction */
        for(j = 0; j <= n - 1; j++)
        {
          /*vc[j] = BETA*v[vg][j] + (1-BETA)*vm[j]; */
          vc[j] = vm[j] - BETA * (vm[j] - v[vg][j]);
        }
        if(constrain != NULL)
        {
          constrain(vc, n);
        }
        fc = objfunc(vc, rest);
        k++;
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
        f[vg] = objfunc(v[vg], rest);
        k++;
        if(constrain != NULL)
        {
          constrain(v[vh], n);
        }
        f[vh] = objfunc(v[vh], rest);
        k++;
      }
    }

#if 0
    /* print out the value at each iteration */
    printf ("Iteration %d\n", itr);
    for(j = 0; j <= n; j++)
    {
      for(i = 0; i < n; i++)
      {
        printf ("%f %f\n", v[j][i], f[j]);
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
  printf ("The minimum was found at\n");
  for(j = 0; j < n; j++)
  {
    printf ("%e\n", v[vs][j]);
    start[j] = v[vs][j];
  }
  double min = objfunc (v[vs], rest);
  printf ("Function value at minimum %f\n", min);
  k++;
  printf ("%d Function Evaluations\n", k);
  printf ("%d Iterations through program\n", itr);
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


static int _nm_fit_output_to_input_roi(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                       const dt_iop_roi_t *iroi, dt_iop_roi_t *oroi, int delta)
{
  void *rest[4] = { (void *)self, (void *)piece, (void *)iroi, (void *)oroi };
  double start[4] = { (float)oroi->x / piece->iwidth, (float)oroi->y / piece->iheight,
                      (float)oroi->width / piece->iwidth, (float)oroi->height / piece->iheight };
  double epsilon = (double)delta / MIN(piece->iwidth, piece->iheight);
  int maxiter = 1000;

  int iter = _simplex(_nm_fitness, start, 4, epsilon, 1.0, maxiter, NULL, rest);

  dt_vprint(DT_DEBUG_TILING, "[_nm_fit_output_to_input_roi] _simplex: %d, delta: %d, epsilon: %f\n", iter, delta, epsilon);

  oroi->x = start[0] * piece->iwidth;
  oroi->y = start[1] * piece->iheight;
  oroi->width = start[2] * piece->iwidth;
  oroi->height = start[3] * piece->iheight;

  return (iter <= maxiter);
}



/* find a matching oroi_full by probing start value of oroi and get corresponding input roi into iroi_probe.
   We search in two steps. first by a simplicistic iterative search which will succeed in most cases.
   If this does not converge, we do a downhill simplex (nelder-mead) fitting */
static int _fit_output_to_input_roi(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                    const dt_iop_roi_t *iroi, dt_iop_roi_t *oroi, int delta, int iter)
{
  dt_iop_roi_t iroi_probe = *iroi;
  dt_iop_roi_t save_oroi = *oroi;

  // try to go the easy way. this works in many cases where output is
  // just like input, only scaled down
  self->modify_roi_in(self, piece, oroi, &iroi_probe);
  while((abs((int)iroi_probe.x - (int)iroi->x) > delta || abs((int)iroi_probe.y - (int)iroi->y) > delta
         || abs((int)iroi_probe.width - (int)iroi->width) > delta
         || abs((int)iroi_probe.height - (int)iroi->height) > delta) && iter > 0)
  {
    _print_roi(&iroi_probe, "tile iroi_probe");
    _print_roi(oroi, "tile oroi old");

    oroi->x += (iroi->x - iroi_probe.x) * oroi->scale / iroi->scale;
    oroi->y += (iroi->y - iroi_probe.y) * oroi->scale / iroi->scale;
    oroi->width += (iroi->width - iroi_probe.width) * oroi->scale / iroi->scale;
    oroi->height += (iroi->height - iroi_probe.height) * oroi->scale / iroi->scale;

    _print_roi(oroi, "tile oroi new");

    self->modify_roi_in(self, piece, oroi, &iroi_probe);
    iter--;
  }

  if(iter > 0) return TRUE;

  *oroi = save_oroi;

  // simplicistic approach did not converge.
  // try simplex downhill fitting now.
  // it's crucial that we have a good starting point in oroi, else this
  // will not converge as well.
  int fit = _nm_fit_output_to_input_roi(self, piece, iroi, oroi, delta);
  return fit;
}


/* simple tiling algorithm for roi_in == roi_out, i.e. for pixel to pixel modules/operations */
static void _default_process_tiling_ptp(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                        const void *const ivoid, void *const ovoid,
                                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                        const int in_bpp)
{
  void *input = NULL;
  void *output = NULL;
  dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s] **** tiling module '%s' for image with size %dx%d --> %dx%d\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, roi_in->width, roi_in->height, roi_out->width, roi_out->height);
  dt_iop_buffer_dsc_t dsc;
  self->output_format(self, piece->pipe, piece, &dsc);
  const int out_bpp = dt_iop_buffer_dsc_to_bpp(&dsc);

  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);

  /* tiling really does not make sense in these cases. standard process() is not better or worse than we are
   */
  if((tiling.factor < 2.2f)
     && (tiling.overhead < 0.2f * roi_in->width * roi_in->height * max_bpp))
  {
    dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s]  no need to use tiling for module '%s' as no real "
                           "memory saving to be expected\n", dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
    goto fallback;
  }

  /* calculate optimal size of tiles */
  float available = dt_get_available_mem();
  assert(available >= 500.0f * 1024.0f * 1024.0f);
  /* correct for size of ivoid and ovoid which are needed on top of tiling */
  available = fmaxf(available - ((float)roi_out->width * roi_out->height * out_bpp)
                   - ((float)roi_in->width * roi_in->height * in_bpp) - tiling.overhead,
                   0);

  /* we ignore the above value if singlebuffer_limit (is defined and) is higher than available/tiling.factor.
     this will mainly allow tiling for modules with high and "unpredictable" memory demand which is
     reflected in high values of tiling.factor (take bilateral noise reduction as an example). */
  float singlebuffer = dt_get_singlebuffer_mem();
  const float factor = fmaxf(tiling.factor, 1.0f);
  const float maxbuf = fmaxf(tiling.maxbuf, 1.0f);
  singlebuffer = fmaxf(available / factor, singlebuffer);

  int width = roi_in->width;
  int height = roi_in->height;

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width * height * max_bpp * maxbuf > singlebuffer)
  {
    const float scale = singlebuffer / ((float)width * height * max_bpp * maxbuf);

    /* TODO: can we make this more efficient to minimize total overlap between tiles? */
    if(width < height && scale >= 0.333f)
    {
      height = floorf(height * scale);
    }
    else if(height <= width && scale >= 0.333f)
    {
      width = floorf(width * scale);
    }
    else
    {
      width = floorf(width * sqrtf(scale));
      height = floorf(height * sqrtf(scale));
    }
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_ptp] buffer exceeds singlebuffer, corrected to %dx%d\n",
            width, height);
  }

  /* make sure we have a reasonably effective tile dimension. if not try square tiles */
  if(3 * tiling.overlap > width || 3 * tiling.overlap > height)
  {
    width = height = floorf(sqrtf((float)width * height));
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_roi] use squares because of overlap, corrected to %dx%d\n",
            width, height);
  }

  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction.
     We guarantee alignment by selecting image width/height and overlap accordingly. For a tile width/height
     that is identical to image width/height no special alignment is needed. */

  const unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);

  assert(xyalign != 0);

  /* properly align tile width and height by making them smaller if needed */
  if(width < roi_in->width) width = (width / xyalign) * xyalign;
  if(height < roi_in->height) height = (height / xyalign) * xyalign;

  /* also make sure that overlap follows alignment rules by making it wider when needed */
  const int overlap = tiling.overlap % xyalign != 0 ? (tiling.overlap / xyalign + 1) * xyalign
                                                    : tiling.overlap;

  /* calculate effective tile size */
  const int tile_wd = width - 2 * overlap > 0 ? width - 2 * overlap : 1;
  const int tile_ht = height - 2 * overlap > 0 ? height - 2 * overlap : 1;

  /* calculate number of tiles */
  const int tiles_x = width < roi_in->width ? ceilf(roi_in->width / (float)tile_wd) : 1;
  const int tiles_y = height < roi_in->height ? ceilf(roi_in->height / (float)tile_ht) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > _maximum_number_tiles())
  {
    dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s] gave up tiling for module '%s'. too many tiles: %d x %d\n",
             dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, tiles_x, tiles_y);
    goto error;
  }

  dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s] (%dx%d) tiles with max dimensions %dx%d and overlap %d\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), tiles_x, tiles_y, width, height, overlap);

  /* reserve input and output buffers for tiles */
  input = dt_alloc_align(64, (size_t)width * height * in_bpp);
  if(input == NULL)
  {
    dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s] could not alloc input buffer for module '%s'\n",
             dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
    goto error;
  }
  output = dt_alloc_align(64, (size_t)width * height * out_bpp);
  if(output == NULL)
  {
    dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s]  could not alloc output buffer for module '%s'\n",
             dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
    goto error;
  }

  /* store processed_maximum to be re-used and aggregated */
  dt_aligned_pixel_t processed_maximum_saved;
  dt_aligned_pixel_t processed_maximum_new = { 1.0f };
  for_four_channels(k) processed_maximum_saved[k] = piece->pipe->dsc.processed_maximum[k];

  /* iterate over tiles */
  for(size_t tx = 0; tx < tiles_x; tx++)
  {
    const size_t wd = tx * tile_wd + width > roi_in->width ? roi_in->width - tx * tile_wd : width;
    for(size_t ty = 0; ty < tiles_y; ty++)
    {
      piece->pipe->tiling = 1;

      const size_t ht = ty * tile_ht + height > roi_in->height ? roi_in->height - ty * tile_ht : height;

      /* no need to process end-tiles that are smaller than the total overlap area */
      if((wd <= 2 * overlap && tx > 0) || (ht <= 2 * overlap && ty > 0)) continue;

      /* origin and region of effective part of tile, which we want to store later */
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { wd, ht, 1 };

      /* roi_in and roi_out for process_cl on subbuffer */
      dt_iop_roi_t iroi = { roi_in->x + tx * tile_wd, roi_in->y + ty * tile_ht, wd, ht, roi_in->scale };
      dt_iop_roi_t oroi = { roi_out->x + tx * tile_wd, roi_out->y + ty * tile_ht, wd, ht, roi_out->scale };

      /* offsets of tile into ivoid and ovoid */
      const size_t ioffs = (ty * tile_ht) * ipitch + (tx * tile_wd) * in_bpp;
      size_t ooffs = (ty * tile_ht) * opitch + (tx * tile_wd) * out_bpp;

      dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s] tile (%zu,%zu) with %zux%zu at origin [%zu,%zu]\n",
              dt_dev_pixelpipe_type_to_str(piece->pipe->type), tx, ty, wd, ht, tx * tile_wd, ty * tile_ht);

/* prepare input tile buffer */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(ht, in_bpp, ipitch, ivoid, wd) \
      dt_omp_sharedconst(ioffs) \
      shared(input, width) \
      schedule(static)
#endif
      for(size_t j = 0; j < ht; j++)
        memcpy((char *)input + j * wd * in_bpp, (char *)ivoid + ioffs + j * ipitch, (size_t)wd * in_bpp);

      /* take original processed_maximum as starting point */
      for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_saved[k];

      /* call process() of module */
      self->process(self, piece, input, output, &iroi, &oroi);

      /* aggregate resulting processed_maximum */
      /* TODO: check if there really can be differences between tiles and take
               appropriate action (calculate minimum, maximum, average, ...?) */
      for(int k = 0; k < 4; k++)
      {
        if(tx + ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->dsc.processed_maximum[k]) > 1.0e-6f)
          dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s] processed_maximum[%d] differs between tiles in module '%s'\n",
                   dt_dev_pixelpipe_type_to_str(piece->pipe->type), k, self->op);
        processed_maximum_new[k] = piece->pipe->dsc.processed_maximum[k];
      }

      /* correct origin and region of tile for overlap.
         make sure that we only copy back the "good" part. */
      if(tx > 0)
      {
        origin[0] += overlap;
        region[0] -= overlap;
        ooffs += (size_t)overlap * out_bpp;
      }
      if(ty > 0)
      {
        origin[1] += overlap;
        region[1] -= overlap;
        ooffs += (size_t)overlap * opitch;
      }

/* copy "good" part of tile to output buffer */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(opitch, out_bpp, ovoid, wd) \
      shared(ooffs, output, width, origin, region) \
      schedule(static)
#endif
      for(size_t j = 0; j < region[1]; j++)
        memcpy((char *)ovoid + ooffs + j * opitch,
               (char *)output + ((j + origin[1]) * wd + origin[0]) * out_bpp, (size_t)region[0] * out_bpp);
    }
  }

  /* copy back final processed_maximum */
  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_new[k];

  if(input != NULL) dt_free_align(input);
  if(output != NULL) dt_free_align(output);
  piece->pipe->tiling = 0;
  return;

error:
  dt_control_log(_("tiling failed for module '%s'. output might be garbled."), self->op);
// fall through

fallback:
  if(input != NULL) dt_free_align(input);
  if(output != NULL) dt_free_align(output);
  piece->pipe->tiling = 0;
  dt_print(DT_DEBUG_TILING, "[default_process_tiling_ptp] [%s] fall back to standard processing for module '%s'\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
  self->process(self, piece, ivoid, ovoid, roi_in, roi_out);
  return;
}



/* more elaborate tiling algorithm for roi_in != roi_out: slower than the ptp variant,
   more tiles and larger overlap */
static void _default_process_tiling_roi(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                        const void *const ivoid, void *const ovoid,
                                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                        const int in_bpp)
{
  void *input = NULL;
  void *output = NULL;

  dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] **** tiling module '%s' for image input size %dx%d --> %dx%d\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, roi_in->width, roi_in->height, roi_out->width, roi_out->height);
  _print_roi(roi_in, "module roi_in");
  _print_roi(roi_out, "module roi_out");

  dt_iop_buffer_dsc_t dsc;
  self->output_format(self, piece->pipe, piece, &dsc);
  const int out_bpp = dt_iop_buffer_dsc_to_bpp(&dsc);

  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  float fullscale = fmaxf(roi_in->scale / roi_out->scale, sqrtf(((float)roi_in->width * roi_in->height)
                                                              / ((float)roi_out->width * roi_out->height)));

  /* inaccuracy for roi_in elements in roi_out -> roi_in calculations */
  const int delta = ceilf(fullscale);

  /* estimate for additional (space) requirement in buffer dimensions due to inaccuracies */
  const int inacc = RESERVE * delta;

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);

  /* tiling really does not make sense in these cases. standard process() is not better or worse than we are
   */
  if((tiling.factor < 2.2f && tiling.overhead < 0.2f * roi_in->width * roi_in->height * max_bpp))
  {
    dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] no need to use tiling for module '%s' as no memory saving is expected\n",
             dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
    goto fallback;
  }

  /* calculate optimal size of tiles */
  float available = dt_get_available_mem();
  assert(available >= 500.0f * 1024.0f * 1024.0f);
  /* correct for size of ivoid and ovoid which are needed on top of tiling */
  available = fmaxf(available - ((float)roi_out->width * roi_out->height * out_bpp)
                   - ((float)roi_in->width * roi_in->height * in_bpp) - tiling.overhead,
                   0);

  /* we ignore the above value if singlebuffer_limit (is defined and) is higher than available/tiling.factor.
     this will mainly allow tiling for modules with high and "unpredictable" memory demand which is
     reflected in high values of tiling.factor (take bilateral noise reduction as an example). */
  float singlebuffer = dt_get_singlebuffer_mem();
  const float factor = fmaxf(tiling.factor, 1.0f);
  const float maxbuf = fmaxf(tiling.maxbuf, 1.0f);
  singlebuffer = fmaxf(available / factor, singlebuffer);

  int width = _max(roi_in->width, roi_out->width);
  int height = _max(roi_in->height, roi_out->height);

  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction. */

  /* for simplicity reasons we use only one alignment that fits to x and y requirements at the same time */
  const unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);

  assert(xyalign != 0);

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width * height * max_bpp * maxbuf > singlebuffer)
  {
    const float scale = singlebuffer / ((float)width * height * max_bpp * maxbuf);

    /* TODO: can we make this more efficient to minimize total overlap between tiles? */
    if(width < height && scale >= 0.333f)
    {
      height = _align_down((int)floorf(height * scale), xyalign);
    }
    else if(height <= width && scale >= 0.333f)
    {
      width = _align_down((int)floorf(width * scale), xyalign);
    }
    else
    {
      width = _align_down((int)floorf(width * sqrtf(scale)), xyalign);
      height = _align_down((int)floorf(height * sqrtf(scale)), xyalign);
    }
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] buffer exceeds singlebuffer, corrected to %dx%d\n",
            dt_dev_pixelpipe_type_to_str(piece->pipe->type), width, height);
  }

  /* make sure we have a reasonably effective tile dimension. if not try square tiles */
  if(3 * tiling.overlap > width || 3 * tiling.overlap > height)
  {
    width = height = _align_down((int)floorf(sqrtf((float)width * height)), xyalign);
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] use squares because of overlap, corrected to %dx%d\n",
            dt_dev_pixelpipe_type_to_str(piece->pipe->type), width, height);
  }

  /* make sure that overlap follows alignment rules by making it wider when needed.
     overlap_in needs to be aligned, overlap_out is only here to calculate output buffer size */
  const int overlap_in = _align_up(tiling.overlap, xyalign);
  const int overlap_out = ceilf((float)overlap_in / fullscale);

  int tiles_x = 1, tiles_y = 1;

  /* calculate number of tiles taking the larger buffer (input or output) as a guiding one.
     normally it is roi_in > roi_out; but let's be prepared */
  if(roi_in->width > roi_out->width)
    tiles_x = width < roi_in->width
                  ? ceilf((float)roi_in->width / (float)_max(width - 2 * overlap_in - inacc, 1))
                  : 1;
  else
    tiles_x = width < roi_out->width ? ceilf((float)roi_out->width / (float)_max(width - 2 * overlap_out, 1))
                                     : 1;

  if(roi_in->height > roi_out->height)
    tiles_y = height < roi_in->height
                  ? ceilf((float)roi_in->height / (float)_max(height - 2 * overlap_in - inacc, 1))
                  : 1;
  else
    tiles_y = height < roi_out->height
                  ? ceilf((float)roi_out->height / (float)_max(height - 2 * overlap_out, 1))
                  : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > _maximum_number_tiles())
  {
    dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] gave up tiling for module '%s'. too many tiles: %d x %d\n",
             dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, tiles_x, tiles_y);
    goto error;
  }


  /* calculate tile width and height excl. overlap (i.e. the good part) for output.
     values are important for all following processing steps. */
  const int tile_wd = _align_up(
      roi_out->width % tiles_x == 0 ? roi_out->width / tiles_x : roi_out->width / tiles_x + 1, xyalign);
  const int tile_ht = _align_up(
      roi_out->height % tiles_y == 0 ? roi_out->height / tiles_y : roi_out->height / tiles_y + 1, xyalign);

  dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] (%dx%d) tiles with max dimensions %dx%d, good %dx%d, overlap %d->%d\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), tiles_x, tiles_y, width, height, tile_wd, tile_ht, overlap_in, overlap_out);

  /* store processed_maximum to be re-used and aggregated */
  dt_aligned_pixel_t processed_maximum_saved;
  dt_aligned_pixel_t processed_maximum_new = { 1.0f };
  for_four_channels(k) processed_maximum_saved[k] = piece->pipe->dsc.processed_maximum[k];

  /* iterate over tiles */
  for(size_t tx = 0; tx < tiles_x; tx++)
    for(size_t ty = 0; ty < tiles_y; ty++)
    {
      piece->pipe->tiling = 1;

      /* the output dimensions of the good part of this specific tile */
      const size_t wd = (tx + 1) * tile_wd > roi_out->width ? (size_t)roi_out->width - tx * tile_wd : tile_wd;
      const size_t ht = (ty + 1) * tile_ht > roi_out->height ? (size_t)roi_out->height - ty * tile_ht : tile_ht;

      /* roi_in and roi_out of good part: oroi_good easy to calculate based on number and dimension of tile.
         iroi_good is calculated by modify_roi_in() of respective module */
      dt_iop_roi_t iroi_good = { roi_in->x  + tx * tile_wd, roi_in->y  + ty * tile_ht, wd, ht, roi_in->scale };
      dt_iop_roi_t oroi_good = { roi_out->x + tx * tile_wd, roi_out->y + ty * tile_ht, wd, ht, roi_out->scale };

      self->modify_roi_in(self, piece, &oroi_good, &iroi_good);

      /* clamp iroi_good to not exceed roi_in */
      iroi_good.x = _max(iroi_good.x, roi_in->x);
      iroi_good.y = _max(iroi_good.y, roi_in->y);
      iroi_good.width = _min(iroi_good.width, roi_in->width + roi_in->x - iroi_good.x);
      iroi_good.height = _min(iroi_good.height, roi_in->height + roi_in->y - iroi_good.y);

      _print_roi(&iroi_good, "tile iroi_good");
      _print_roi(&oroi_good, "tile oroi_good");

      /* now we need to calculate full region of this tile: increase input roi to take care of overlap
         requirements
         and alignment and add additional delta to correct for possible rounding errors in modify_roi_in()
         -> generates first estimate of iroi_full */
      const int x_in = iroi_good.x;
      const int y_in = iroi_good.y;
      const int width_in = iroi_good.width;
      const int height_in = iroi_good.height;
      const int new_x_in = _max(_align_close(x_in - overlap_in - delta, xyalign), roi_in->x);
      const int new_y_in = _max(_align_close(y_in - overlap_in - delta, xyalign), roi_in->y);
      const int new_width_in = _min(_align_up(width_in + overlap_in + delta + (x_in - new_x_in), xyalign),
                                    roi_in->width + roi_in->x - new_x_in);
      const int new_height_in = _min(_align_up(height_in + overlap_in + delta + (y_in - new_y_in), xyalign),
                                     roi_in->height + roi_in->y - new_y_in);

      /* iroi_full based on calculated numbers and dimensions. oroi_full just set as a starting point for the
       * following iterative search */
      dt_iop_roi_t iroi_full = { new_x_in, new_y_in, new_width_in, new_height_in, iroi_good.scale };
      dt_iop_roi_t oroi_full = oroi_good; // a good starting point for optimization

      _print_roi(&iroi_full, "tile iroi_full before optimization");
      _print_roi(&oroi_full, "tile oroi_full before optimization");

      /* try to find a matching oroi_full */
      if(!_fit_output_to_input_roi(self, piece, &iroi_full, &oroi_full, delta, 10))
      {
        dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] can not handle requested roi's. tiling for module '%s' not possible.\n",
                 dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
        goto error;
      }

      _print_roi(&iroi_full, "tile iroi_full after optimization");
      _print_roi(&oroi_full, "tile oroi_full after optimization");

      /* make sure that oroi_full at least covers the range of oroi_good.
         this step is needed due to the possibility of rounding errors */
      oroi_full.x = _min(oroi_full.x, oroi_good.x);
      oroi_full.y = _min(oroi_full.y, oroi_good.y);
      oroi_full.width = _max(oroi_full.width, oroi_good.x + oroi_good.width - oroi_full.x);
      oroi_full.height = _max(oroi_full.height, oroi_good.y + oroi_good.height - oroi_full.y);

      /* clamp oroi_full to not exceed roi_out */
      oroi_full.x = _max(oroi_full.x, roi_out->x);
      oroi_full.y = _max(oroi_full.y, roi_out->y);
      oroi_full.width = _min(oroi_full.width, roi_out->width + roi_out->x - oroi_full.x);
      oroi_full.height = _min(oroi_full.height, roi_out->height + roi_out->y - oroi_full.y);

      /* calculate final iroi_full */
      self->modify_roi_in(self, piece, &oroi_full, &iroi_full);

      /* clamp iroi_full to not exceed roi_in */
      iroi_full.x = _max(iroi_full.x, roi_in->x);
      iroi_full.y = _max(iroi_full.y, roi_in->y);
      iroi_full.width = _min(iroi_full.width, roi_in->width + roi_in->x - iroi_full.x);
      iroi_full.height = _min(iroi_full.height, roi_in->height + roi_in->y - iroi_full.y);

      _print_roi(&iroi_full, "tile iroi_full final");
      _print_roi(&oroi_full, "tile oroi_full final");

      /* offsets of tile into ivoid and ovoid */
      const size_t ioffs = ((size_t)iroi_full.y - roi_in->y)  * ipitch + ((size_t)iroi_full.x - roi_in->x) * in_bpp;
            size_t ooffs = ((size_t)oroi_good.y - roi_out->y) * opitch + ((size_t)oroi_good.x - roi_out->x) * out_bpp;

      dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] process tile (%zu,%zu) size %dx%d at origin [%d,%d]\n",
               dt_dev_pixelpipe_type_to_str(piece->pipe->type), tx, ty, iroi_full.width, iroi_full.height, iroi_full.x, iroi_full.y);

      /* prepare input tile buffer */
      input = dt_alloc_align(64, (size_t)iroi_full.width * iroi_full.height * in_bpp);
      if(input == NULL)
      {
        dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] could not alloc input buffer for module '%s'\n",
                 dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
        goto error;
      }
      output = dt_alloc_align(64, (size_t)oroi_full.width * oroi_full.height * out_bpp);
      if(output == NULL)
      {
        dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] could not alloc output buffer for module '%s'\n",
                 dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
        goto error;
      }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(in_bpp, ipitch, ivoid) \
      dt_omp_sharedconst(ioffs) shared(input, iroi_full) \
      schedule(static)
#endif
      for(size_t j = 0; j < iroi_full.height; j++)
        memcpy((char *)input + j * iroi_full.width * in_bpp, (char *)ivoid + ioffs + j * ipitch,
               (size_t)iroi_full.width * in_bpp);

      /* take original processed_maximum as starting point */
      for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_saved[k];

      /* call process() of module */
      self->process(self, piece, input, output, &iroi_full, &oroi_full);

      /* aggregate resulting processed_maximum */
      /* TODO: check if there really can be differences between tiles and take
               appropriate action (calculate minimum, maximum, average, ...?) */
      for(int k = 0; k < 4; k++)
      {
        if(tx + ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->dsc.processed_maximum[k]) > 1.0e-6f)
          dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] processed_maximum[%d] differs between tiles in module '%s'\n",
              k, self->op);
        processed_maximum_new[k] = piece->pipe->dsc.processed_maximum[k];
      }

      /* copy "good" part of tile to output buffer */
      const int origin_x = oroi_good.x - oroi_full.x;
      const int origin_y = oroi_good.y - oroi_full.y;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(opitch, origin_x, origin_y, out_bpp, ovoid) \
      shared(ooffs, output, oroi_good, oroi_full) \
      schedule(static)
#endif
      for(size_t j = 0; j < oroi_good.height; j++)
        memcpy((char *)ovoid + ooffs + j * opitch,
               (char *)output + ((j + origin_y) * oroi_full.width + origin_x) * out_bpp,
               (size_t)oroi_good.width * out_bpp);

      dt_free_align(input);
      dt_free_align(output);
      input = output = NULL;
    }

  /* copy back final processed_maximum */
  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_new[k];

  if(input != NULL) dt_free_align(input);
  if(output != NULL) dt_free_align(output);
  piece->pipe->tiling = 0;
  return;

error:
  dt_control_log(_("tiling failed for module '%s'. output might be garbled."), self->op);
// fall through

fallback:
  if(input != NULL) dt_free_align(input);
  if(output != NULL) dt_free_align(output);
  piece->pipe->tiling = 0;
  dt_print(DT_DEBUG_TILING, "[default_process_tiling_roi] [%s] fall back to standard processing for module '%s'\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
  self->process(self, piece, ivoid, ovoid, roi_in, roi_out);
  return;
}



/* if a module does not implement process_tiling() by itself, this function is called instead.
   _default_process_tiling_ptp() is able to handle standard cases where pixels do not change their places.
   _default_process_tiling_roi() takes care of all other cases where image gets distorted and for module
   "clipping",
   "flip" as this may flip or mirror the image. */
void default_process_tiling(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                            const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out, const int in_bpp)
{
  if(memcmp(roi_in, roi_out, sizeof(struct dt_iop_roi_t)) || (self->flags() & IOP_FLAGS_TILING_FULL_ROI))
    _default_process_tiling_roi(self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
  else
    _default_process_tiling_ptp(self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
  return;
}

float dt_tiling_estimate_cpumem(struct dt_develop_tiling_t *tiling, struct dt_dev_pixelpipe_iop_t *piece,
                                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                        const int max_bpp)
{
  const int m_dx = MAX(roi_in->width, roi_out->width);
  const int m_dy = MAX(roi_in->height, roi_out->height);
  if(dt_tiling_piece_fits_host_memory(m_dx, m_dy, max_bpp, tiling->factor, tiling->overhead))
    return (float)m_dx * m_dy * max_bpp * tiling->factor + tiling->overhead;

  float fullscale = fmaxf(roi_in->scale / roi_out->scale, sqrtf(((float)roi_in->width * roi_in->height)
                                                              / ((float)roi_out->width * roi_out->height)));
  float available = dt_get_available_mem();
  available = fmaxf(available - ((float)roi_out->width * roi_out->height * max_bpp)
                   - ((float)roi_in->width * roi_in->height * max_bpp) - tiling->overhead, 0.0f);

  float singlebuffer = dt_get_singlebuffer_mem();
  const float factor = fmaxf(tiling->factor, 1.0f);
  const float maxbuf = fmaxf(tiling->maxbuf, 1.0f);
  singlebuffer = fmaxf(available / factor, singlebuffer);

  int width = _max(roi_in->width, roi_out->width);
  int height = _max(roi_in->height, roi_out->height);

  const unsigned int xyalign = _lcm(tiling->xalign, tiling->yalign);
  if((float)width * height * max_bpp * maxbuf > singlebuffer)
  {
    const float scale = singlebuffer / ((float)width * height * max_bpp * maxbuf);
    if(width < height && scale >= 0.333f)
       height = _align_down((int)floorf(height * scale), xyalign);
    else if(height <= width && scale >= 0.333f)
      width = _align_down((int)floorf(width * scale), xyalign);
    else
    {
      width = _align_down((int)floorf(width * sqrtf(scale)), xyalign);
      height = _align_down((int)floorf(height * sqrtf(scale)), xyalign);
    }
  }

  if(3 * tiling->overlap > width || 3 * tiling->overlap > height)
    width = height = _align_down((int)floorf(sqrtf((float)width * height)), xyalign);
  const int overlap_in = _align_up(tiling->overlap, xyalign);
  const int overlap_out = ceilf((float)overlap_in / fullscale);

  int tiles_x = 1, tiles_y = 1;

  if(roi_in->width > roi_out->width)
    tiles_x = (width < roi_in->width) ? ceilf((float)roi_in->width / (float)_max(width - 2 * overlap_in, 1)) : 1;
  else
    tiles_x = (width < roi_out->width) ? ceilf((float)roi_out->width / (float)_max(width - 2 * overlap_out, 1)) : 1;

  if(roi_in->height > roi_out->height)
    tiles_y = (height < roi_in->height) ? ceilf((float)roi_in->height / (float)_max(height - 2 * overlap_in, 1)) : 1;
  else
    tiles_y = (height < roi_out->height) ? ceilf((float)roi_out->height / (float)_max(height - 2 * overlap_out, 1)) : 1;
fprintf(stderr, "tilex = %i, tiley = %i\n", tiles_x, tiles_y);
  return (float)tiles_x * tiles_y * singlebuffer ;
}

#ifdef HAVE_OPENCL
float dt_tiling_estimate_clmem(struct dt_develop_tiling_t *tiling, struct dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                          const int max_bpp)
{
  const int devid = piece->pipe->devid;
  const float fullscale = fmaxf(roi_in->scale / roi_out->scale, sqrtf(((float)roi_in->width * roi_in->height)
                                                              / ((float)roi_out->width * roi_out->height)));
  const gboolean use_pinned_memory = dt_opencl_use_pinned_memory(devid);
  const int pinned_buffer_overhead = use_pinned_memory ? 2 : 0;
  const float pinned_buffer_slack = use_pinned_memory ? 0.85f : 1.0f;
  const float available = (float)dt_opencl_get_device_available(devid);
  const float factor = fmaxf(tiling->factor_cl + pinned_buffer_overhead, 1.0f);
  const float singlebuffer = fminf(fmaxf((available - tiling->overhead) / factor, 0.0f),
                                  pinned_buffer_slack * (float)(dt_opencl_get_device_memalloc(devid)));
  const float maxbuf = fmaxf(tiling->maxbuf_cl, 1.0f);

  int width = _min(_max(roi_in->width, roi_out->width), darktable.opencl->dev[devid].max_image_width);
  int height = _min(_max(roi_in->height, roi_out->height), darktable.opencl->dev[devid].max_image_height);

  unsigned int xyalign = _lcm(tiling->xalign, tiling->yalign);
  xyalign = _lcm(xyalign, CL_ALIGNMENT);

  if((float)width * height * max_bpp * maxbuf > singlebuffer)
  {
    const float scale = singlebuffer / ((float)width * height * max_bpp * maxbuf);

    if(width < height && scale >= 0.333f)
       height = _align_down((int)floorf(height * scale), xyalign);
     else if(height <= width && scale >= 0.333f)
       width = _align_down((int)floorf(width * scale), xyalign);
     else
    {
      width = _align_down((int)floorf(width * sqrtf(scale)), xyalign);
      height = _align_down((int)floorf(height * sqrtf(scale)), xyalign);
    }
  }

  if(3 * tiling->overlap > width || 3 * tiling->overlap > height)
    width = height = _align_down((int)floorf(sqrtf((float)width * height)), xyalign);

  const int overlap_in = _align_up(tiling->overlap, xyalign);
  const int overlap_out = ceilf((float)overlap_in / fullscale);

  int tiles_x = 1, tiles_y = 1;

  if(roi_in->width > roi_out->width)
    tiles_x = (width < roi_in->width) ? ceilf((float)roi_in->width / (float)_max(width - 2 * overlap_in, 1)) : 1;
  else
    tiles_x = (width < roi_out->width) ? ceilf((float)roi_out->width / (float)_max(width - 2 * overlap_out, 1)) : 1;

  if(roi_in->height > roi_out->height)
    tiles_y = (height < roi_in->height) ? ceilf((float)roi_in->height / (float)_max(height - 2 * overlap_in, 1)) : 1;
  else
    tiles_y = (height < roi_out->height) ? ceilf((float)roi_out->height / (float)_max(height - 2 * overlap_out, 1)) : 1;

  return (float)tiles_x * tiles_y * singlebuffer * factor;
}

/* simple tiling algorithm for roi_in == roi_out, i.e. for pixel to pixel modules/operations */
static int _default_process_tiling_cl_ptp(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                          const void *const ivoid, void *const ovoid,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                          const int in_bpp)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem input = NULL;
  cl_mem output = NULL;
  cl_mem pinned_input = NULL;
  cl_mem pinned_output = NULL;
  void *input_buffer = NULL;
  void *output_buffer = NULL;

  dt_print(DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] **** tiling module '%s' for image with size %dx%d --> %dx%d\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, roi_in->width, roi_in->height, roi_out->width, roi_out->height);

  dt_iop_buffer_dsc_t dsc;
  self->output_format(self, piece->pipe, piece, &dsc);
  const int out_bpp = dt_iop_buffer_dsc_to_bpp(&dsc);

  const int devid = piece->pipe->devid;
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);

  /* shall we use pinned memory transfers? */
  gboolean use_pinned_memory = dt_opencl_use_pinned_memory(devid);
  const int pinned_buffer_overhead = use_pinned_memory ? 2 : 0; // add two additional pinned memory buffers
                                                                // which seemingly get allocated not only on
                                                                // host but also on device (why???)
  // avoid problems when pinned buffer size gets too close to max_mem_alloc size
  const float pinned_buffer_slack = use_pinned_memory ? 0.85f : 1.0f;
  const float available = (float)dt_opencl_get_device_available(devid);
  const float factor = fmaxf(tiling.factor_cl + pinned_buffer_overhead, 1.0f);
  const float singlebuffer = fminf(fmaxf((available - tiling.overhead) / factor, 0.0f),
                                  pinned_buffer_slack * (float)(dt_opencl_get_device_memalloc(devid)));
  const float maxbuf = fmaxf(tiling.maxbuf_cl, 1.0f);
  int width = _min(roi_in->width, darktable.opencl->dev[devid].max_image_width);
  int height = _min(roi_in->height, darktable.opencl->dev[devid].max_image_height);

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width * height * max_bpp * maxbuf > singlebuffer)
  {
    const float scale = singlebuffer / ((float)width * height * max_bpp * maxbuf);

    if(width < height && scale >= 0.333f)
    {
      height = floorf(height * scale);
    }
    else if(height <= width && scale >= 0.333f)
    {
      width = floorf(width * scale);
    }
    else
    {
      width = floorf(width * sqrtf(scale));
      height = floorf(height * sqrtf(scale));
    }
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] buffer exceeds singlebuffer, corrected to %dx%d\n",
            dt_dev_pixelpipe_type_to_str(piece->pipe->type), width, height);
  }

  /* make sure we have a reasonably effective tile dimension. if not try square tiles */
  if(3 * tiling.overlap > width || 3 * tiling.overlap > height)
  {
    width = height = floorf(sqrtf((float)width * height));
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] use squares because of overlap, corrected to %dx%d\n",
            dt_dev_pixelpipe_type_to_str(piece->pipe->type), width, height);
  }

  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction. Additional alignment requirements are set via definition of CL_ALIGNMENT.
     We guarantee alignment by selecting image width/height and overlap accordingly. For a tile width/height
     that is identical to image width/height no special alignment is done. */

  /* for simplicity reasons we use only one alignment that fits to x and y requirements at the same time */
  const unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);

  /* determining alignment requirement for tile width/height.
     in case of tile width also align according to definition of CL_ALIGNMENT */
  const unsigned int walign = _lcm(xyalign, CL_ALIGNMENT);
  const unsigned int halign = xyalign;

  assert(xyalign != 0 && walign != 0 && halign != 0);

  /* properly align tile width and height by making them smaller if needed */
  if(width < roi_in->width) width = (width / walign) * walign;
  if(height < roi_in->height) height = (height / halign) * halign;

  /* also make sure that overlap follows alignment rules by making it wider when needed */
  const int overlap = tiling.overlap % xyalign != 0 ? (tiling.overlap / xyalign + 1) * xyalign
                                                    : tiling.overlap;


  /* calculate effective tile size */
  const int tile_wd = width - 2 * overlap > 0 ? width - 2 * overlap : 1;
  const int tile_ht = height - 2 * overlap > 0 ? height - 2 * overlap : 1;


  /* calculate number of tiles */
  const int tiles_x = width < roi_in->width ? ceilf(roi_in->width / (float)tile_wd) : 1;
  const int tiles_y = height < roi_in->height ? ceilf(roi_in->height / (float)tile_ht) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > _maximum_number_tiles())
  {
    dt_print(DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] aborted tiling for module '%s'. too many tiles: %d x %d\n",
             dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, tiles_x, tiles_y);
    return FALSE;
  }

  dt_print(DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] (%dx%d) tiles with max dimensions %dx%d, pinned=%s, good %dx%d and overlap %d\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), tiles_x, tiles_y, width, height, (use_pinned_memory) ? "ON" : "OFF", tile_wd, tile_ht, overlap);

  /* store processed_maximum to be re-used and aggregated */
  dt_aligned_pixel_t processed_maximum_saved;
  dt_aligned_pixel_t processed_maximum_new = { 1.0f };
  for_four_channels(k) processed_maximum_saved[k] = piece->pipe->dsc.processed_maximum[k];

  /* reserve pinned input and output memory for host<->device data transfer */
  if(use_pinned_memory)
  {
    pinned_input = dt_opencl_alloc_device_buffer_with_flags(devid, (size_t)width * height * in_bpp,
                                                            CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR);
    if(pinned_input == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING,
               "[default_process_tiling_cl_ptp] could not alloc pinned input buffer for module '%s'\n", self->op);
      use_pinned_memory = FALSE;
    }
  }

  if(use_pinned_memory)
  {

    input_buffer = dt_opencl_map_buffer(devid, pinned_input, CL_TRUE, CL_MAP_WRITE, 0,
                                        (size_t)width * height * in_bpp);
    if(input_buffer == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] could not map pinned input buffer to host "
                                "memory for module '%s'\n", dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
      use_pinned_memory = FALSE;
    }
  }

  if(use_pinned_memory)
  {

    pinned_output = dt_opencl_alloc_device_buffer_with_flags(devid, (size_t)width * height * out_bpp,
                                                             CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR);
    if(pinned_output == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING,
               "[default_process_tiling_cl_ptp] could not alloc pinned output buffer for module '%s'\n", self->op);
      use_pinned_memory = FALSE;
    }
  }

  if(use_pinned_memory)
  {

    output_buffer = dt_opencl_map_buffer(devid, pinned_output, CL_TRUE, CL_MAP_READ, 0,
                                         (size_t)width * height * out_bpp);
    if(output_buffer == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] could not map pinned output buffer to host "
                                "memory for module '%s'\n", dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
      use_pinned_memory = FALSE;
    }
  }

  /* iterate over tiles */
  for(size_t tx = 0; tx < tiles_x; tx++)
    for(size_t ty = 0; ty < tiles_y; ty++)
    {
      piece->pipe->tiling = 1;

      const size_t wd = tx * tile_wd + width > roi_in->width ? roi_in->width - tx * tile_wd : width;
      const size_t ht = ty * tile_ht + height > roi_in->height ? roi_in->height - ty * tile_ht : height;

      /* no need to process (end)tiles that are smaller than the total overlap area */
      if((wd <= 2 * overlap && tx > 0) || (ht <= 2 * overlap && ty > 0)) continue;

      /* origin and region of effective part of tile, which we want to store later */
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { wd, ht, 1 };

      /* roi_in and roi_out for process_cl on subbuffer */
      dt_iop_roi_t iroi = { roi_in->x + tx * tile_wd, roi_in->y + ty * tile_ht, wd, ht, roi_in->scale };
      dt_iop_roi_t oroi = { roi_out->x + tx * tile_wd, roi_out->y + ty * tile_ht, wd, ht, roi_out->scale };


      /* offsets of tile into ivoid and ovoid */
      const size_t ioffs = (ty * tile_ht) * ipitch + (tx * tile_wd) * in_bpp;
      size_t ooffs = (ty * tile_ht) * opitch + (tx * tile_wd) * out_bpp;


      dt_print(DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] tile (%zu,%zu) size %zux%zu at origin [%zu,%zu]\n",
               dt_dev_pixelpipe_type_to_str(piece->pipe->type), tx, ty, wd, ht, tx * tile_wd, ty * tile_ht);

      /* get input and output buffers */
      input = dt_opencl_alloc_device(devid, wd, ht, in_bpp);
      if(input == NULL) goto error;
      output = dt_opencl_alloc_device(devid, wd, ht, out_bpp);
      if(output == NULL) goto error;

      if(use_pinned_memory)
      {
/* prepare pinned input tile buffer: copy part of input image */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(in_bpp, ipitch, ivoid) \
        dt_omp_sharedconst(ioffs, wd, ht) shared(input_buffer, width) \
        schedule(static)
#endif
        for(size_t j = 0; j < ht; j++)
          memcpy((char *)input_buffer + j * wd * in_bpp, (char *)ivoid + ioffs + j * ipitch,
                 (size_t)wd * in_bpp);

        /* blocking memory transfer: pinned host input buffer -> opencl/device tile */
        err = dt_opencl_write_host_to_device_raw(devid, (char *)input_buffer, input, origin, region,
                                                 wd * in_bpp, CL_TRUE);
        if(err != CL_SUCCESS)
        {
          use_pinned_memory = FALSE;
          goto error;
        }
      }
      else
      {
        /* blocking direct memory transfer: host input image -> opencl/device tile */
        err = dt_opencl_write_host_to_device_raw(devid, (char *)ivoid + ioffs, input, origin, region, ipitch,
                                                 CL_TRUE);
        if(err != CL_SUCCESS) goto error;
      }

      /* take original processed_maximum as starting point */
      for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_saved[k];

      /* call process_cl of module */

      if(!self->process_cl(self, piece, input, output, &iroi, &oroi))
      {
        err = DT_OPENCL_PROCESS_CL;
        goto error;
      }
      /* aggregate resulting processed_maximum */
      /* TODO: check if there really can be differences between tiles and take
               appropriate action (calculate minimum, maximum, average, ...?) */
      for(int k = 0; k < 4; k++)
      {
        if(tx + ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->dsc.processed_maximum[k]) > 1.0e-6f)
          dt_print(DT_DEBUG_TILING, "[default_process_tiling_cl_ptp] [%s] processed_maximum[%d] differs between tiles in module '%s'\n",
              dt_dev_pixelpipe_type_to_str(piece->pipe->type), k, self->op);
        processed_maximum_new[k] = piece->pipe->dsc.processed_maximum[k];
      }

      if(use_pinned_memory)
      {
        /* blocking memory transfer: complete opencl/device tile -> pinned host output buffer */
        err = dt_opencl_read_host_from_device_raw(devid, (char *)output_buffer, output, origin, region,
                                                  wd * out_bpp, CL_TRUE);
        if(err != CL_SUCCESS)
        {
          use_pinned_memory = FALSE;
          goto error;
        }
      }

      /* correct origin and region of tile for overlap.
         makes sure that we only copy back the "good" part. */
      if(tx > 0)
      {
        origin[0] += overlap;
        region[0] -= overlap;
        ooffs += (size_t)overlap * out_bpp;
      }
      if(ty > 0)
      {
        origin[1] += overlap;
        region[1] -= overlap;
        ooffs += (size_t)overlap * opitch;
      }

      if(use_pinned_memory)
      {
/* copy "good" part of tile from pinned output buffer to output image */
#if 0 // def _OPENMP
#pragma omp parallel for default(none) shared(ovoid, ooffs, output_buffer, width, origin, region,            \
                                              wd) schedule(static)
#endif
        for(size_t j = 0; j < region[1]; j++)
          memcpy((char *)ovoid + ooffs + j * opitch,
                 (char *)output_buffer + ((j + origin[1]) * wd + origin[0]) * out_bpp,
                 (size_t)region[0] * out_bpp);
      }
      else
      {
        /* blocking direct memory transfer: good part of opencl/device tile -> host output image */
        err = dt_opencl_read_host_from_device_raw(devid, (char *)ovoid + ooffs, output, origin, region,
                                                  opitch, CL_TRUE);
        if(err != CL_SUCCESS) goto error;
      }

      /* release input and output buffers */
      dt_opencl_release_mem_object(input);
      input = NULL;
      dt_opencl_release_mem_object(output);
      output = NULL;

      /* block until opencl queue has finished to free all used event handlers */
      dt_opencl_finish_sync_pipe(devid, piece->pipe->type);
    }

  /* copy back final processed_maximum */
  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_new[k];

  if(input_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_input, input_buffer);
  dt_opencl_release_mem_object(pinned_input);
  if(output_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_output, output_buffer);
  dt_opencl_release_mem_object(pinned_output);
  dt_opencl_release_mem_object(input);
  dt_opencl_release_mem_object(output);
  piece->pipe->tiling = 0;
  return TRUE;

error:
  /* copy back stored processed_maximum */
  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_saved[k];
  if(input_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_input, input_buffer);
  dt_opencl_release_mem_object(pinned_input);
  if(output_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_output, output_buffer);
  dt_opencl_release_mem_object(pinned_output);
  dt_opencl_release_mem_object(input);
  dt_opencl_release_mem_object(output);
  piece->pipe->tiling = 0;
  const gboolean pinning_error = (use_pinned_memory == FALSE) && dt_opencl_use_pinned_memory(devid);
  dt_print(DT_DEBUG_TILING | DT_DEBUG_OPENCL,
      "[default_process_tiling_opencl_ptp] [%s] couldn't run process_cl() for module '%s' in tiling mode:%s %s\n",
      dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, (pinning_error) ? " pinning problem" : "", cl_errstr(err));
  if(pinning_error) darktable.opencl->dev[devid].runtime_error |= DT_OPENCL_TUNE_PINNED;
  return FALSE;
}


/* more elaborate tiling algorithm for roi_in != roi_out: slower than the ptp variant,
   more tiles and larger overlap */
static int _default_process_tiling_cl_roi(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                          const void *const ivoid, void *const ovoid,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                          const int in_bpp)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem input = NULL;
  cl_mem output = NULL;
  cl_mem pinned_input = NULL;
  cl_mem pinned_output = NULL;
  void *input_buffer = NULL;
  void *output_buffer = NULL;

  dt_print(DT_DEBUG_TILING,
      "[default_process_tiling_cl_roi] [%s] **** tiling module '%s' for image with input size %dx%d --> %dx%d\n",
      dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, roi_in->width, roi_in->height, roi_out->width, roi_out->height);
  _print_roi(roi_in, "module roi_in");
  _print_roi(roi_out, "module roi_out");

  dt_iop_buffer_dsc_t dsc;
  self->output_format(self, piece->pipe, piece, &dsc);
  const int out_bpp = dt_iop_buffer_dsc_to_bpp(&dsc);

  const int devid = piece->pipe->devid;
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  const float fullscale = fmaxf(roi_in->scale / roi_out->scale, sqrtf(((float)roi_in->width * roi_in->height)
                                                              / ((float)roi_out->width * roi_out->height)));

  /* inaccuracy for roi_in elements in roi_out -> roi_in calculations */
  const int delta = ceilf(fullscale);

  /* estimate for additional (space) requirement in buffer dimensions due to inaccuracies */
  const int inacc = RESERVE * delta;

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);

  /* shall we use pinned memory transfers? */
  gboolean use_pinned_memory = dt_opencl_use_pinned_memory(devid);
  const int pinned_buffer_overhead = use_pinned_memory ? 2 : 0; // add two additional pinned memory buffers
                                                                // which seemingly get allocated not only on
                                                                // host but also on device (why???)
  // avoid problems when pinned buffer size gets too close to max_mem_alloc size
  const float pinned_buffer_slack = use_pinned_memory ? 0.85f : 1.0f;
  const float available = (float)dt_opencl_get_device_available(devid);
  const float factor = fmaxf(tiling.factor_cl + pinned_buffer_overhead, 1.0f);
  const float singlebuffer = fminf(fmaxf((available - tiling.overhead) / factor, 0.0f),
                                  pinned_buffer_slack * (float)(dt_opencl_get_device_memalloc(devid)));
  const float maxbuf = fmaxf(tiling.maxbuf_cl, 1.0f);

  int width = _min(_max(roi_in->width, roi_out->width), darktable.opencl->dev[devid].max_image_width);
  int height = _min(_max(roi_in->height, roi_out->height), darktable.opencl->dev[devid].max_image_height);

  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction. Additional alignment requirements are set via definition of CL_ALIGNMENT. */

  /* for simplicity reasons we use only one alignment that fits to x and y requirements at the same time */
  unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);
  xyalign = _lcm(xyalign, CL_ALIGNMENT);

  assert(xyalign != 0);

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width * height * max_bpp * maxbuf > singlebuffer)
  {
    const float scale = singlebuffer / ((float)width * height * max_bpp * maxbuf);

    if(width < height && scale >= 0.333f)
    {
      height = _align_down((int)floorf(height * scale), xyalign);
    }
    else if(height <= width && scale >= 0.333f)
    {
      width = _align_down((int)floorf(width * scale), xyalign);
    }
    else
    {
      width = _align_down((int)floorf(width * sqrtf(scale)), xyalign);
      height = _align_down((int)floorf(height * sqrtf(scale)), xyalign);
    }
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_cl_roi] [%s] buffer exceeds singlebuffer, corrected to %dx%d\n",
            dt_dev_pixelpipe_type_to_str(piece->pipe->type), width, height);
  }

  /* make sure we have a reasonably effective tile dimension. if not try square tiles */
  if(3 * tiling.overlap > width || 3 * tiling.overlap > height)
  {
    width = height = _align_down((int)floorf(sqrtf((float)width * height)), xyalign);
    dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_cl_roi] [%s] use squares because of overlap, corrected to %dx%d\n",
            dt_dev_pixelpipe_type_to_str(piece->pipe->type), width, height);
  }

  /* make sure that overlap follows alignment rules by making it wider when needed.
     overlap_in needs to be aligned, overlap_out is only here to calculate output buffer size */
  const int overlap_in = _align_up(tiling.overlap, xyalign);
  const int overlap_out = ceilf((float)overlap_in / fullscale);

  int tiles_x = 1, tiles_y = 1;

  /* calculate number of tiles taking the larger buffer (input or output) as a guiding one.
     normally it is roi_in > roi_out; but let's be prepared */
  if(roi_in->width > roi_out->width)
    tiles_x = width < roi_in->width
                  ? ceilf((float)roi_in->width / (float)_max(width - 2 * overlap_in - inacc, 1))
                  : 1;
  else
    tiles_x = width < roi_out->width ? ceilf((float)roi_out->width / (float)_max(width - 2 * overlap_out, 1))
                                     : 1;

  if(roi_in->height > roi_out->height)
    tiles_y = height < roi_in->height
                  ? ceilf((float)roi_in->height / (float)_max(height - 2 * overlap_in - inacc, 1))
                  : 1;
  else
    tiles_y = height < roi_out->height
                  ? ceilf((float)roi_out->height / (float)_max(height - 2 * overlap_out, 1))
                  : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > _maximum_number_tiles())
  {
    dt_print(DT_DEBUG_TILING,
             "[default_process_tiling_cl_roi] [%s] aborted tiling for module '%s'. too many tiles: %dx%d\n",
             dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, tiles_x, tiles_y);
    return FALSE;
  }

  /* calculate tile width and height excl. overlap (i.e. the good part) for output.
     important for all following processing steps. */
  const int tile_wd = _align_up(
      roi_out->width % tiles_x == 0 ? roi_out->width / tiles_x : roi_out->width / tiles_x + 1, xyalign);
  const int tile_ht = _align_up(
      roi_out->height % tiles_y == 0 ? roi_out->height / tiles_y : roi_out->height / tiles_y + 1, xyalign);

  dt_print(DT_DEBUG_TILING,
           "[default_process_tiling_cl_roi] [%s] (%dx%d) tiles with max input dimensions %dx%d, pinned=%s, good %ix%i\n",
           dt_dev_pixelpipe_type_to_str(piece->pipe->type), tiles_x, tiles_y, width, height, (use_pinned_memory) ? "ON" : "OFF", tile_wd, tile_ht);


  /* store processed_maximum to be re-used and aggregated */
  dt_aligned_pixel_t processed_maximum_saved;
  dt_aligned_pixel_t processed_maximum_new = { 1.0f };
  for_four_channels(k) processed_maximum_saved[k] = piece->pipe->dsc.processed_maximum[k];

  /* reserve pinned input and output memory for host<->device data transfer */
  if(use_pinned_memory)
  {
    pinned_input = dt_opencl_alloc_device_buffer_with_flags(devid, (size_t)width * height * in_bpp,
                                                            CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR);
    if(pinned_input == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING,
               "[default_process_tiling_cl_roi] [%s] could not alloc pinned input buffer for module '%s'\n",
               dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
      use_pinned_memory = FALSE;
    }
  }

  if(use_pinned_memory)
  {

    input_buffer = dt_opencl_map_buffer(devid, pinned_input, CL_TRUE, CL_MAP_WRITE, 0,
                                        (size_t)width * height * in_bpp);
    if(input_buffer == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[default_process_tiling_cl_roi] [%s] could not map pinned input buffer to host "
                                "memory for module '%s'\n", dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
      use_pinned_memory = FALSE;
    }
  }

  if(use_pinned_memory)
  {

    pinned_output = dt_opencl_alloc_device_buffer_with_flags(devid, (size_t)width * height * out_bpp,
                                                             CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR);
    if(pinned_output == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[default_process_tiling_cl_roi] [%s] could not alloc pinned output buffer for module '%s'\n",
        dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
      use_pinned_memory = FALSE;
    }
  }

  if(use_pinned_memory)
  {

    output_buffer = dt_opencl_map_buffer(devid, pinned_output, CL_TRUE, CL_MAP_READ, 0,
                                         (size_t)width * height * out_bpp);
    if(output_buffer == NULL)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[default_process_tiling_cl_roi] [%s] could not map pinned output buffer to host "
                                "memory for module '%s'\n", dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
      use_pinned_memory = FALSE;
    }
  }


  /* iterate over tiles */
  for(size_t tx = 0; tx < tiles_x; tx++)
    for(size_t ty = 0; ty < tiles_y; ty++)
    {
      piece->pipe->tiling = 1;

      /* the output dimensions of the good part of this specific tile */
      const size_t wd = (tx + 1) * tile_wd > roi_out->width ? (size_t)roi_out->width - tx * tile_wd : tile_wd;
      const size_t ht = (ty + 1) * tile_ht > roi_out->height ? (size_t)roi_out->height - ty * tile_ht : tile_ht;

      /* roi_in and roi_out of good part: oroi_good easy to calculate based on number and dimension of tile.
         iroi_good is calculated by modify_roi_in() of respective module */
      dt_iop_roi_t iroi_good = { roi_in->x  + tx * tile_wd, roi_in->y  + ty * tile_ht, wd, ht, roi_in->scale };
      dt_iop_roi_t oroi_good = { roi_out->x + tx * tile_wd, roi_out->y + ty * tile_ht, wd, ht, roi_out->scale };

      self->modify_roi_in(self, piece, &oroi_good, &iroi_good);

      /* clamp iroi_good to not exceed roi_in */
      iroi_good.x = _max(iroi_good.x, roi_in->x);
      iroi_good.y = _max(iroi_good.y, roi_in->y);
      iroi_good.width = _min(iroi_good.width, roi_in->width + roi_in->x - iroi_good.x);
      iroi_good.height = _min(iroi_good.height, roi_in->height + roi_in->y - iroi_good.y);

      _print_roi(&iroi_good, "tile iroi_good");
      _print_roi(&oroi_good, "tile oroi_good");

      /* now we need to calculate full region of this tile: increase input roi to take care of overlap
         requirements
         and alignment and add additional delta to correct for possible rounding errors in modify_roi_in()
         -> generates first estimate of iroi_full */
      const int x_in = iroi_good.x;
      const int y_in = iroi_good.y;
      const int width_in = iroi_good.width;
      const int height_in = iroi_good.height;
      const int new_x_in = _max(_align_close(x_in - overlap_in - delta, xyalign), roi_in->x);
      const int new_y_in = _max(_align_close(y_in - overlap_in - delta, xyalign), roi_in->y);
      const int new_width_in = _min(_align_up(width_in + overlap_in + delta + (x_in - new_x_in), xyalign),
                                    roi_in->width + roi_in->x - new_x_in);
      const int new_height_in = _min(_align_up(height_in + overlap_in + delta + (y_in - new_y_in), xyalign),
                                     roi_in->height + roi_in->y - new_y_in);

      /* iroi_full based on calculated numbers and dimensions. oroi_full just set as a starting point for the
       * following iterative search */
      dt_iop_roi_t iroi_full = { new_x_in, new_y_in, new_width_in, new_height_in, iroi_good.scale };
      dt_iop_roi_t oroi_full = oroi_good; // a good starting point for optimization

      _print_roi(&iroi_full, "tile iroi_full before optimization");
      _print_roi(&oroi_full, "tile oroi_full before optimization");

      /* try to find a matching oroi_full */
      if(!_fit_output_to_input_roi(self, piece, &iroi_full, &oroi_full, delta, 10))
      {
        dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[default_process_tiling_cl_roi] [%s] can not handle requested roi's tiling "
                                  "for module '%s' not possible.\n",
                 dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op);
        goto error;
      }


      /* make sure that oroi_full at least covers the range of oroi_good.
         this step is needed due to the possibility of rounding errors */
      oroi_full.x = _min(oroi_full.x, oroi_good.x);
      oroi_full.y = _min(oroi_full.y, oroi_good.y);
      oroi_full.width = _max(oroi_full.width, oroi_good.x + oroi_good.width - oroi_full.x);
      oroi_full.height = _max(oroi_full.height, oroi_good.y + oroi_good.height - oroi_full.y);

      /* clamp oroi_full to not exceed roi_out */
      oroi_full.x = _max(oroi_full.x, roi_out->x);
      oroi_full.y = _max(oroi_full.y, roi_out->y);
      oroi_full.width = _min(oroi_full.width, roi_out->width + roi_out->x - oroi_full.x);
      oroi_full.height = _min(oroi_full.height, roi_out->height + roi_out->y - oroi_full.y);


      /* calculate final iroi_full */
      self->modify_roi_in(self, piece, &oroi_full, &iroi_full);

      /* clamp iroi_full to not exceed roi_in */
      iroi_full.x = _max(iroi_full.x, roi_in->x);
      iroi_full.y = _max(iroi_full.y, roi_in->y);
      iroi_full.width = _min(iroi_full.width, roi_in->width + roi_in->x - iroi_full.x);
      iroi_full.height = _min(iroi_full.height, roi_in->height + roi_in->y - iroi_full.y);

      _print_roi(&iroi_full, "tile iroi_full");
      _print_roi(&oroi_full, "tile oroi_full");

      /* offsets of tile into ivoid and ovoid */
      const int in_dx = iroi_full.x - roi_in->x;
      const int in_dy = iroi_full.y - roi_in->y;
      const int out_dx = oroi_good.x - roi_out->x;
      const int out_dy = oroi_good.y - roi_out->y;
      const size_t ioffs = (size_t)(in_dy  * ipitch) + (size_t)(in_dx * in_bpp);
      const size_t ooffs = (size_t)(out_dy * opitch) + (size_t)(out_dx * out_bpp);

      /* origin and region of full input tile */
      size_t iorigin[] = { 0, 0, 0 };
      size_t iregion[] = { iroi_full.width, iroi_full.height, 1 };

      /* origin and region of full output tile */
      size_t oforigin[] = { 0, 0, 0 };
      size_t ofregion[] = { oroi_full.width, oroi_full.height, 1 };

      /* origin and region of good part of output tile */
      size_t oorigin[] = { oroi_good.x - oroi_full.x, oroi_good.y - oroi_full.y, 0 };
      size_t oregion[] = { oroi_good.width, oroi_good.height, 1 };

      dt_print(DT_DEBUG_TILING,  "[default_process_tiling_cl_roi] [%s] process tile (%zu,%zu) size %dx%d at origin [%d,%d]\n",
               dt_dev_pixelpipe_type_to_str(piece->pipe->type), tx, ty, iroi_full.width, iroi_full.height, iroi_full.x, iroi_full.y);
      dt_vprint(DT_DEBUG_TILING, "[default_process_tiling_cl_roi]    dest [%lu,%lu] at [%lu,%lu], offsets [%i,%i] -> [%i,%i], delta=%i\n\n",
               oregion[0], oregion[1], oorigin[0], oorigin[1], in_dx, in_dy, out_dx, out_dy, delta);

      /* get opencl input and output buffers */
      input = dt_opencl_alloc_device(devid, iroi_full.width, iroi_full.height, in_bpp);
      if(input == NULL) goto error;

      output = dt_opencl_alloc_device(devid, oroi_full.width, oroi_full.height, out_bpp);
      if(output == NULL) goto error;

      if(use_pinned_memory)
      {
/* prepare pinned input tile buffer: copy part of input image */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(in_bpp, ipitch, ivoid) \
        dt_omp_sharedconst(ioffs) shared(input_buffer, width, iroi_full) schedule(static)
#endif
        for(size_t j = 0; j < iroi_full.height; j++)
          memcpy((char *)input_buffer + j * iroi_full.width * in_bpp, (char *)ivoid + ioffs + j * ipitch,
                 (size_t)iroi_full.width * in_bpp);

        /* blocking memory transfer: pinned host input buffer -> opencl/device tile */
        err = dt_opencl_write_host_to_device_raw(devid, (char *)input_buffer, input, iorigin, iregion,
                                                 (size_t)iroi_full.width * in_bpp, CL_TRUE);
        if(err != CL_SUCCESS)
        {
          use_pinned_memory = FALSE;
          goto error;
        }
      }
      else
      {
        /* blocking direct memory transfer: host input image -> opencl/device tile */
        err = dt_opencl_write_host_to_device_raw(devid, (char *)ivoid + ioffs, input, iorigin, iregion,
                                                 ipitch, CL_TRUE);
        if(err != CL_SUCCESS) goto error;
      }

      /* take original processed_maximum as starting point */
      for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_saved[k];

      /* call process_cl of module */
      if(!self->process_cl(self, piece, input, output, &iroi_full, &oroi_full))
      {
        err = DT_OPENCL_PROCESS_CL;
        goto error;
      }
      /* aggregate resulting processed_maximum */
      /* TODO: check if there really can be differences between tiles and take
               appropriate action (calculate minimum, maximum, average, ...?) */
      for(int k = 0; k < 4; k++)
      {
        if(tx + ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->dsc.processed_maximum[k]) > 1.0e-6f)
          dt_print(DT_DEBUG_TILING,
              "[default_process_tiling_cl_roi] [%s] processed_maximum[%d] differs between tiles in module '%s'\n",
              dt_dev_pixelpipe_type_to_str(piece->pipe->type), k, self->op);
        processed_maximum_new[k] = piece->pipe->dsc.processed_maximum[k];
      }

      if(use_pinned_memory)
      {
        /* blocking memory transfer: complete opencl/device tile -> pinned host output buffer */
        err = dt_opencl_read_host_from_device_raw(devid, (char *)output_buffer, output, oforigin, ofregion,
                                                  (size_t)oroi_full.width * out_bpp, CL_TRUE);
        if(err != CL_SUCCESS)
        {
          use_pinned_memory = FALSE;
          goto error;
        }
/* copy "good" part of tile from pinned output buffer to output image */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(ipitch, opitch, ovoid, out_bpp) \
        dt_omp_sharedconst(ooffs) shared(output_buffer, oroi_full, oorigin, oregion) \
        schedule(static)
#endif
        for(size_t j = 0; j < oregion[1]; j++)
          memcpy((char *)ovoid + ooffs + j * opitch,
                 (char *)output_buffer + ((j + oorigin[1]) * oroi_full.width + oorigin[0]) * out_bpp,
                 (size_t)oregion[0] * out_bpp);
      }
      else
      {
        /* blocking direct memory transfer: good part of opencl/device tile -> host output image */
        err = dt_opencl_read_host_from_device_raw(devid, (char *)ovoid + ooffs, output, oorigin, oregion,
                                                  opitch, CL_TRUE);
        if(err != CL_SUCCESS) goto error;
      }

      /* release input and output buffers */
      dt_opencl_release_mem_object(input);
      input = NULL;
      dt_opencl_release_mem_object(output);
      output = NULL;

      /* block until opencl queue has finished to free all used event handlers */
      dt_opencl_finish_sync_pipe(devid, piece->pipe->type);
    }

  /* copy back final processed_maximum */
  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_new[k];
  if(input_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_input, input_buffer);
  dt_opencl_release_mem_object(pinned_input);
  if(output_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_output, output_buffer);
  dt_opencl_release_mem_object(pinned_output);
  dt_opencl_release_mem_object(input);
  dt_opencl_release_mem_object(output);
  piece->pipe->tiling = 0;
  return TRUE;

error:
  /* copy back stored processed_maximum */
  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = processed_maximum_saved[k];
  if(input_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_input, input_buffer);
  dt_opencl_release_mem_object(pinned_input);
  if(output_buffer != NULL) dt_opencl_unmap_mem_object(devid, pinned_output, output_buffer);
  dt_opencl_release_mem_object(pinned_output);
  dt_opencl_release_mem_object(input);
  dt_opencl_release_mem_object(output);
  piece->pipe->tiling = 0;
  const gboolean pinning_error = (use_pinned_memory == FALSE) && dt_opencl_use_pinned_memory(devid);
  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING,
      "[default_process_tiling_opencl_roi] [%s] couldn't run process_cl() for module '%s' in tiling mode:%s %s\n",
      dt_dev_pixelpipe_type_to_str(piece->pipe->type), self->op, (pinning_error) ? " pinning problem" : "", cl_errstr(err));
  if(pinning_error) darktable.opencl->dev[devid].runtime_error |= DT_OPENCL_TUNE_PINNED;
  return FALSE;
}



/* if a module does not implement process_tiling_cl() by itself, this function is called instead.
   _default_process_tiling_cl_ptp() is able to handle standard cases where pixels do not change their places.
   _default_process_tiling_cl_roi() takes care of all other cases where image gets distorted. */
int default_process_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const int in_bpp)
{
  if(memcmp(roi_in, roi_out, sizeof(struct dt_iop_roi_t)) || (self->flags() & IOP_FLAGS_TILING_FULL_ROI))
    return _default_process_tiling_cl_roi(self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
  else
    return _default_process_tiling_cl_ptp(self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
}

#else
int default_process_tiling_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const int in_bpp)
{
  return FALSE;
}
#endif


/* If a module does not implement tiling_callback() by itself, this function is called instead.
   Default is an image size factor of 2 (i.e. input + output buffer needed), no overhead (1),
   no overlap between tiles, and an pixel alignment of 1 in x and y direction, i.e. no special
   alignment required. Simple pixel to pixel modules (take tonecurve as an example) can happily
   live with that.
   (1) Small overhead like look-up-tables in tonecurve can be ignored safely. */
void default_tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                             struct dt_develop_tiling_t *tiling)
{
  const float ioratio
      = ((float)roi_out->width * (float)roi_out->height) / ((float)roi_in->width * (float)roi_in->height);

  tiling->factor = 1.0f + ioratio;
  tiling->factor_cl = tiling->factor;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = tiling->maxbuf;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;

  if((self->flags() & IOP_FLAGS_TILING_FULL_ROI) == IOP_FLAGS_TILING_FULL_ROI) tiling->overlap = 4;

  if(self->iop_order > dt_ioppr_get_iop_order(piece->pipe->iop_order_list, "demosaic", 0)) return;

  // all operations that work with mosaiced data should respect pattern size!

  if(!piece->pipe->dsc.filters) return;

  if(piece->pipe->dsc.filters == 9u)
  {
    // X-Trans, sensor is 6x6 but algorithms have been corrected to work with 3x3
    tiling->xalign = 3;
    tiling->yalign = 3;
  }
  else
  {
    // Bayer, good old 2x2
    tiling->xalign = 2;
    tiling->yalign = 2;
  }

  return;
}

gboolean dt_tiling_piece_fits_host_memory(const size_t width, const size_t height, const unsigned bpp,
                                     const float factor, const size_t overhead)
{
  const size_t available = dt_get_available_mem();
  const size_t total = factor * width * height * bpp + overhead;

  return (total <= available) ? TRUE : FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


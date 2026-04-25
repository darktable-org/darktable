/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <stddef.h>
#include <potracelib.h>

#include "develop/masks.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/* Macros as in Inkscape */
#define BM_WORDBITS   (8 * (int)sizeof(potrace_word))
#define BM_HIBIT      ((potrace_word)1 << (BM_WORDBITS-1))

#define bm_scanline(bm, y) ((bm)->map + (ptrdiff_t)(y) * (ptrdiff_t)(bm)->dy)
#define bm_index(bm,x,y)   (&bm_scanline(bm,y)[ (x) / BM_WORDBITS ])
#define bm_mask(x)         (BM_HIBIT >> ((x) & (BM_WORDBITS-1)))

#define BM_USET(bm,x,y)    (*bm_index(bm,x,y) |=  bm_mask(x))
#define BM_UCLR(bm,x,y)    (*bm_index(bm,x,y) &= ~bm_mask(x))

static potrace_bitmap_t *_bm_new(const int w,
                                 const int h)
{
  potrace_bitmap_t *bm = calloc(1, sizeof(*bm));
  if(!bm) return NULL;

  bm->w = w;
  bm->h = h;
  bm->dy = (w + BM_WORDBITS - 1) / BM_WORDBITS; /* words per scanline */
  const size_t total_words = (size_t)bm->dy * h;
  bm->map = calloc(total_words, sizeof(potrace_word));

  if(!bm->map)
  {
    free(bm);
    return NULL;
  }

  return bm;
}

static void _bm_free(potrace_bitmap_t *bm)
{
  if(bm)
  {
    free(bm->map);
    free(bm);
  }
}

#define SET_THRESHOLD 0.6f

static inline void _scale_point(float p[2],
                                const float xscale,
                                const float yscale,
                                const float cx,
                                const float cy,
                                const float iwidth,
                                const float iheight)
{
  p[0] = ((p[0] * xscale) + cx) / iwidth;
  p[1] = ((p[1] * yscale) + cy) / iheight;
}

static void _add_point(dt_masks_form_t *form,
                       const dt_image_t *const image,
                       const float width,
                       const float height,
                       const float x,
                       const float y,
                       const float ctl1_x,
                       const float ctl1_y,
                       const float ctl2_x,
                       const float ctl2_y)
{
  dt_masks_point_path_t *bzpt = calloc(1, sizeof(dt_masks_point_path_t));

  bzpt->corner[0] = x;
  bzpt->corner[1] = y;

  // set the control points if defined
  if(ctl1_x > 0)
  {
    bzpt->ctrl1[0] = ctl1_x;
    bzpt->ctrl1[1] = ctl1_y;
    bzpt->ctrl2[0] = ctl2_x;
    bzpt->ctrl2[1] = ctl2_y;
  }
  else
  {
    bzpt->ctrl1[0] = x;
    bzpt->ctrl1[1] = y;
    bzpt->ctrl2[0] = x;
    bzpt->ctrl2[1] = y;
  }

  if(image)
  {
    const float iwidth = image->width;
    const float iheight = image->height;
    const float pwidth = image->p_width;
    const float pheight = image->p_height;
    const float cx = image->crop_x;
    const float cy = image->crop_y;

    const float xscale = pwidth / (float)width;
    const float yscale = pheight / (float)height;

    _scale_point(bzpt->corner, xscale, yscale, cx, cy, iwidth, iheight);
    _scale_point(bzpt->ctrl1, xscale, yscale, cx, cy, iwidth, iheight);
    _scale_point(bzpt->ctrl2, xscale, yscale, cx, cy, iwidth, iheight);
  }

  bzpt->state = DT_MASKS_POINT_STATE_USER;
  bzpt->border[0] = bzpt->border[1] = 0.f;

  form->points = g_list_append(form->points, bzpt);
}

static gint formnb = 0;

GList *ras2forms(const float *mask,
                 const int width,
                 const int height,
                 const dt_image_t *const image,
                 const int turdsize,
                 const double alphamax,
                 GList **out_signs)
{
  GList *forms = NULL;
  GList *signs = NULL;

  //  create bitmap mask for potrace

  potrace_bitmap_t *bm = _bm_new(width, height);

  DT_OMP_FOR()
  for(int y=0; y < height; y++)
  {
    for(int x=0; x < width; x++)
    {
      const int index = x + y * width;
      if(mask[index] < SET_THRESHOLD)
      {
        // black enough to be a point of the form
        BM_USET(bm, x, y);
      }
      else
      {
        BM_UCLR(bm, x, y);
      }
    }
  }

  potrace_param_t *param = potrace_param_default();
  // finer path possible
  param->turdsize = turdsize > 0 ? turdsize : 50; // ignore area whose size are < 50
  param->alphamax = alphamax;
  param->turnpolicy = POTRACE_TURNPOLICY_MINORITY;
  param->opticurve = 1;
  param->opttolerance = 0.8;

  potrace_state_t *st = potrace_trace(param, bm);

  //  get all paths, create corresponding path form

  for(const potrace_path_t *p = st->plist;
      p;
      p = p->next)
  {
    const potrace_curve_t *cv = &p->curve;
    const int n = cv->n;

    // Start = end of last segment
    const potrace_dpoint_t start = cv->c[n-1][2];

    dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
    snprintf(form->name, sizeof(form->name), "path raster %d",
             g_atomic_int_add(&formnb, 1) + 1);

    // Potrace outputs cubic Bezier segments where:
    //   c[i][0] = outgoing ctrl of the segment's start point
    //   c[i][1] = incoming ctrl of the segment's end point
    //   c[i][2] = endpoint
    // darktable stores per point: ctrl1 = incoming handle, ctrl2 = outgoing.
    // We must split each segment's control pair across two adjacent points.

    // precompute image scaling factors (used for handle coordinate transform)
    const float xsc = image ? image->p_width / (float)width : 0.0f;
    const float ysc = image ? image->p_height / (float)height : 0.0f;

    // add all corner points with zero-length handles
    _add_point(form, image, width, height, start.x, start.y, -1, -1, -1, -1);

    for(int i = 0; i < n; i++)
    {
      if(cv->tag[i] == POTRACE_CURVETO)
      {
        const potrace_dpoint_t e = cv->c[i][2];
        _add_point(form, image, width, height, e.x, e.y, -1, -1, -1, -1);
      }
      else // POTRACE_CORNER
      {
        const potrace_dpoint_t v = cv->c[i][1];
        const potrace_dpoint_t e = cv->c[i][2];

        _add_point(form, image, width, height, v.x, v.y, -1, -1, -1, -1);
        _add_point(form, image, width, height, e.x, e.y, -1, -1, -1, -1);
      }
    }

    // assign Bezier handles: for each CURVETO segment, set
    //   start_point.ctrl2 = c[i][0]  (outgoing)
    //   end_point.ctrl1   = c[i][1]  (incoming)
    // the path is closed, so the last segment wraps back to the start point
    {
      GList *pt = form->points;  // start point
      for(int i = 0; i < n; i++)
      {
        if(cv->tag[i] == POTRACE_CURVETO)
        {
          const potrace_dpoint_t c0 = cv->c[i][0];
          const potrace_dpoint_t c1 = cv->c[i][1];

          // outgoing handle of current (start-of-segment) point
          dt_masks_point_path_t *ps = pt->data;
          ps->ctrl2[0] = c0.x;
          ps->ctrl2[1] = c0.y;
          if(image)
            _scale_point(ps->ctrl2, xsc, ysc,
                         image->crop_x, image->crop_y,
                         image->width, image->height);

          // advance to endpoint (wrap to start for the closing segment)
          pt = g_list_next(pt);
          if(!pt) pt = form->points;

          // incoming handle of end-of-segment point
          dt_masks_point_path_t *pe = pt->data;
          pe->ctrl1[0] = c1.x;
          pe->ctrl1[1] = c1.y;
          if(image)
            _scale_point(pe->ctrl1, xsc, ysc,
                         image->crop_x, image->crop_y,
                         image->width, image->height);
        }
        else // POTRACE_CORNER: two points added, no Bezier handles
        {
          pt = g_list_next(pt); if(!pt) pt = form->points;
          pt = g_list_next(pt); if(!pt) pt = form->points;
        }
      }
    }

    forms = g_list_prepend(forms, form);
    if(out_signs)
      signs = g_list_prepend(signs, GINT_TO_POINTER(p->sign));
  }

  potrace_state_free(st);
  potrace_param_free(param);
  _bm_free(bm);

  // restore potrace's traversal order (outer first, then its holes).
  // group consumers need the outer at list position 0 so it acts as the
  // base while holes subtract on top with DIFFERENCE mode
  forms = g_list_reverse(forms);
  if(out_signs) *out_signs = g_list_reverse(signs);
  return forms;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

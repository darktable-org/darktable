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

static uint32_t formnb = 0;

GList *ras2forms(const float *mask,
                 const int width,
                 const int height,
                 const dt_image_t *const image)
{
  GList *forms = NULL;

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
  param->alphamax = 0.0f;

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
    snprintf(form->name, sizeof(form->name), "path raster %d", ++formnb);

    _add_point(form, image, width, height, start.x, start.y, -1, -1, -1, -1);

    for(int i = 0; i < n; i++)
    {
      if(cv->tag[i] == POTRACE_CURVETO)
      {
        const potrace_dpoint_t c0 = cv->c[i][0];
        const potrace_dpoint_t c1 = cv->c[i][1];
        const potrace_dpoint_t e  = cv->c[i][2];

        _add_point(form, image, width, height, e.x, e.y, c0.x, c0.y, c1.x, c1.y);
      }
      else // POTRACE_CORNER
      {
        const potrace_dpoint_t v = cv->c[i][1];
        const potrace_dpoint_t e = cv->c[i][2];

        _add_point(form, image, width, height, v.x, v.y, -1, -1, -1, -1);
        _add_point(form, image, width, height, e.x, e.y, -1, -1, -1, -1);
      }
    }

    forms = g_list_prepend(forms, form);
  }

  potrace_state_free(st);
  potrace_param_free(param);
  _bm_free(bm);

  return forms;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"

static int _group_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                        uint32_t state, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                        int unused)
{
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel && sel->functions)
      return sel->functions->mouse_scrolled(module, pzx, pzy, up, state, sel, fpt->parentid, gui, gui->group_edited);
  }
  return 0;
}

static int _group_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                        double pressure, int which, int type, uint32_t state,
                                        dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui, int unused2)
{
  if(gui->group_edited != gui->group_selected)
  {
    // we set the selected form in edit mode
    gui->group_edited = gui->group_selected;
    // we initialise some variable
    gui->dx = gui->dy = 0.0f;
    gui->form_selected = gui->border_selected = gui->form_dragging = gui->form_rotating = FALSE;
    gui->pivot_selected = FALSE;
    gui->point_border_selected = gui->seg_selected = gui->point_selected = gui->feather_selected = -1;
    gui->point_border_dragging = gui->seg_dragging = gui->feather_dragging = gui->point_dragging = -1;

    dt_control_queue_redraw_center();
    return 1;
  }
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return 0;
    if(sel->functions)
      return sel->functions->button_pressed(module, pzx, pzy, pressure, which, type, state, sel,
                                           fpt->parentid, gui, gui->group_edited);
  }
  return 0;
}

static int _group_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                         uint32_t state, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                         int unused2)
{
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel && sel->functions)
      return sel->functions->button_released(module, pzx, pzy, which, state, sel, fpt->parentid, gui,
                                             gui->group_edited);
  }
  return 0;
}

static inline gboolean _is_handling_form(dt_masks_form_gui_t *gui)
{
  return gui->form_dragging
    || gui->source_dragging
    || gui->gradient_toggling
    || gui->form_rotating
    || (gui->point_edited != -1)
    || (gui->point_dragging != -1)
    || (gui->feather_dragging != -1)
    || (gui->point_border_dragging != -1)
    || (gui->seg_dragging != -1);
}

static int _group_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                     int which, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                     int unused2)
{
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
  const float pr_d = darktable.develop->preview_downsampling;
  const float as = DT_PIXEL_APPLY_DPI(5) / (pr_d * zoom_scale);  // transformed to backbuf dimensions

  // we first don't do anything if we are inside a scrolling session

  if(gui->scrollx != 0.0f && gui->scrolly != 0.0f)
  {
    const float as2 = 0.015f / zoom_scale;
    if((gui->scrollx - pzx < as2 && gui->scrollx - pzx > -as2)
       && (gui->scrolly - pzy < as2 && gui->scrolly - pzy > -as2))
      return 1;
    gui->scrollx = gui->scrolly = 0.0f;
  }

  // if a form is in edit mode and we are dragging, don't try to select another form
  if(gui->group_edited >= 0 && _is_handling_form(gui))
  {
    // we get the form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return 0;
    int rep = 0;
    if(sel->functions)
      rep = sel->functions->mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui,
                                       gui->group_edited);
    if(rep) return 1;
    // if a point is in state editing, then we don't want that another form can be selected
    if(gui->point_edited >= 0) return 0;
  }

  // now we check if we are near a form
  int pos = 0;
  gui->form_selected = gui->border_selected = FALSE;
  gui->source_selected = gui->source_dragging = FALSE;
  gui->pivot_selected = FALSE;
  gui->feather_selected = -1;
  gui->point_edited = gui->point_selected = -1;
  gui->seg_selected = -1;
  gui->point_border_selected = -1;
  gui->group_edited = gui->group_selected = -1;

  dt_masks_form_t *sel = NULL;
  dt_masks_point_group_t *sel_fpt = NULL;
  int sel_pos = 0;
  float sel_dist = FLT_MAX;

  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *frm = dt_masks_get_from_id(darktable.develop, fpt->formid);
    int inside, inside_border, near, inside_source;
    float dist = FLT_MAX;
    inside = inside_border = inside_source = 0;
    near = -1;
    const float xx = pzx * darktable.develop->preview_pipe->backbuf_width,
                yy = pzy * darktable.develop->preview_pipe->backbuf_height;
    if(frm->functions && frm->functions->get_distance)
      frm->functions->get_distance(xx, yy, as, gui, pos, g_list_length(frm->points),
                                   &inside, &inside_border, &near, &inside_source, &dist);

    if(inside || inside_border || near >= 0 || inside_source)
    {

      if(sel_dist > dist)
      {
        sel = frm;
        sel_dist = dist;
        sel_pos = pos;
        sel_fpt = fpt;
      }
    }
    pos++;
  }

  if(sel && sel->functions)
  {
    gui->group_edited = gui->group_selected = sel_pos;
    return sel->functions->mouse_moved(module, pzx, pzy, pressure, which, sel, sel_fpt->parentid, gui, gui->group_edited);
  }

  dt_control_queue_redraw_center();
  return 0;
}

void dt_group_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui)
{
  int pos = 0;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return;
    if(sel->functions)
      sel->functions->post_expose(cr, zoom_scale, gui, pos, g_list_length(sel->points));
    pos++;
  }
}

static void _inverse_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form,
                          float **buffer, int *width, int *height, int *posx, int *posy)
{
  // we create a new buffer
  const int wt = piece->iwidth;
  const int ht = piece->iheight;
  float *buf = dt_alloc_align_float((size_t)ht * wt);

  // we fill this buffer
  for(int yy = 0; yy < MIN(*posy, ht); yy++)
  {
    for(int xx = 0; xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
  }

  for(int yy = MAX(*posy, 0); yy < MIN(ht, (*posy) + (*height)); yy++)
  {
    for(int xx = 0; xx < MIN((*posx), wt); xx++) buf[(size_t)yy * wt + xx] = 1.0f;
    for(int xx = MAX((*posx), 0); xx < MIN(wt, (*posx) + (*width)); xx++)
      buf[(size_t)yy * wt + xx] = 1.0f - (*buffer)[((size_t)yy - (*posy)) * (*width) + xx - (*posx)];
    for(int xx = MAX((*posx) + (*width), 0); xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
  }

  for(int yy = MAX((*posy) + (*height), 0); yy < ht; yy++)
  {
    for(int xx = 0; xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
  }

  // we free the old buffer
  dt_free_align(*buffer);
  (*buffer) = buf;

  // we return correct values for positions;
  *posx = *posy = 0;
  *width = wt;
  *height = ht;
}

static int _group_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const form,
                           float **buffer, int *width, int *height, int *posx, int *posy)
{
  // we allocate buffers and values
  const guint nb = g_list_length(form->points);
  if(nb == 0) return 0;
  float **bufs = calloc(nb, sizeof(float *));
  int *w = malloc(sizeof(int) * nb);
  int *h = malloc(sizeof(int) * nb);
  int *px = malloc(sizeof(int) * nb);
  int *py = malloc(sizeof(int) * nb);
  int *ok = malloc(sizeof(int) * nb);
  int *states = malloc(sizeof(int) * nb);
  float *op = malloc(sizeof(float) * nb);

  // and we get all masks
  int pos = 0;
  int nb_ok = 0;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);
    if(sel)
    {
      ok[pos] = dt_masks_get_mask(module, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]);
      if(fpt->state & DT_MASKS_STATE_INVERSE)
      {
        const double start = dt_get_wtime();
        _inverse_mask(module, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]);
        if(darktable.unmuted & DT_DEBUG_PERF)
          dt_print(DT_DEBUG_MASKS, "[masks %s] inverse took %0.04f sec\n", sel->name, dt_get_wtime() - start);
      }
      op[pos] = fpt->opacity;
      states[pos] = fpt->state;
      if(ok[pos]) nb_ok++;
    }
    pos++;
  }
  if(nb_ok == 0) goto error;

  // now we get the min, max, width, height of the final mask
  int l = INT_MAX, r = INT_MIN, t = INT_MAX, b = INT_MIN;
  for(int i = 0; i < nb; i++)
  {
    l = MIN(l, px[i]);
    t = MIN(t, py[i]);
    r = MAX(r, px[i] + w[i]);
    b = MAX(b, py[i] + h[i]);
  }
  *posx = l;
  *posy = t;
  *width = r - l;
  *height = b - t;

  // we allocate the buffer
  *buffer = dt_alloc_align_float((size_t)(r - l) * (b - t));

  // and we copy each buffer inside, row by row
  for(int i = 0; i < nb; i++)
  {
    const double start = dt_get_wtime();
    if(states[i] & DT_MASKS_STATE_UNION)
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l]
              = fmaxf((*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l], bufs[i][y * w[i] + x] * op[i]);
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_INTERSECTION)
    {
      for(int y = 0; y < b - t; y++)
      {
        for(int x = 0; x < r - l; x++)
        {
          const float b1 = (*buffer)[y * (r - l) + x];
          float b2 = 0.0f;
          if(y + t - py[i] >= 0 && y + t - py[i] < h[i] && x + l - px[i] >= 0 && x + l - px[i] < w[i])
            b2 = bufs[i][(y + t - py[i]) * w[i] + x + l - px[i]];
          if(b1 > 0.0f && b2 > 0.0f)
            (*buffer)[y * (r - l) + x] = fminf(b1, b2 * op[i]);
          else
            (*buffer)[y * (r - l) + x] = 0.0f;
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_DIFFERENCE)
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          const float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          const float b2 = bufs[i][y * w[i] + x] * op[i];
          if(b1 > 0.0f && b2 > 0.0f) (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l] = b1 * (1.0f - b2);
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_EXCLUSION)
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          const float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          const float b2 = bufs[i][y * w[i] + x] * op[i];
          if(b1 > 0.0f && b2 > 0.0f)
            (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l] = fmaxf((1.0f - b1) * b2, b1 * (1.0f - b2));
          else
            (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l]
                = fmaxf((*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l], bufs[i][y * w[i] + x] * op[i]);
        }
      }
    }
    else // if we are here, this mean that we just have to copy the shape and null other parts
    {
      for(int y = 0; y < b - t; y++)
      {
        for(int x = 0; x < r - l; x++)
        {
          float b2 = 0.0f;
          if(y + t - py[i] >= 0 && y + t - py[i] < h[i] && x + l - px[i] >= 0 && x + l - px[i] < w[i])
            b2 = bufs[i][(y + t - py[i]) * w[i] + x + l - px[i]];
          (*buffer)[y * (r - l) + x] = b2 * op[i];
        }
      }
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", i, dt_get_wtime() - start);
  }

  free(op);
  free(states);
  free(ok);
  free(py);
  free(px);
  free(h);
  free(w);
  for(int i = 0; i < nb; i++) dt_free_align(bufs[i]);
  free(bufs);
  return 1;

error:
  free(op);
  free(states);
  free(ok);
  free(py);
  free(px);
  free(h);
  free(w);
  for(int i = 0; i < nb; i++) dt_free_align(bufs[i]);
  free(bufs);
  return 0;
}

static void _combine_masks_union(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                 const float opacity, const int inverted)
{
  if(inverted)
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MAX(dest[index], mask);
    }
  }
  else
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MAX(dest[index], mask);
    }
  }
}

static void _combine_masks_intersect(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                     const float opacity, const int inverted)
{
  if(inverted)
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
  else
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline int both_positive(const float val1, const float val2)
{
  // this needs to be a separate inline function to convince the compiler to vectorize
  return (val1 > 0.0f) && (val2 > 0.0f);
}

static void _combine_masks_difference(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                      const float opacity, const int inverted)
{
  if(inverted)
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] *= (1.0f - mask * both_positive(dest[index],mask));
    }
  }
  else
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] *= (1.0f - mask * both_positive(dest[index],mask));
    }
  }
}

static void _combine_masks_exclusion(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                     const float opacity, const int inverted)
{
  if(inverted)
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      const float pos = both_positive(dest[index], mask);
      const float neg = (1.0f - pos);
      const float b1 = dest[index];
      dest[index] = pos * MAX((1.0f - b1) * mask, b1 * (1.0f - mask)) + neg * MAX(b1, mask);
    }
  }
  else
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      const float pos = both_positive(dest[index], mask);
      const float neg = (1.0f - pos);
      const float b1 = dest[index];
      dest[index] = pos * MAX((1.0f - b1) * mask, b1 * (1.0f - mask)) + neg * MAX(b1, mask);
    }
  }
}

static int _group_get_mask_roi(const dt_iop_module_t *const restrict module,
                               const dt_dev_pixelpipe_iop_t *const restrict piece,
                               dt_masks_form_t *const form, const dt_iop_roi_t *const roi,
                               float *const restrict buffer)
{
  double start = dt_get_wtime();
  if(!form->points) return 0;
  int nb_ok = 0;

  const int width = roi->width;
  const int height = roi->height;
  const size_t npixels = (size_t)width * height;

  // we need to allocate a zeroed temporary buffer for intermediate creation of individual shapes
  float *const restrict bufs = dt_alloc_align_float(npixels);
  if(bufs == NULL) return 0;

  // and we get all masks
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);

    if(sel)
    {
      // ensure that we start with a zeroed buffer regardless of what was previously written into 'bufs'
      memset(bufs, 0, npixels*sizeof(float));
      const int ok = dt_masks_get_mask_roi(module, piece, sel, roi, bufs);
      const float op = fpt->opacity;
      const int state = fpt->state;

      if(ok)
      {
        // first see if we need to invert this shape
        const int inverted = (state & DT_MASKS_STATE_INVERSE);

        if(state & DT_MASKS_STATE_UNION)
        {
          _combine_masks_union(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_INTERSECTION)
        {
          _combine_masks_intersect(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_DIFFERENCE)
        {
          _combine_masks_difference(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_EXCLUSION)
        {
          _combine_masks_exclusion(buffer, bufs, npixels, op, inverted);
        }
        else // if we are here, this mean that we just have to copy the shape and null other parts
        {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
          dt_omp_firstprivate(npixels, op, inverted) \
          dt_omp_sharedconst(buffer, bufs) schedule(simd:static) aligned(buffer, bufs : 64)
#else
#pragma omp parallel for shared(bufs, buffer)
#endif
#endif
          for(int index = 0; index < npixels; index++)
          {
            buffer[index] = op * (inverted ? (1.0f - bufs[index]) : bufs[index]);
          }
        }

        if(darktable.unmuted & DT_DEBUG_PERF)
          dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", nb_ok, dt_get_wtime() - start);
        start = dt_get_wtime();

        nb_ok++;
      }
    }
  }
  // and we free the intermediate buffer
  dt_free_align(bufs);

  return nb_ok != 0;
}

int dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              const dt_iop_roi_t *roi, float *buffer)
{
  const double start = dt_get_wtime();
  if(!form) return 0;

  const int ok = dt_masks_get_mask_roi(module, piece, form, roi, buffer);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] render all masks took %0.04f sec\n", dt_get_wtime() - start);
  return ok;
}

static GSList *_group_setup_mouse_actions(const struct dt_masks_form_t *const form)
{
  GSList *lm = NULL;
  // initialize the mask of seen shapes to the set of flags which aren't actually shapes
  dt_masks_type_t seen_types = (DT_MASKS_GROUP | DT_MASKS_CLONE | DT_MASKS_NON_CLONE);
  // iterate over the shapes in the group, adding the mouse_action for each distinct type of shape
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel || (sel->type & ~seen_types) == 0)
      continue;
    if(sel->functions && sel->functions->setup_mouse_actions)
    {
      GSList *new_actions = sel->functions->setup_mouse_actions(sel);
      lm = g_slist_concat(lm, new_actions);
      seen_types |= sel->type;
    }
  }
  return lm;
}

static void _group_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base,
                                    dt_masks_form_t *const dest)
{
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
    dt_masks_point_group_t *npt = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));

    npt->formid = dt_masks_form_duplicate(dev, pt->formid);
    npt->parentid = dest->formid;
    npt->state = pt->state;
    npt->opacity = pt->opacity;
    dest->points = g_list_append(dest->points, npt);
  }
}

// The function table for groups.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_group = {
  .point_struct_size = sizeof(struct dt_masks_point_group_t),
  .sanitize_config = NULL,
  .setup_mouse_actions = _group_setup_mouse_actions,
  .set_form_name = NULL,
  .set_hint_message = NULL,
  .duplicate_points = _group_duplicate_points,
  .initial_source_pos = NULL,
  .get_distance = NULL,
  .get_points = NULL,
  .get_points_border = NULL,
  .get_mask = _group_get_mask,
  .get_mask_roi = _group_get_mask_roi,
  .get_area = NULL,
  .get_source_area = NULL,
  .mouse_moved = _group_events_mouse_moved,
  .mouse_scrolled = _group_events_mouse_scrolled,
  .button_pressed = _group_events_button_pressed,
  .button_released = _group_events_button_released,
//TODO:  .post_expose = _group_events_post_expose
};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


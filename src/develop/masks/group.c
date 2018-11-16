/*
    This file is part of darktable,
    copyright (c) 2012 aldric renaudin.

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

static int dt_group_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                          uint32_t state, dt_masks_form_t *form, dt_masks_form_gui_t *gui)
{
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return 0;
    if(sel->type & DT_MASKS_CIRCLE)
      return dt_circle_events_mouse_scrolled(module, pzx, pzy, up, state, sel, fpt->parentid, gui,
                                             gui->group_edited);
    else if(sel->type & DT_MASKS_PATH)
      return dt_path_events_mouse_scrolled(module, pzx, pzy, up, state, sel, fpt->parentid, gui,
                                           gui->group_edited);
    else if(sel->type & DT_MASKS_GRADIENT)
      return dt_gradient_events_mouse_scrolled(module, pzx, pzy, up, state, sel, fpt->parentid, gui,
                                               gui->group_edited);
    else if(sel->type & DT_MASKS_ELLIPSE)
      return dt_ellipse_events_mouse_scrolled(module, pzx, pzy, up, state, sel, fpt->parentid, gui,
                                              gui->group_edited);
    else if(sel->type & DT_MASKS_BRUSH)
      return dt_brush_events_mouse_scrolled(module, pzx, pzy, up, state, sel, fpt->parentid, gui,
                                            gui->group_edited);
  }
  return 0;
}

static int dt_group_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                          double pressure, int which, int type, uint32_t state,
                                          dt_masks_form_t *form, dt_masks_form_gui_t *gui)
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
    if(sel->type & DT_MASKS_CIRCLE)
      return dt_circle_events_button_pressed(module, pzx, pzy, pressure, which, type, state, sel,
                                             fpt->parentid, gui, gui->group_edited);
    else if(sel->type & DT_MASKS_PATH)
      return dt_path_events_button_pressed(module, pzx, pzy, pressure, which, type, state, sel, fpt->parentid,
                                           gui, gui->group_edited);
    else if(sel->type & DT_MASKS_GRADIENT)
      return dt_gradient_events_button_pressed(module, pzx, pzy, pressure, which, type, state, sel,
                                               fpt->parentid, gui, gui->group_edited);
    else if(sel->type & DT_MASKS_ELLIPSE)
      return dt_ellipse_events_button_pressed(module, pzx, pzy, pressure, which, type, state, sel,
                                              fpt->parentid, gui, gui->group_edited);
    else if(sel->type & DT_MASKS_BRUSH)
      return dt_brush_events_button_pressed(module, pzx, pzy, pressure, which, type, state, sel,
                                            fpt->parentid, gui, gui->group_edited);
  }
  return 0;
}

static int dt_group_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                           uint32_t state, dt_masks_form_t *form, dt_masks_form_gui_t *gui)
{
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return 0;
    if(sel->type & DT_MASKS_CIRCLE)
      return dt_circle_events_button_released(module, pzx, pzy, which, state, sel, fpt->parentid, gui,
                                              gui->group_edited);
    else if(sel->type & DT_MASKS_PATH)
      return dt_path_events_button_released(module, pzx, pzy, which, state, sel, fpt->parentid, gui,
                                            gui->group_edited);
    else if(sel->type & DT_MASKS_GRADIENT)
      return dt_gradient_events_button_released(module, pzx, pzy, which, state, sel, fpt->parentid, gui,
                                                gui->group_edited);
    else if(sel->type & DT_MASKS_ELLIPSE)
      return dt_ellipse_events_button_released(module, pzx, pzy, which, state, sel, fpt->parentid, gui,
                                               gui->group_edited);
    else if(sel->type & DT_MASKS_BRUSH)
      return dt_brush_events_button_released(module, pzx, pzy, which, state, sel, fpt->parentid, gui,
                                             gui->group_edited);
  }
  return 0;
}

static int dt_group_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                       int which, dt_masks_form_t *form, dt_masks_form_gui_t *gui)
{
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
  float as = 0.005f / zoom_scale * darktable.develop->preview_pipe->backbuf_width;

  // we first don't do anything if we are inside a scrolling session

  if(gui->scrollx != 0.0f && gui->scrolly != 0.0f)
  {
    float as2 = 0.015f / zoom_scale;
    if((gui->scrollx - pzx < as2 && gui->scrollx - pzx > -as2)
       && (gui->scrolly - pzy < as2 && gui->scrolly - pzy > -as2))
      return 1;
    gui->scrollx = gui->scrolly = 0.0f;
  }

  // if a form is in edit mode, we first execute the corresponding event
  if(gui->group_edited >= 0)
  {
    // we get the form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return 0;
    int rep = 0;
    if(sel->type & DT_MASKS_CIRCLE)
      rep = dt_circle_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui,
                                         gui->group_edited);
    else if(sel->type & DT_MASKS_PATH)
      rep = dt_path_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui,
                                       gui->group_edited);
    else if(sel->type & DT_MASKS_GRADIENT)
      rep = dt_gradient_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui,
                                           gui->group_edited);
    else if(sel->type & DT_MASKS_ELLIPSE)
      rep = dt_ellipse_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui,
                                          gui->group_edited);
    else if(sel->type & DT_MASKS_BRUSH)
      rep = dt_brush_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui,
                                        gui->group_edited);
    if(rep) return 1;
    // if a point is in state editing, then we don't want that another form can be selected
    if(gui->point_edited >= 0) return 0;
  }

  // now we check if we are near a form
  GList *fpts = g_list_first(form->points);
  int pos = 0;
  gui->form_selected = gui->border_selected = FALSE;
  gui->source_selected = gui->source_dragging = FALSE;
  gui->pivot_selected = FALSE;
  gui->feather_selected = -1;
  gui->point_edited = gui->point_selected = -1;
  gui->seg_selected = -1;
  gui->point_border_selected = -1;
  gui->group_edited = gui->group_selected = -1;
  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    int inside, inside_border, near, inside_source;
    inside = inside_border = inside_source = 0;
    near = -1;
    float xx = pzx * darktable.develop->preview_pipe->backbuf_width,
          yy = pzy * darktable.develop->preview_pipe->backbuf_height;
    if(sel->type & DT_MASKS_CIRCLE)
      dt_circle_get_distance(xx, yy, as, gui, pos, &inside, &inside_border, &near, &inside_source);
    else if(sel->type & DT_MASKS_PATH)
      dt_path_get_distance(xx, yy, as, gui, pos, g_list_length(sel->points), &inside, &inside_border, &near,
                           &inside_source);
    else if(sel->type & DT_MASKS_GRADIENT)
      dt_gradient_get_distance(xx, yy, as, gui, pos, &inside, &inside_border, &near, &inside_source);
    else if(sel->type & DT_MASKS_ELLIPSE)
      dt_ellipse_get_distance(xx, yy, as, gui, pos, &inside, &inside_border, &near, &inside_source);
    else if(sel->type & DT_MASKS_BRUSH)
      dt_brush_get_distance(xx, yy, as, gui, pos, g_list_length(sel->points), &inside, &inside_border, &near,
                            &inside_source);
    if(inside || inside_border || near >= 0 || inside_source)
    {
      gui->group_edited = gui->group_selected = pos;
      if(sel->type & DT_MASKS_CIRCLE)
        return dt_circle_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui, pos);
      else if(sel->type & DT_MASKS_PATH)
        return dt_path_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui, pos);
      else if(sel->type & DT_MASKS_GRADIENT)
        return dt_gradient_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui, pos);
      else if(sel->type & DT_MASKS_ELLIPSE)
        return dt_ellipse_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui, pos);
      else if(sel->type & DT_MASKS_BRUSH)
        return dt_brush_events_mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui, pos);
    }
    fpts = g_list_next(fpts);
    pos++;
  }
  dt_control_queue_redraw_center();
  return 0;
}

static void dt_group_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_t *form,
                                        dt_masks_form_gui_t *gui)
{
  GList *fpts = g_list_first(form->points);
  int pos = 0;
  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if (!sel) return;
    if(sel->type & DT_MASKS_CIRCLE)
      dt_circle_events_post_expose(cr, zoom_scale, gui, pos);
    else if(sel->type & DT_MASKS_PATH)
      dt_path_events_post_expose(cr, zoom_scale, gui, pos, g_list_length(sel->points));
    else if(sel->type & DT_MASKS_GRADIENT)
      dt_gradient_events_post_expose(cr, zoom_scale, gui, pos);
    else if(sel->type & DT_MASKS_ELLIPSE)
      dt_ellipse_events_post_expose(cr, zoom_scale, gui, pos);
    else if(sel->type & DT_MASKS_BRUSH)
      dt_brush_events_post_expose(cr, zoom_scale, gui, pos, g_list_length(sel->points));
    fpts = g_list_next(fpts);
    pos++;
  }
}

static void _inverse_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                          float **buffer, int *width, int *height, int *posx, int *posy)
{
  // we create a new buffer
  int wt = piece->iwidth;
  int ht = piece->iheight;
  float *buf = malloc((size_t)ht * wt * sizeof(float));

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
  free(*buffer);
  (*buffer) = buf;

  // we return correct values for positions;
  *posx = *posy = 0;
  *width = wt;
  *height = ht;
}

static int dt_group_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2;
  // we allocate buffers and values
  const guint nb = g_list_length(form->points);
  if(nb == 0) return 0;
  float **bufs = calloc(nb, sizeof(float *));
  int *w = malloc(nb * sizeof(int));
  int *h = malloc(nb * sizeof(int));
  int *px = malloc(nb * sizeof(int));
  int *py = malloc(nb * sizeof(int));
  int *ok = malloc(nb * sizeof(int));
  int *states = malloc(nb * sizeof(int));
  float *op = malloc(nb * sizeof(float));

  // and we get all masks
  GList *fpts = g_list_first(form->points);
  int pos = 0;
  int nb_ok = 0;
  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);
    if(sel)
    {
      ok[pos] = dt_masks_get_mask(module, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]);
      if(fpt->state & DT_MASKS_STATE_INVERSE)
      {
        start2 = dt_get_wtime();
        _inverse_mask(module, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]);
        if(darktable.unmuted & DT_DEBUG_PERF)
          dt_print(DT_DEBUG_MASKS, "[masks %s] inverse took %0.04f sec\n", sel->name, dt_get_wtime() - start2);
//         start2 = dt_get_wtime();
      }
      op[pos] = fpt->opacity;
      states[pos] = fpt->state;
      if(ok[pos]) nb_ok++;
    }
    fpts = g_list_next(fpts);
    pos++;
  }
  if(nb_ok == 0) goto error;

  // now we get the min, max, width, height of the final mask
  int l, r, t, b;
  l = t = INT_MAX;
  r = b = INT_MIN;
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
  *buffer = malloc(sizeof(float) * (r - l) * (b - t));

  // and we copy each buffer inside, row by row
  for(int i = 0; i < nb; i++)
  {
    start2 = dt_get_wtime();
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
          float b1 = (*buffer)[y * (r - l) + x];
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
          float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          float b2 = bufs[i][y * w[i] + x] * op[i];
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
          float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          float b2 = bufs[i][y * w[i] + x] * op[i];
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
      dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", i, dt_get_wtime() - start2);
//     start2 = dt_get_wtime();
  }

  free(op);
  free(states);
  free(ok);
  free(py);
  free(px);
  free(h);
  free(w);
  for(int i = 0; i < nb; i++) free(bufs[i]);
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
  for(int i = 0; i < nb; i++) free(bufs[i]);
  free(bufs);
  return 0;
}

int dt_masks_group_render(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                          float **buffer, int *roi, float scale)
{
  double start2 = dt_get_wtime();

  if(!form) return 0;
  float *mask = *buffer;
  // we first reset the buffer to 0
  memset(mask, 0, roi[2] * roi[3] * sizeof(float));

  // we get the mask
  float *fm = NULL;
  int fx = roi[0], fy = roi[1], fw = roi[2], fh = roi[3];
  if(!dt_masks_get_mask(module, piece, form, &fm, &fw, &fh, &fx, &fy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] get all masks took %0.04f sec\n", dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we don't want row which are outside the roi_out
  int fxx = fx * scale;
  int fww = fw * scale;
  int fyy = fy * scale;
  int fhh = fh * scale;
  if(fxx > roi[0] + roi[2])
  {
    free(fm);
    return 1;
  }

  if(fxx < roi[0]) fww += fxx - roi[0], fxx = roi[0];
  if(fww + fxx >= roi[0] + roi[2]) fww = roi[0] + roi[2] - fxx - 1;

  // we adjust to avoid rounding errors
  if(fyy / scale - fy < 0) fyy++, fhh--;
  if(fxx / scale - fx < 0) fxx++, fww--;
  if((fyy + fhh) / scale - fy >= fh) fhh--;
  if((fxx + fww) / scale - fx >= fw) fww--;

  // we apply the mask row by row
  for(int yy = fyy; yy < fyy + fhh; yy++)
  {
    if(yy < roi[1] || yy >= roi[1] + roi[3]) continue;
    for(int xx = fxx; xx < fxx + fww; xx++)
    {
      int a = (yy / scale - fy);
      int b = (xx / scale);
      mask[(yy - roi[1]) * roi[2] + xx - roi[0]]
          = fmaxf(mask[(yy - roi[1]) * roi[2] + xx - roi[0]], fm[a * fw + b - fx]);
    }
  }

  // we free the mask
  free(fm);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] scale all masks took %0.04f sec\n", dt_get_wtime() - start2);

  return 1;
}

static int dt_group_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                 dt_masks_form_t *form, const dt_iop_roi_t *roi, float *buffer)
{
  double start2 = dt_get_wtime();
  const guint nb = g_list_length(form->points);
  if(nb == 0) return 0;
  int nb_ok = 0;

  const int width = roi->width;
  const int height = roi->height;

  // we need to allocate a temporary buffer for intermediate creation of individual shapes
  float *bufs = dt_alloc_align(64, (size_t)width * height * sizeof(float));
  if(bufs == NULL) return 0;

  // empty the output buffer
  memset(buffer, 0, (size_t)width * height * sizeof(float));

  // and we get all masks
  GList *fpts = g_list_first(form->points);

  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);

    if(sel)
    {
      const int ok = dt_masks_get_mask_roi(module, piece, sel, roi, bufs);
      const float op = fpt->opacity;
      const int state = fpt->state;

      if(ok)
      {
        // first see if we need to invert this shape
        if(state & DT_MASKS_STATE_INVERSE)
        {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(bufs)
#else
#pragma omp parallel for shared(bufs)
#endif
#endif
          for(int y = 0; y < height; y++)
            for(int x = 0; x < width; x++)
            {
              size_t index = (size_t)y * width + x;
              bufs[index] = 1.0f - bufs[index];
            }
        }

        if(state & DT_MASKS_STATE_UNION)
        {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(bufs, buffer)
#else
#pragma omp parallel for shared(bufs, buffer)
#endif
#endif
          for(int y = 0; y < height; y++)
            for(int x = 0; x < width; x++)
            {
              size_t index = (size_t)y * width + x;
              buffer[index] = fmaxf(buffer[index], bufs[index] * op);
            }
        }
        else if(state & DT_MASKS_STATE_INTERSECTION)
        {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(bufs, buffer)
#else
#pragma omp parallel for shared(bufs, buffer)
#endif
#endif
          for(int y = 0; y < height; y++)
            for(int x = 0; x < width; x++)
            {
              size_t index = (size_t)y * width + x;
              float b1 = buffer[index];
              float b2 = b2 = bufs[index]; // FIXME: is this line correct? what it supposed to be doing?
              if(b1 > 0.0f && b2 > 0.0f)
                buffer[index] = fminf(b1, b2 * op);
              else
                buffer[index] = 0.0f;
            }
        }
        else if(state & DT_MASKS_STATE_DIFFERENCE)
        {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(bufs, buffer)
#else
#pragma omp parallel for shared(bufs, buffer)
#endif
#endif
          for(int y = 0; y < height; y++)
            for(int x = 0; x < width; x++)
            {
              size_t index = (size_t)y * width + x;
              float b1 = buffer[index];
              float b2 = bufs[index] * op;
              if(b1 > 0.0f && b2 > 0.0f) buffer[index] = b1 * (1.0f - b2);
            }
        }
        else if(state & DT_MASKS_STATE_EXCLUSION)
        {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(bufs, buffer)
#else
#pragma omp parallel for shared(bufs, buffer)
#endif
#endif
          for(int y = 0; y < height; y++)
            for(int x = 0; x < width; x++)
            {
              size_t index = (size_t)y * width + x;
              float b1 = buffer[index];
              float b2 = bufs[index] * op;
              if(b1 > 0.0f && b2 > 0.0f)
                buffer[index] = fmaxf((1.0f - b1) * b2, b1 * (1.0f - b2));
              else
                buffer[index] = fmaxf(b1, b2);
            }
        }
        else // if we are here, this mean that we just have to copy the shape and null other parts
        {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) shared(bufs, buffer)
#else
#pragma omp parallel for shared(bufs, buffer)
#endif
#endif
          for(int y = 0; y < height; y++)
            for(int x = 0; x < width; x++)
            {
              size_t index = (size_t)y * width + x;
              buffer[index] = bufs[index] * op;
            }
        }

        if(darktable.unmuted & DT_DEBUG_PERF)
          dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", nb_ok, dt_get_wtime() - start2);
        start2 = dt_get_wtime();

        nb_ok++;
      }
    }
    fpts = g_list_next(fpts);
  }

  // and we free the intermediate buffer
  dt_free_align(bufs);

  return (nb_ok != 0);
}

int dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              const dt_iop_roi_t *roi, float *buffer)
{
  double start2 = dt_get_wtime();
  if(!form) return 0;

  int ok = dt_masks_get_mask_roi(module, piece, form, roi, buffer);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] render all masks took %0.04f sec\n", dt_get_wtime() - start2);
  return ok;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

#include "scopes.h"

typedef struct dt_scopes_split_t
{
  dt_scopes_mode_t *left;
  dt_scopes_mode_t *right;
} dt_scopes_split_t;

const char* _split_name(const dt_scopes_mode_t *const self)
{
  // FIXME: this could depend on values of left/right scopes
  return N_("split");
}

static void _split_process(dt_scopes_mode_t *const self,
                          const float *const input,
                          dt_histogram_roi_t *const roi,
                          const dt_iop_order_iccprofile_info_t *vs_prof)
{
  dt_scopes_split_t *const d = self->data;
  dt_scopes_call(d->left, process, input, roi, vs_prof);
  dt_scopes_call(d->right, process, input, roi, vs_prof);
  self->update_counter = self->scopes->update_counter;
}

static void _split_draw_highlight(const dt_scopes_mode_t *const self,
                                  cairo_t *cr,
                                  dt_scopes_highlight_t highlight,
                                  const int width,
                                  const int height)
{
  const dt_scopes_split_t *const d = self->data;
  const int half_width = width / 2;
  cairo_save(cr);
  dt_scopes_call_if_exists(d->left, draw_highlight, cr, self->scopes->highlight,
                           half_width, height);
  cairo_translate(cr, half_width, 0);
  dt_scopes_call_if_exists(d->right, draw_highlight, cr, self->scopes->highlight,
                           half_width, height);
  cairo_restore(cr);
}

static void _split_draw_bkgd(const dt_scopes_mode_t *const self,
                             cairo_t *cr,
                             const int width,
                             const int height)
{
  const dt_scopes_split_t *const d = self->data;
  const int half_width = width / 2;
  cairo_save(cr);
  dt_scopes_call_if_exists(d->left, draw_bkgd, cr, half_width, height);
  cairo_translate(cr, half_width, 0);
  dt_scopes_call_if_exists(d->right, draw_bkgd, cr, half_width, height);
  cairo_restore(cr);
}

static void _split_draw_grid(const dt_scopes_mode_t *const self,
                             cairo_t *cr,
                             const int width,
                             const int height)
{
  const dt_scopes_split_t *const d = self->data;
  const int half_width = width / 2;
  cairo_save(cr);
  dt_scopes_call_if_exists(d->left, draw_grid, cr, half_width, height);
  cairo_translate(cr, half_width, 0);
  dt_scopes_call_if_exists(d->right, draw_grid, cr, half_width, height);
  cairo_restore(cr);
}

static void _split_draw(const dt_scopes_mode_t *const self,
                        cairo_t *cr,
                        const int width,
                        const int height,
                        const scopes_channels_t channels)
{
  const dt_scopes_split_t *const d = self->data;
  const int half_width = width / 2;

  if(d->left->update_counter == self->scopes->update_counter)
  {
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, half_width, height);
    cairo_clip(cr);
    if(d->left->functions->draw_scope_channels)
      dt_scopes_call(d->left, draw_scope_channels,
                     cr, half_width, height, self->scopes->channels);
    else
      dt_scopes_call(d->left, draw_scope, cr, half_width, height);
    cairo_restore(cr);
  }
  // FIXME: make slight gap/line between two scopes, as it is crammed, especially visibile in RYB vectorscope where color harmony name is awkwardly close to waveform
  if(d->right->update_counter == self->scopes->update_counter)
  {
    cairo_save(cr);
    cairo_translate(cr, half_width, 0);
    cairo_rectangle(cr, 0, 0, half_width, height);
    cairo_clip(cr);
    if(d->right->functions->draw_scope_channels)
      dt_scopes_call(d->right, draw_scope_channels,
                     cr, half_width, height, self->scopes->channels);
    else
      dt_scopes_call(d->right, draw_scope, cr, half_width, height);
    cairo_restore(cr);
  }
}

static void _split_append_to_tooltip(const dt_scopes_mode_t *const self,
                                     gchar **tip)
{
  const dt_scopes_split_t *const d = self->data;
  dt_scopes_call_if_exists(d->left, append_to_tooltip, tip);
  dt_scopes_call_if_exists(d->right, append_to_tooltip, tip);
}

static double _split_get_exposure_pos(const dt_scopes_mode_t *const self,
                                      const double x,
                                      const double y)
{
  const dt_scopes_split_t *const d = self->data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(self->scopes->scope_draw, &allocation);
  if(x < allocation.width/2.0)
  {
    if(d->left->functions->get_exposure_pos)
      return dt_scopes_call(d->left, get_exposure_pos, x*2.0, y);
    else
      dt_print(DT_DEBUG_ALWAYS,
               "[_split_get_exposure_pos] no %s (left) get_exposure_pos at %f, %f",
               dt_scopes_call(d->left, name), x, y);
  }
  else
  {
    if(d->right->functions->get_exposure_pos)
      return dt_scopes_call(d->right, get_exposure_pos, x*2.0-allocation.width, y);
    else
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[_split_get_exposure_pos] no %s (right) get_exposure_pos at %f, %f",
               dt_scopes_call(d->right, name), x, y);
      // hack to handle user starting drag in left side and to right side
      // FIXME: making each side of split in own widget will handle this
      if(d->left->functions->get_exposure_pos)
        return dt_scopes_call(d->left, get_exposure_pos, x*2.0, y);
    }
  }
  return x;
}

static dt_scopes_highlight_t _split_get_highlight(const dt_scopes_mode_t *const self,
                                                  const double posx,
                                                  const double posy)
{
  const dt_scopes_split_t *const d = self->data;
  if((posx < 0.5) && d->left->functions->get_highlight)
    return dt_scopes_call(d->left, get_highlight, posx*2.0, posy);
  if((posx >= 0.5) && d->right->functions->get_highlight)
    return dt_scopes_call(d->right, get_highlight, posx*2.0-1.0, posy);
  return DT_SCOPES_HIGHLIGHT_NONE;
}

static void _split_clear(dt_scopes_mode_t *const self)
{
  // FIXME: make sure this is actually called appropriately in libs/histogram.c
  dt_scopes_split_t *const d = self->data;
  dt_scopes_call(d->left, clear);
  dt_scopes_call(d->right, clear);
}

static void _split_eventbox_scroll(dt_scopes_mode_t *const self,
                                   gdouble x, gdouble y,
                                   gdouble delta_x, gdouble delta_y,
                                   GdkModifierType state)
{
  dt_scopes_split_t *const d = self->data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(self->scopes->scope_draw, &allocation);
  if(x < allocation.width/2)
    dt_scopes_call_if_exists(d->left, eventbox_scroll, x, y, delta_x, delta_y, state);
  if(x >= allocation.width/2)
    dt_scopes_call_if_exists(d->right, eventbox_scroll, x, y, delta_x, delta_y, state);
}

static void _split_update_buttons(const dt_scopes_mode_t *const self)
{
  dt_scopes_split_t *d = self->data;
  dt_scopes_call(d->left, update_buttons);
  dt_scopes_call(d->right, update_buttons);
}

static void _split_mode_enter(dt_scopes_mode_t *const self)
{
  const dt_scopes_split_t *const d = self->data;
  dt_scopes_call(d->left, mode_enter);
  dt_scopes_call(d->right, mode_enter);
  self->update_counter = MAX(d->left->update_counter, d->right->update_counter);
  if(d->left->update_counter != d->right->update_counter)
    dt_scopes_reprocess();
}

static void _split_mode_leave(const dt_scopes_mode_t *const self)
{
  const dt_scopes_split_t *const d = self->data;
  dt_scopes_call(d->left, mode_leave);
  dt_scopes_call(d->right, mode_leave);
}

static void _split_gui_init(dt_scopes_mode_t *const self,
                           dt_scopes_t *const scopes)
{
  dt_scopes_split_t *d = dt_calloc1_align_type(dt_scopes_split_t);
  self->data = (void *)d;
  d->left = &self->scopes->modes[DT_SCOPES_MODE_WAVEFORM];
  d->right = &self->scopes->modes[DT_SCOPES_MODE_VECTORSCOPE];
}

static void _split_gui_cleanup(dt_scopes_mode_t *const self)
{
  dt_free_align(self->data);
  self->data = NULL;
}

// The function table for split mode. This must be public, i.e. no "static" keyword.
const dt_scopes_functions_t dt_scopes_functions_split = {
  .name = _split_name,
  .process = _split_process,
  .clear = _split_clear,
  .draw_grid = _split_draw_grid,
  .draw_bkgd = _split_draw_bkgd,
  .draw_highlight = _split_draw_highlight,
  .draw_scope = NULL,
  .draw_scope_channels = _split_draw,
  .get_highlight = _split_get_highlight,
  .get_exposure_pos = _split_get_exposure_pos,
  .append_to_tooltip = _split_append_to_tooltip,
  .eventbox_scroll = _split_eventbox_scroll,
  .update_buttons = _split_update_buttons,
  .mode_enter = _split_mode_enter,
  .mode_leave = _split_mode_leave,
  .gui_init = _split_gui_init,
  .add_options = NULL,
  .gui_cleanup = _split_gui_cleanup
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

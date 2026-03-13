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
  // point back to parent
  // FIXME: is this healthy? is this needed?
  dt_scopes_t *scopes;
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
  d->left->functions->process(d->left, input, roi, vs_prof);
  d->right->functions->process(d->right, input, roi, vs_prof);
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
  if(d->left->functions->draw_highlight)
    d->left->functions->draw_highlight(d->left, cr, d->scopes->highlight,
                                       half_width, height);
  cairo_translate(cr, half_width, 0);
  if(d->right->functions->draw_highlight)
    d->right->functions->draw_highlight(d->right, cr, d->scopes->highlight,
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
  if(d->left->functions->draw_bkgd)
    d->left->functions->draw_bkgd(d->left, cr, half_width, height);
  cairo_translate(cr, half_width, 0);
  if(d->right->functions->draw_bkgd)
    d->right->functions->draw_bkgd(d->right, cr, half_width, height);
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
  if(d->left->functions->draw_grid)
    d->left->functions->draw_grid(d->left, cr, half_width, height);
  cairo_translate(cr, half_width, 0);
  if(d->right->functions->draw_grid)
    d->right->functions->draw_grid(d->right, cr, half_width, height);
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

  if(d->left->update_counter == d->scopes->update_counter)
  {
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, half_width, height);
    cairo_clip(cr);
    if(d->left->functions->draw_scope_channels)
      d->left->functions->draw_scope_channels(d->left, cr,
                                              half_width, height,
                                              d->scopes->channels);
    else
      d->left->functions->draw_scope(d->left, cr,
                                     half_width, height);
    cairo_restore(cr);
  }
  if(d->right->update_counter == d->scopes->update_counter)
  {
    cairo_save(cr);
    cairo_translate(cr, half_width, 0);
    cairo_rectangle(cr, 0, 0, half_width, height);
    cairo_clip(cr);
    if(d->right->functions->draw_scope_channels)
      d->right->functions->draw_scope_channels(d->right, cr,
                                               half_width, height,
                                               d->scopes->channels);
    else
      d->right->functions->draw_scope(d->right, cr,
                                      half_width, height);
    cairo_restore(cr);
  }
}

static void _split_append_to_tooltip(const dt_scopes_mode_t *const self,
                                     gchar **tip)
{
  const dt_scopes_split_t *const d = self->data;
  if(d->left->functions->append_to_tooltip)
    d->left->functions->append_to_tooltip(d->left, tip);
  if(d->right->functions->append_to_tooltip)
    d->right->functions->append_to_tooltip(d->right, tip);
}

static double _split_get_exposure_pos(const dt_scopes_mode_t *const self,
                                      const double x,
                                      const double y)
{
  const dt_scopes_split_t *const d = self->data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(d->scopes->scope_draw, &allocation);
  if(x < allocation.width/2.0)
  {
    if(d->left->functions->get_exposure_pos)
      return d->left->functions->get_exposure_pos(d->left, x*2.0, y);
    else
      dt_print(DT_DEBUG_ALWAYS,
               "[_split_get_exposure_pos] no %s (left) get_exposure_pos at %f, %f\n",
               d->left->functions->name(d->left), x, y);
  }
  else
  {
    if(d->right->functions->get_exposure_pos)
      return d->right->functions->get_exposure_pos(d->right, x*2.0-allocation.width, y);
    else
      dt_print(DT_DEBUG_ALWAYS,
               "[_split_get_exposure_pos] no %s (right) get_exposure_pos at %f, %f\n",
               d->right->functions->name(d->right), x, y);
  }
  return x;
}

static dt_scopes_highlight_t _split_get_highlight(const dt_scopes_mode_t *const self,
                                                  const double posx,
                                                  const double posy)
{
  const dt_scopes_split_t *const d = self->data;
  if((posx < 0.5) && d->left->functions->get_highlight)
    return d->left->functions->get_highlight(d->left, posx*2.0, posy);
  else if((posx >= 0.5) && d->right->functions->get_highlight)
    return d->right->functions->get_highlight(d->right, posx*2.0-1.0, posy);
  else
    return DT_SCOPES_HIGHLIGHT_NONE;
}

static void _split_clear(dt_scopes_mode_t *const self)
{
  // FIXME: make sure this is actually called appropriately in libs/histogram.c
  dt_scopes_split_t *const d = self->data;
  d->left->functions->clear(d->left);
  d->right->functions->clear(d->right);
}

static void _split_eventbox_scroll(dt_scopes_mode_t *const self,
                                   gdouble x, gdouble y,
                                   gdouble delta_x, gdouble delta_y,
                                   GdkModifierType state)
{
  dt_scopes_split_t *const d = self->data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(d->scopes->scope_draw, &allocation);
  if((x < allocation.width/2) && d->left->functions->eventbox_scroll)
    d->left->functions->eventbox_scroll(d->left, x, y, delta_x, delta_y, state);
  if((x >= allocation.width/2) && d->right->functions->eventbox_scroll)
    d->right->functions->eventbox_scroll(d->right, x, y, delta_x, delta_y, state);
}

static void _split_eventbox_motion(dt_scopes_mode_t *const self,
                                   GtkEventControllerMotion *controller,
                                   double x,
                                   double y)
{
  dt_scopes_split_t *const d = self->data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(d->scopes->scope_draw, &allocation);
  if((x < allocation.width/2) && d->left->functions->eventbox_motion)
    d->left->functions->eventbox_motion(d->left, controller, x, y);
  if((x >= allocation.width/2) && d->right->functions->eventbox_motion)
    d->right->functions->eventbox_motion(d->right, controller, x, y);
}

static void _split_update_buttons(const dt_scopes_mode_t *const self)
{
  dt_scopes_split_t *d = self->data;
  d->left->functions->update_buttons(d->left);
  d->right->functions->update_buttons(d->right);
}

static void _split_mode_enter(dt_scopes_mode_t *const self)
{
  const dt_scopes_split_t *const d = self->data;
  d->left->functions->mode_enter(d->left);
  d->right->functions->mode_enter(d->right);
  self->update_counter = MAX(d->left->update_counter, d->right->update_counter);
  if(d->left->update_counter != d->right->update_counter)
    dt_scopes_reprocess();
}

static void _split_mode_leave(const dt_scopes_mode_t *const self)
{
  const dt_scopes_split_t *const d = self->data;
  d->left->functions->mode_leave(d->left);
  d->right->functions->mode_leave(d->right);
}

static void _split_gui_init(dt_scopes_mode_t *const self,
                           dt_scopes_t *const scopes)
{
  dt_scopes_split_t *d = dt_calloc1_align_type(dt_scopes_split_t);
  self->data = (void *)d;
  // FIXME: ideal this is an attribute of self set up by caller
  d->scopes = scopes;

  d->left = &d->scopes->modes[DT_SCOPES_MODE_VECTORSCOPE];
  d->right = &d->scopes->modes[DT_SCOPES_MODE_WAVEFORM];
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
  .eventbox_motion = _split_eventbox_motion,
  .update_buttons = _split_update_buttons,
  .mode_enter = _split_mode_enter,
  .mode_leave = _split_mode_leave,
  .gui_init = _split_gui_init,
  .add_to_main_box = NULL,
  .add_to_options_box = NULL,
  .gui_cleanup = _split_gui_cleanup
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

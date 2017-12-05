/*
 *    This file is part of darktable,
 *    copyright (c) 2012-2015 tobias ellinghaus.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib.h>

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "gui/guides.h"

typedef struct dt_QRect_t
{
  float left, top, right, bottom, width, height;
} dt_QRect_t;

static void dt_guides_q_rect(dt_QRect_t *R1, float left, float top, float width, float height)
{
  R1->left = left;
  R1->top = top;
  R1->right = left + width;
  R1->bottom = top + height;
  R1->width = width;
  R1->height = height;
}


static void dt_guides_draw_simple_grid(cairo_t *cr, const float x, const float y, const float w,
                                       const float h, float zoom_scale)
{
  float right = x + w;
  float bottom = y + h;
  // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
  cairo_set_line_width(cr, 1.0 / zoom_scale);
  cairo_set_source_rgb(cr, .2, .2, .2);
  dt_draw_grid(cr, 3, x, y, right, bottom);
  cairo_translate(cr, 1.0 / zoom_scale, 1.0 / zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);
  dt_draw_grid(cr, 3, x, y, right, bottom);
  cairo_set_source_rgba(cr, .8, .8, .8, 0.5);
  double dashes = 5.0 / zoom_scale;
  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_grid(cr, 9, x, y, right, bottom);
}


static void dt_guides_draw_diagonal_method(cairo_t *cr, const float x, const float y, const float w, const float h)
{
  if(w > h)
  {
    dt_draw_line(cr, x, y, x + h, y + h);
    dt_draw_line(cr, x, y + h, x + h, y);
    dt_draw_line(cr, x + w - h, y, x + w, y + h);
    dt_draw_line(cr, x + w - h, y + h, x + w, y);
  }
  else
  {
    dt_draw_line(cr, x, y, x + w, y + w);
    dt_draw_line(cr, x, y + w, x + w, y);
    dt_draw_line(cr, x, y + h - w, x + w, y + h);
    dt_draw_line(cr, x, y + h, x + w, y + h - w);
  }
}


static void dt_guides_draw_rules_of_thirds(cairo_t *cr, const float left, const float top,
                                           const float width, const float height)
{
  const float right = left + width, bottom = top + height;
  const float x_3 = width / 3.0, y_3 = height / 3.0;

  dt_draw_line(cr, left + x_3, top, left + x_3, bottom);
  dt_draw_line(cr, left + 2 * x_3, top, left + 2 * x_3, bottom);

  dt_draw_line(cr, left, top + y_3, right, top + y_3);
  dt_draw_line(cr, left, top + 2 * y_3, right, top + 2 * y_3);
}


static void dt_guides_draw_harmonious_triangles(cairo_t *cr, const float left, const float top, const float width,
                                                const float height/*, const float dst*/)
{
  int dst = (int)((height * cos(atan(width / height)) / (cos(atan(height / width)))));

  dt_draw_line(cr, -width / 2, -height / 2, width / 2, height / 2);
  dt_draw_line(cr, -width / 2 + dst, -height / 2, -width / 2, height / 2);
  dt_draw_line(cr, width / 2, -height / 2, width / 2 - dst, height / 2);
}


#define PERSPECTIVE_LINES 16
static void dt_guides_draw_perspective(cairo_t *cr, const float x, const float y, const float w, const float h)
{
  const float rotation_step = 2.0 / PERSPECTIVE_LINES,
              line_length = w * w + h * h; // no need for sqrt or *0.25, this is inside a cairo_clip anyway

  cairo_save(cr);
  for(int i = 0; i < PERSPECTIVE_LINES; i++)
  {
    cairo_save(cr);
    cairo_rotate(cr, -M_PI * rotation_step * i);
    dt_draw_line(cr, 0, 0, line_length, 0);
    cairo_restore(cr);
  }
  cairo_restore(cr);
}
#undef PERSPECTIVE_LINES


#define X_LINES 49
#define Y_LINES 33
#define CROSSES 6
static void dt_guides_draw_metering(cairo_t *cr, const float x, const float y, const float w, const float h)
{
  const float x_step = w / (X_LINES - 1), y_step = h / (Y_LINES - 1), length_short = MIN(w, h) * 0.02,
              length_middle = length_short * 1.5,
              length_long = length_middle * 1.5; // these are effectively * 2!

  cairo_save(cr);
  cairo_translate(cr, x, y);

  // along x axis
  cairo_save(cr);
  cairo_translate(cr, 0, h * 0.5);
  for(int i = 0; i < X_LINES; i++)
    if(i % 4 != 0)
      dt_draw_line(cr, i * x_step, -length_short, i * x_step, length_short); // short lines
    else if(i % 12 != 0)
      dt_draw_line(cr, i * x_step, -length_middle, i * x_step, length_middle); // medium lines
    else if(i != X_LINES / 2)
      dt_draw_line(cr, i * x_step, -length_long, i * x_step, length_long); // long lines
    else
      dt_draw_line(cr, i * x_step, -h * 0.5, i * x_step, h * 0.5); // middle line
  cairo_restore(cr);

  // along y axis
  cairo_save(cr);
  cairo_translate(cr, w * 0.5, 0);
  for(int i = 0; i < Y_LINES; i++)
    if((i - 4) % 4 != 0)
      dt_draw_line(cr, -length_short, i * y_step, length_short, i * y_step); // short lines
    else if(i == Y_LINES / 2)
      dt_draw_line(cr, -w * 0.5, i * y_step, w * 0.5, i * y_step); // middle line
    else if((i - 4) % 12 != 0)
      dt_draw_line(cr, -length_middle, i * y_step, length_middle, i * y_step); // medium lines
    else
      dt_draw_line(cr, -length_long, i * y_step, length_long, i * y_step); // long lines
  cairo_restore(cr);

  // small crosses
  const float length_cross = length_short * .5, cross_x_step = w / CROSSES, cross_y_step = h / CROSSES;
  for(int cx = 1; cx < CROSSES; cx++)
    for(int cy = 1; cy < CROSSES; cy++)
      if(cx != CROSSES / 2 && cy != CROSSES / 2)
      {
        float _x = cx * cross_x_step, _y = cy * cross_y_step;
        dt_draw_line(cr, _x - length_cross, _y, _x + length_cross, _y);
        dt_draw_line(cr, _x, _y - length_cross, _x, _y + length_cross);
      }
  cairo_restore(cr);
}
#undef X_LINES
#undef y_LINES
#undef CROSSES

#define RADIANS(degrees) ((degrees) * (M_PI / 180.))
static void dt_guides_draw_golden_mean(cairo_t *cr, dt_QRect_t *R1, dt_QRect_t *R2, dt_QRect_t *R3, dt_QRect_t *R4,
                                       dt_QRect_t *R5, dt_QRect_t *R6, dt_QRect_t *R7, gboolean goldenSection,
                                       gboolean goldenTriangle, gboolean goldenSpiralSection, gboolean goldenSpiral)
{
  // Drawing Golden sections.
  if(goldenSection)
  {
    // horizontal lines:
    dt_draw_line(cr, R1->left, R2->top, R2->right, R2->top);
    dt_draw_line(cr, R1->left, R1->top + R2->height, R2->right, R1->top + R2->height);

    // vertical lines:
    dt_draw_line(cr, R1->right, R1->top, R1->right, R1->bottom);
    dt_draw_line(cr, R1->left + R2->width, R1->top, R1->left + R2->width, R1->bottom);
  }

  // Drawing Golden triangle guides.
  if(goldenTriangle)
  {
    dt_draw_line(cr, R1->left, R1->bottom, R2->right, R1->top);
    dt_draw_line(cr, R1->left, R1->top, R2->right - R1->width, R1->bottom);
    dt_draw_line(cr, R1->left + R1->width, R1->top, R2->right, R1->bottom);
  }

  // Drawing Golden spiral sections.
  if(goldenSpiralSection)
  {
    dt_draw_line(cr, R1->right, R1->top, R1->right, R1->bottom);
    dt_draw_line(cr, R2->left, R2->top, R2->right, R2->top);
    dt_draw_line(cr, R3->left, R3->top, R3->left, R3->bottom);
    dt_draw_line(cr, R4->left, R4->bottom, R4->right, R4->bottom);
    dt_draw_line(cr, R5->right, R5->top, R5->right, R5->bottom);
    dt_draw_line(cr, R6->left, R6->top, R6->right, R6->top);
    dt_draw_line(cr, R7->left, R7->top, R7->left, R7->bottom);
  }

  // Drawing Golden Spiral.
  if(goldenSpiral)
  {
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R1->width / R1->height, 1);
    cairo_arc(cr, R1->right / R1->width * R1->height, R1->top, R1->height, RADIANS(90), RADIANS(180));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R2->width / R2->height, 1);
    cairo_arc(cr, R2->left / R2->width * R2->height, R2->top, R2->height, RADIANS(0), RADIANS(90));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R3->width / R3->height, 1);
    cairo_arc(cr, R3->left / R3->width * R3->height, R3->bottom, R3->height, RADIANS(270), RADIANS(360));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R4->height / R4->width);
    cairo_arc(cr, R4->right, R4->bottom / R4->height * R4->width, R4->width, RADIANS(180), RADIANS(270));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R5->height / R5->width);
    cairo_arc(cr, R5->right, R5->top / R5->height * R5->width, R5->width, RADIANS(90), RADIANS(180));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R6->height / R6->width);
    cairo_arc(cr, R6->left, R6->top / R6->height * R6->width, R6->width, RADIANS(0), RADIANS(90));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R7->width / R7->height, 1);
    cairo_arc(cr, R7->left / R7->width * R7->height, R7->bottom, R7->height, RADIANS(270), RADIANS(360));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, (R6->width - R7->width) / R7->height, 1);
    cairo_arc(cr, R7->left / (R6->width - R7->width) * R7->height, R7->bottom, R7->height, RADIANS(210),
              RADIANS(270));
    cairo_restore(cr);
  }
}
#undef RADIANS


///////// wrappers for the guides system

typedef struct _golden_mean_t
{
  int which;
  gboolean golden_section;
  gboolean golden_triangle;
  gboolean golden_spiral_section;
  gboolean golden_spiral;
} _golden_mean_t;

static void _guides_draw_grid(cairo_t *cr, const float x, const float y,
                              const float w, const float h,
                              const float zoom_scale, void *user_data)
{
  dt_guides_draw_simple_grid(cr, x, y, w, h, zoom_scale);
}
static void _guides_draw_rules_of_thirds(cairo_t *cr, const float x, const float y,
                                         const float w, const float h,
                                         const float zoom_scale, void *user_data)
{
  dt_guides_draw_rules_of_thirds(cr, x, y, w, h);
}
static void _guides_draw_metering(cairo_t *cr, const float x, const float y,
                                  const float w, const float h,
                                  const float zoom_scale, void *user_data)
{
  dt_guides_draw_metering(cr, x, y, w, h);
}
static void _guides_draw_perspective(cairo_t *cr, const float x, const float y,
                                     const float w, const float h,
                                     const float zoom_scale, void *user_data)
{
  dt_guides_draw_perspective(cr, x, y, w, h);
}
static void _guides_draw_diagonal_method(cairo_t *cr, const float x, const float y,
                                         const float w, const float h,
                                         const float zoom_scale, void *user_data)
{
  dt_guides_draw_diagonal_method(cr, x, y, w, h);
}
static void _guides_draw_harmonious_triangles(cairo_t *cr, const float x, const float y,
                                              const float w, const float h,
                                              const float zoom_scale, void *user_data)
{
  dt_guides_draw_harmonious_triangles(cr, x, y, w, h);
}
static void _guides_draw_golden_mean(cairo_t *cr, const float x, const float y,
                                     const float w, const float h,
                                     const float zoom_scale, void *user_data)
{
  _golden_mean_t *d = (_golden_mean_t *)user_data;

  // lengths for the golden mean and half the sizes of the region:
  float w_g = w * INVPHI;
  float h_g = h * INVPHI;
  float w_2 = w / 2;
  float h_2 = h / 2;

  dt_QRect_t R1, R2, R3, R4, R5, R6, R7;
  dt_guides_q_rect(&R1, -w_2, -h_2, w_g, h);

  // w - 2*w_2 corrects for one-pixel difference
  // so that R2.right() is really at the right end of the region
  dt_guides_q_rect(&R2, w_g - w_2, h_2 - h_g, w - w_g + 1 - (w - 2 * w_2), h_g);
  dt_guides_q_rect(&R3, w_2 - R2.width * INVPHI, -h_2, R2.width * INVPHI, h - R2.height);
  dt_guides_q_rect(&R4, R2.left, R1.top, R3.left - R2.left, R3.height * INVPHI);
  dt_guides_q_rect(&R5, R4.left, R4.bottom, R4.width * INVPHI, R3.height - R4.height);
  dt_guides_q_rect(&R6, R5.left + R5.width, R5.bottom - R5.height * INVPHI, R3.left - R5.right,
                   R5.height * INVPHI);
  dt_guides_q_rect(&R7, R6.right - R6.width * INVPHI, R4.bottom, R6.width * INVPHI, R5.height - R6.height);

  dt_guides_draw_golden_mean(cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7, d->golden_section, d->golden_triangle,
                             d->golden_spiral_section, d->golden_spiral);
}
static inline void _golden_mean_set_data(_golden_mean_t *data, int which)
{
  data->which = which;
  data->golden_section = which == 0 || which == 3;
  data->golden_triangle = 0;
  data->golden_spiral_section = which == 1 || which == 3;
  data->golden_spiral = which == 2 || which == 3;
}
static void _golden_mean_changed(GtkWidget *combo, _golden_mean_t *user_data)
{
  int which = dt_bauhaus_combobox_get(combo);

  // remember setting
  dt_conf_set_int("plugins/darkroom/clipping/golden_extras", which);

  _golden_mean_set_data(user_data, which);

  dt_control_queue_redraw_center();
}
static GtkWidget *_guides_gui_golden_mean(dt_iop_module_t *self, void *user_data)
{
  _golden_mean_t *data = (_golden_mean_t *)user_data;
  GtkWidget *golden_extras = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(golden_extras, NULL, _("extra"));
  dt_bauhaus_combobox_add(golden_extras, _("golden sections"));
  dt_bauhaus_combobox_add(golden_extras, _("golden spiral sections"));
  dt_bauhaus_combobox_add(golden_extras, _("golden spiral"));
  dt_bauhaus_combobox_add(golden_extras, _("all"));
  gtk_widget_set_tooltip_text(golden_extras, _("show some extra guides"));
  dt_bauhaus_combobox_set(golden_extras, data->which);
  g_signal_connect(G_OBJECT(golden_extras), "value-changed", G_CALLBACK(_golden_mean_changed), user_data);

  return golden_extras;
}


static void _guides_add_guide(GList **list, const char *name,
                              dt_guides_draw_callback draw,
                              dt_guides_widget_callback widget,
                              void *user_data, GDestroyNotify free)
{
  dt_guides_t *guide = (dt_guides_t *)malloc(sizeof(dt_guides_t));
  g_strlcpy(guide->name, name, sizeof(guide->name));
  guide->draw = draw;
  guide->widget = widget;
  guide->user_data = user_data;
  guide->free = free;
  *list = g_list_append(*list, guide);
}

void dt_guides_add_guide(const char *name, dt_guides_draw_callback draw, dt_guides_widget_callback widget, void *user_data, GDestroyNotify free)
{
  _guides_add_guide(&darktable.guides, name, draw, widget, user_data, free);
}

GList *dt_guides_init()
{
  GList *guides = NULL;


  _guides_add_guide(&guides, _("grid"), _guides_draw_grid, NULL, NULL, NULL); // TODO: make the number of lines configurable with a slider?
  _guides_add_guide(&guides, _("rules of thirds"), _guides_draw_rules_of_thirds, NULL, NULL, NULL);
  _guides_add_guide(&guides, _("metering"), _guides_draw_metering, NULL, NULL, NULL);
  _guides_add_guide(&guides, _("perspective"), _guides_draw_perspective, NULL, NULL, NULL); // TODO: make the number of lines configurable with a slider?
  _guides_add_guide(&guides, _("diagonal method"), _guides_draw_diagonal_method, NULL, NULL, NULL);
  _guides_add_guide(&guides, _("harmonious triangles"), _guides_draw_harmonious_triangles, NULL, NULL, NULL);
  {
    _golden_mean_t *user_data = (_golden_mean_t *)malloc(sizeof(_golden_mean_t));
    _golden_mean_set_data(user_data, dt_conf_get_int("plugins/darkroom/clipping/golden_extras"));
    _guides_add_guide(&guides, _("golden mean"), _guides_draw_golden_mean, _guides_gui_golden_mean, user_data, free);
  }

  return guides;
}

static void free_guide(void *data)
{
  dt_guides_t *guide = (dt_guides_t *)data;
  if(guide->free) guide->free(guide->user_data);
  free(guide);
}

void dt_guides_cleanup(GList *guides)
{
  g_list_free_full(guides, free_guide);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

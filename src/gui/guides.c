/*
 *    This file is part of darktable,
 *    Copyright (C) 2012-2020 darktable developers.
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

typedef struct _guides_settings_t
{
  GtkWidget *g_guides, *g_flip, *g_widgets;

  dt_iop_module_t *module;
  GtkWidget *m_guides, *m_flip, *m_widgets;

  GtkWidget *colors;
  gboolean block_events;
} _guides_settings_t;


// return the index of the guide in the list or 0 if not found
static int _guides_get_value(gchar *name)
{
  if(!g_strcmp0(name, "none")) return -1;
  int i = 1;
  for(GList *iter = darktable.guides; iter; iter = g_list_next(iter), i++)
  {
    dt_guides_t *guide = (dt_guides_t *)iter->data;
    if(!g_strcmp0(name, guide->name)) return i;
  }
  return 0;
}

static void dt_guides_draw_grid(cairo_t *cr, const float x, const float y, const float w, const float h,
                                float zoom_scale, void *data)
{
  // retrieve the grid values in settings
  dt_iop_module_t *module = darktable.develop->gui_module;
  int nbh = 3, nbv = 3, subdiv = 3;
  gboolean loaded = FALSE;
  if(module)
  {
    // we verify that we want the module specific setting, not the global one
    gchar *key = dt_util_dstrcat(NULL, "guides/%s/guide", module->op);
    gchar *val = dt_conf_get_string(key);
    g_free(key);
    if(!g_strcmp0(val, "grid"))
    {
      key = dt_util_dstrcat(NULL, "guides/%s/grid_nbh", module->op);
      if(dt_conf_key_exists(key)) nbh = dt_conf_get_int(key);
      g_free(key);
      key = dt_util_dstrcat(NULL, "guides/%s/grid_nbv", module->op);
      if(dt_conf_key_exists(key)) nbv = dt_conf_get_int(key);
      g_free(key);
      key = dt_util_dstrcat(NULL, "guides/%s/grid_subdiv", module->op);
      if(dt_conf_key_exists(key)) subdiv = dt_conf_get_int(key);
      g_free(key);
      loaded = TRUE;
    }
    g_free(val);
  }
  // if we want the global setting
  if(!loaded)
  {
    gchar *val = dt_conf_get_string("guides/global/guide");
    if(!g_strcmp0(val, "grid"))
    {
      if(dt_conf_key_exists("guides/global/grid_nbh")) nbh = dt_conf_get_int("guides/global/grid_nbh");
      if(dt_conf_key_exists("guides/global/grid_nbv")) nbv = dt_conf_get_int("guides/global/grid_nbv");
      if(dt_conf_key_exists("guides/global/grid_subdiv")) subdiv = dt_conf_get_int("guides/global/grid_subdiv");
      loaded = TRUE;
    }
    g_free(val);
  }
  // if stille not loaded that mean we don't want to be here !
  if(!loaded) return;

  float right = x + w;
  float bottom = y + h;
  double dashes = 5.0 / zoom_scale;

  cairo_set_line_width(cr, 1.0 / zoom_scale);

  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_set_color_overlay(cr, 0.2, 0.3);
  dt_draw_horizontal_lines(cr, (1 + nbh) * (1 + subdiv), x, y, right, bottom);
  dt_draw_vertical_lines(cr, (1 + nbv) * (1 + subdiv), x, y, right, bottom);
  cairo_set_dash(cr, &dashes, 1, dashes);
  dt_draw_set_color_overlay(cr, 0.8, 0.3);
  dt_draw_horizontal_lines(cr, (1 + nbh) * (1 + subdiv), x, y, right, bottom);
  dt_draw_vertical_lines(cr, (1 + nbv) * (1 + subdiv), x, y, right, bottom);

  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_set_color_overlay(cr, 0.2, 0.5);
  dt_draw_horizontal_lines(cr, 1 + nbh, x, y, right, bottom);
  dt_draw_vertical_lines(cr, 1 + nbv, x, y, right, bottom);

  cairo_set_dash(cr, &dashes, 1, dashes);
  dt_draw_set_color_overlay(cr, 0.8, 0.5);
  dt_draw_horizontal_lines(cr, 1 + nbh, x, y, right, bottom);
  dt_draw_vertical_lines(cr, 1 + nbv, x, y, right, bottom);
}

static void _grid_horizontal_changed(GtkWidget *w, void *data)
{
  int horizontal = dt_bauhaus_slider_get(w);
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(w), "module");
  gchar *key = dt_util_dstrcat(NULL, "guides/%s/grid_nbh", module ? module->op : "global");
  dt_conf_set_int(key, horizontal);
  g_free(key);
  dt_control_queue_redraw_center();
}

static void _grid_vertical_changed(GtkWidget *w, void *data)
{
  int vertical = dt_bauhaus_slider_get(w);
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(w), "module");
  gchar *key = dt_util_dstrcat(NULL, "guides/%s/grid_nbv", module ? module->op : "global");
  dt_conf_set_int(key, vertical);
  g_free(key);
  dt_control_queue_redraw_center();
}

static void _grid_subdiv_changed(GtkWidget *w, void *data)
{
  int subdiv = dt_bauhaus_slider_get(w);
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(w), "module");
  gchar *key = dt_util_dstrcat(NULL, "guides/%s/grid_subdiv", module ? module->op : "global");
  dt_conf_set_int(key, subdiv);
  g_free(key);
  dt_control_queue_redraw_center();
}

static GtkWidget *_guides_gui_grid(dt_iop_module_t *self, void *user_data)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  GtkWidget *grid_horizontal = dt_bauhaus_slider_new_with_range(NULL, 0, 12, 1, 3, 0);
  g_object_set_data(G_OBJECT(grid_horizontal), "module", self);
  dt_bauhaus_slider_set_hard_max(grid_horizontal, 36);
  dt_bauhaus_widget_set_label(grid_horizontal, NULL, N_("horizontal lines"));
  gtk_widget_set_tooltip_text(grid_horizontal, _("number of horizontal guide lines"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(grid_horizontal), TRUE, TRUE, 0);
  gchar *key = dt_util_dstrcat(NULL, "guides/%s/grid_nbh", self ? self->op : "global");
  dt_bauhaus_slider_set(grid_horizontal, dt_conf_key_exists(key) ? dt_conf_get_int(key) : 3);
  g_free(key);
  g_signal_connect(G_OBJECT(grid_horizontal), "value-changed", G_CALLBACK(_grid_horizontal_changed), user_data);

  GtkWidget *grid_vertical = dt_bauhaus_slider_new_with_range(NULL, 0, 12, 1, 3, 0);
  g_object_set_data(G_OBJECT(grid_vertical), "module", self);
  dt_bauhaus_slider_set_hard_max(grid_vertical, 36);
  dt_bauhaus_widget_set_label(grid_vertical, NULL, N_("vertical lines"));
  gtk_widget_set_tooltip_text(grid_vertical, _("number of vertical guide lines"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(grid_vertical), TRUE, TRUE, 0);
  key = dt_util_dstrcat(NULL, "guides/%s/grid_nbv", self ? self->op : "global");
  dt_bauhaus_slider_set(grid_vertical, dt_conf_key_exists(key) ? dt_conf_get_int(key) : 3);
  g_free(key);
  g_signal_connect(G_OBJECT(grid_vertical), "value-changed", G_CALLBACK(_grid_vertical_changed), user_data);

  GtkWidget *grid_subdiv = dt_bauhaus_slider_new_with_range(NULL, 0, 10, 1, 3, 0);
  g_object_set_data(G_OBJECT(grid_subdiv), "module", self);
  dt_bauhaus_slider_set_hard_max(grid_subdiv, 30);
  dt_bauhaus_widget_set_label(grid_subdiv, NULL, N_("subdivisions"));
  gtk_widget_set_tooltip_text(grid_subdiv, _("number of subdivisions per grid rectangle"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(grid_subdiv), TRUE, TRUE, 0);
  key = dt_util_dstrcat(NULL, "guides/%s/grid_subdiv", self ? self->op : "global");
  dt_bauhaus_slider_set(grid_subdiv, dt_conf_key_exists(key) ? dt_conf_get_int(key) : 3);
  g_free(key);
  g_signal_connect(G_OBJECT(grid_subdiv), "value-changed", G_CALLBACK(_grid_subdiv_changed), user_data);

  return box;
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
  int dst = (int)((height * cosf(atanf(width / height)) / (cosf(atanf(height / width)))));

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

static void _guides_draw_grid(cairo_t *cr, const float x, const float y,
                              const float w, const float h,
                              const float zoom_scale, void *user_data)
{
  dt_guides_draw_grid(cr, x, y, w, h, zoom_scale, user_data);
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
  // retrieve the golden extra in settings
  dt_iop_module_t *module = darktable.develop->gui_module;
  int extra = -1;
  if(module)
  {
    // we verify that we want the module specific setting, not the global one
    gchar *key = dt_util_dstrcat(NULL, "guides/%s/guide", module->op);
    gchar *val = dt_conf_get_string(key);
    g_free(key);
    if(!g_strcmp0(val, "golden mean"))
    {
      key = dt_util_dstrcat(NULL, "guides/%s/golden_extra", module->op);
      extra = dt_conf_get_int(key);
      g_free(key);
    }
    g_free(val);
  }
  // if we want the global setting
  if(extra == -1)
  {
    gchar *val = dt_conf_get_string("guides/global/guide");
    if(!g_strcmp0(val, "golden mean"))
    {
      extra = dt_conf_get_int("guides/global/golden_extra");
    }
    g_free(val);
  }
  // if extra is still -1 that mean we don't want to be here !
  if(extra < 0) return;

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

  dt_guides_draw_golden_mean(cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7, (extra == 0 || extra == 3), FALSE,
                             (extra == 1 || extra == 3), (extra == 2 || extra == 3));
}

static void _golden_mean_changed(GtkWidget *combo, void *user_data)
{
  int which = dt_bauhaus_combobox_get(combo);

  // remember setting
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(combo), "module");
  gchar *key = dt_util_dstrcat(NULL, "guides/%s/golden_extra", module ? module->op : "global");
  dt_conf_set_int(key, which);
  g_free(key);

  dt_control_queue_redraw_center();
}
static GtkWidget *_guides_gui_golden_mean(dt_iop_module_t *self, void *user_data)
{
  GtkWidget *golden_extras = dt_bauhaus_combobox_new(NULL);
  g_object_set_data(G_OBJECT(golden_extras), "module", self);
  dt_bauhaus_widget_set_label(golden_extras, NULL, N_("extra"));
  dt_bauhaus_combobox_add(golden_extras, _("golden sections"));
  dt_bauhaus_combobox_add(golden_extras, _("golden spiral sections"));
  dt_bauhaus_combobox_add(golden_extras, _("golden spiral"));
  dt_bauhaus_combobox_add(golden_extras, _("all"));
  gtk_widget_set_tooltip_text(golden_extras, _("show some extra guides"));

  // set current value
  gchar *key = dt_util_dstrcat(NULL, "guides/%s/golden_extra", self ? self->op : "global");
  dt_bauhaus_combobox_set(golden_extras, dt_conf_get_int(key));
  g_free(key);

  g_signal_connect(G_OBJECT(golden_extras), "value-changed", G_CALLBACK(_golden_mean_changed), user_data);

  return golden_extras;
}


static void _guides_add_guide(GList **list, const char *name,
                              dt_guides_draw_callback draw,
                              dt_guides_widget_callback widget,
                              void *user_data, GDestroyNotify free,
                              gboolean support_flip)
{
  dt_guides_t *guide = (dt_guides_t *)malloc(sizeof(dt_guides_t));
  g_strlcpy(guide->name, name, sizeof(guide->name));
  guide->draw = draw;
  guide->widget = widget;
  guide->user_data = user_data;
  guide->free = free;
  guide->support_flip = support_flip;
  *list = g_list_append(*list, guide);
}

void dt_guides_add_guide(const char *name, dt_guides_draw_callback draw, dt_guides_widget_callback widget, void *user_data, GDestroyNotify free)
{
  _guides_add_guide(&darktable.guides, name, draw, widget, user_data, free, TRUE);
}

GList *dt_guides_init()
{
  GList *guides = NULL;

  _guides_add_guide(&guides, _("grid"), _guides_draw_grid, _guides_gui_grid, NULL, NULL, FALSE);
  _guides_add_guide(&guides, _("rules of thirds"), _guides_draw_rules_of_thirds, NULL, NULL, NULL, FALSE);
  _guides_add_guide(&guides, _("metering"), _guides_draw_metering, NULL, NULL, NULL, FALSE);
  _guides_add_guide(&guides, _("perspective"), _guides_draw_perspective, NULL, NULL, NULL, FALSE); // TODO: make the number of lines configurable with a slider?
  _guides_add_guide(&guides, _("diagonal method"), _guides_draw_diagonal_method, NULL, NULL, NULL, FALSE);
  _guides_add_guide(&guides, _("harmonious triangles"), _guides_draw_harmonious_triangles, NULL, NULL, NULL, TRUE);
  _guides_add_guide(&guides, _("golden mean"), _guides_draw_golden_mean, _guides_gui_golden_mean, NULL, NULL, TRUE);

  return guides;
}

static void _settings_update_visibility(_guides_settings_t *gw)
{
  // show or hide the flip and extra widgets for global case
  dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(gw->g_guides) - 1);
  gtk_widget_set_visible(gw->g_flip, (guide && guide->support_flip));
  gtk_widget_set_visible(gw->g_widgets, (guide && guide->widget));
  if((guide && guide->widget))
  {
    GList *l = gtk_container_get_children(GTK_CONTAINER(gw->g_widgets));
    if(l && l->data)
    {
      GtkWidget *w = (GtkWidget *)l->data;
      gtk_widget_destroy(w);
    }
    GtkWidget *extra = guide->widget(NULL, guide->user_data);
    gtk_box_pack_start(GTK_BOX(gw->g_widgets), extra, TRUE, TRUE, 0);
    gtk_widget_show_all(extra);
  }

  // show or hide the flip and extra widgets for module case
  if(gw->module)
  {
    guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(gw->m_guides) - 2);
    gtk_widget_set_visible(gw->m_flip, (guide && guide->support_flip));
    gtk_widget_set_visible(gw->m_widgets, (guide && guide->widget));
    if((guide && guide->widget))
    {
      GList *l = gtk_container_get_children(GTK_CONTAINER(gw->m_widgets));
      if(l && l->data)
      {
        GtkWidget *w = (GtkWidget *)l->data;
        gtk_widget_destroy(w);
      }
      GtkWidget *extra = guide->widget(gw->module, guide->user_data);
      gtk_box_pack_start(GTK_BOX(gw->m_widgets), extra, TRUE, TRUE, 0);
      gtk_widget_show_all(extra);
    }
  }
}

static void _settings_flip_update(_guides_settings_t *gw)
{
  gw->block_events = TRUE;

  // we retrieve the global settings
  dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(gw->g_guides) - 1);
  if(guide && guide->support_flip)
  {
    gchar *key = dt_util_dstrcat(NULL, "guides/global/%s/flip", guide->name);
    dt_bauhaus_combobox_set(gw->g_flip, dt_conf_get_int(key));
    g_free(key);
  }
  // we retrieve the module settings
  if(gw->module)
  {
    guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(gw->m_guides) - 2);
    if(guide && guide->support_flip)
    {
      gchar *key = dt_util_dstrcat(NULL, "guides/%s/%s/flip", gw->module->op, guide->name);
      dt_bauhaus_combobox_set(gw->m_flip, dt_conf_get_int(key));
      g_free(key);
    }
  }

  gw->block_events = FALSE;
}

static void _settings_guides_changed(GtkWidget *w, _guides_settings_t *gw)
{
  if(gw->block_events) return;

  // we save the new setting
  if(w == gw->g_guides)
  {
    const int which = dt_bauhaus_combobox_get(gw->g_guides);
    dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, which - 1);
    dt_conf_set_string("guides/global/guide", guide ? guide->name : "none");
  }
  else if(gw->module && w == gw->m_guides)
  {
    const int which = dt_bauhaus_combobox_get(gw->m_guides);
    gchar *val = NULL;
    if(which == 0)
      val = g_strdup("none");
    else
    {
      dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, which - 2);
      if(guide)
        val = g_strdup(guide->name);
      else
        val = g_strdup("follow global");
    }
    gchar *key = dt_util_dstrcat(NULL, "guides/%s/guide", gw->module->op);
    dt_conf_set_string(key, val);
    g_free(val);
    g_free(key);
  }

  // we update the flip combo
  _settings_flip_update(gw);
  // we update the gui
  _settings_update_visibility(gw);

  // we update the drawing
  dt_control_queue_redraw_center();
}

static void _settings_flip_changed(GtkWidget *w, _guides_settings_t *gw)
{
  if(gw->block_events) return;

  // we save the new setting
  if(w == gw->g_flip)
  {
    const int which = dt_bauhaus_combobox_get(gw->g_guides);
    dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, which - 1);
    if(guide)
    {
      gchar *key = dt_util_dstrcat(NULL, "guides/global/%s/flip", guide->name);
      dt_conf_set_int(key, dt_bauhaus_combobox_get(w));
      g_free(key);
    }
  }
  else if(gw->module && w == gw->m_flip)
  {
    const int which = dt_bauhaus_combobox_get(gw->m_guides);
    dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, which - 2);
    if(guide)
    {
      gchar *key = dt_util_dstrcat(NULL, "guides/%s/%s/flip", gw->module->op, guide->name);
      dt_conf_set_int(key, dt_bauhaus_combobox_get(w));
      g_free(key);
    }
  }

  // we update the drawing
  dt_control_queue_redraw_center();
}

static void _settings_box_destroyed(GtkWidget *w, _guides_settings_t *gw)
{
  g_free(gw);
}

static void _settings_colors_changed(GtkWidget *combo, _guides_settings_t *gw)
{
  dt_conf_set_int("darkroom/ui/overlay_color", dt_bauhaus_combobox_get(combo));
  dt_control_queue_redraw_center();
}

// return the box to be included in the settings popup
GtkWidget *dt_guides_get_widgets(dt_iop_module_t *module)
{
  // create a new struct for all the widgets
  _guides_settings_t *gw = (_guides_settings_t *)g_malloc0(sizeof(_guides_settings_t));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_signal_connect(G_OBJECT(vbox), "destroy", G_CALLBACK(_settings_box_destroyed), gw);

  // global guides section
  gtk_box_pack_start(GTK_BOX(vbox), dt_ui_section_label_new(_("global guides")), TRUE, TRUE, 0);

  gw->g_guides = dt_bauhaus_combobox_new(NULL);
  gtk_widget_set_tooltip_text(gw->g_guides, _("display guide lines to help compose your photograph"));
  dt_bauhaus_widget_set_label(gw->g_guides, NULL, N_("guides"));
  gtk_box_pack_start(GTK_BOX(vbox), gw->g_guides, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(gw->g_guides, _("none"));
  for(GList *iter = darktable.guides; iter; iter = g_list_next(iter))
  {
    dt_guides_t *guide = (dt_guides_t *)iter->data;
    dt_bauhaus_combobox_add(gw->g_guides, _(guide->name));
  }

  gw->g_widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), gw->g_widgets, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all(gw->g_widgets, TRUE);

  gw->g_flip = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gw->g_flip, NULL, N_("flip guides"));
  dt_bauhaus_combobox_add(gw->g_flip, _("none"));
  dt_bauhaus_combobox_add(gw->g_flip, _("horizontally"));
  dt_bauhaus_combobox_add(gw->g_flip, _("vertically"));
  dt_bauhaus_combobox_add(gw->g_flip, _("both"));
  gtk_widget_set_tooltip_text(gw->g_flip, _("flip guides"));
  gtk_box_pack_start(GTK_BOX(vbox), gw->g_flip, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all(gw->g_flip, TRUE);

  gchar *val = dt_conf_get_string("guides/global/guide");
  int i = MAX(0, _guides_get_value(val));
  g_free(val);
  dt_bauhaus_combobox_set(gw->g_guides, i);

  g_signal_connect(G_OBJECT(gw->g_guides), "value-changed", G_CALLBACK(_settings_guides_changed), gw);
  g_signal_connect(G_OBJECT(gw->g_flip), "value-changed", G_CALLBACK(_settings_flip_changed), gw);

  // module specific guides section
  if(module)
  {
    gw->module = module;
    gchar *tx = dt_util_dstrcat(NULL, "%s '%s'", _("guides for"), module->name());
    gtk_box_pack_start(GTK_BOX(vbox), dt_ui_section_label_new(tx), TRUE, TRUE, 0);
    g_free(tx);

    gw->m_guides = dt_bauhaus_combobox_new(NULL);
    gtk_widget_set_tooltip_text(gw->m_guides, _("display guide lines to help compose your photograph"));
    dt_bauhaus_widget_set_label(gw->m_guides, NULL, N_("guides"));
    gtk_box_pack_start(GTK_BOX(vbox), gw->m_guides, TRUE, TRUE, 0);
    dt_bauhaus_combobox_add(gw->m_guides, _("none"));
    dt_bauhaus_combobox_add(gw->m_guides, _("follow global setting"));
    for(GList *iter = darktable.guides; iter; iter = g_list_next(iter))
    {
      dt_guides_t *guide = (dt_guides_t *)iter->data;
      dt_bauhaus_combobox_add(gw->m_guides, _(guide->name));
    }

    gw->m_widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gw->m_widgets, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(gw->m_widgets, TRUE);

    gw->m_flip = dt_bauhaus_combobox_new(NULL);
    dt_bauhaus_widget_set_label(gw->m_flip, NULL, N_("flip guides"));
    dt_bauhaus_combobox_add(gw->m_flip, _("none"));
    dt_bauhaus_combobox_add(gw->m_flip, _("horizontally"));
    dt_bauhaus_combobox_add(gw->m_flip, _("vertically"));
    dt_bauhaus_combobox_add(gw->m_flip, _("both"));
    gtk_widget_set_tooltip_text(gw->m_flip, _("flip guides"));
    gtk_box_pack_start(GTK_BOX(vbox), gw->m_flip, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(gw->m_flip, TRUE);

    gchar *key = dt_util_dstrcat(NULL, "guides/%s/guide", module->op);
    val = dt_conf_get_string(key);
    i = _guides_get_value(val) + 1;
    g_free(val);
    g_free(key);
    dt_bauhaus_combobox_set(gw->m_guides, i);

    g_signal_connect(G_OBJECT(gw->m_guides), "value-changed", G_CALLBACK(_settings_guides_changed), gw);
    g_signal_connect(G_OBJECT(gw->m_flip), "value-changed", G_CALLBACK(_settings_flip_changed), gw);
  }

  // update flip values
  _settings_flip_update(gw);
  // update visibility of sub-widgets
  _settings_update_visibility(gw);

  // color section
  gtk_box_pack_start(GTK_BOX(vbox), dt_ui_section_label_new(_("guides color")), TRUE, TRUE, 0);

  gw->colors = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(gw->colors, NULL, N_("overlay color"));
  dt_bauhaus_combobox_add(gw->colors, _("gray"));
  dt_bauhaus_combobox_add(gw->colors, _("red"));
  dt_bauhaus_combobox_add(gw->colors, _("green"));
  dt_bauhaus_combobox_add(gw->colors, _("yellow"));
  dt_bauhaus_combobox_add(gw->colors, _("cyan"));
  dt_bauhaus_combobox_add(gw->colors, _("magenta"));
  dt_bauhaus_combobox_set(gw->colors, dt_conf_get_int("darkroom/ui/overlay_color"));
  gtk_widget_set_tooltip_text(gw->colors, _("set overlay color"));
  g_signal_connect(G_OBJECT(gw->colors), "value-changed", G_CALLBACK(_settings_colors_changed), gw);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(gw->colors), TRUE, TRUE, 0);

  return vbox;
}

void dt_guides_draw(cairo_t *cr, const float left, const float top, const float width, const float height,
                    const float zoom_scale)
{
  const double dashes = DT_PIXEL_APPLY_DPI(5.0) / zoom_scale;

  dt_iop_module_t *module = darktable.develop->gui_module;

  // first, we look at the module specific guide
  gchar *key, *val;
  dt_guides_t *guide = NULL;
  gboolean global = FALSE;
  if(module)
  {
    key = dt_util_dstrcat(NULL, "guides/%s/guide", module->op);
    if(dt_conf_key_exists(key))
    {
      val = dt_conf_get_string(key);
      if(!g_strcmp0(val, "none"))
      {
        // module specific value to not draw guides
        g_free(val);
        g_free(key);
        return;
      }
      guide = (dt_guides_t *)g_list_nth_data(darktable.guides, _guides_get_value(val) - 1);
      g_free(val);
    }
    g_free(key);
  }
  // if still no valid guide, we try the global guide
  if(!guide)
  {
    global = TRUE;
    if(dt_conf_key_exists("guides/global/guide"))
    {
      val = dt_conf_get_string("guides/global/guide");
      guide = (dt_guides_t *)g_list_nth_data(darktable.guides, _guides_get_value(val) - 1);
      g_free(val);
    }
  }
  // if no guide at this point : nothing to draw :)
  if(!guide) return;

  int flip = 0;
  // retrieve guide flip
  if(guide->support_flip)
  {
    key = dt_util_dstrcat(NULL, "guides/%s/%s/flip", global ? "global" : module->op, guide->name);
    if(dt_conf_key_exists(key)) flip = dt_conf_get_int(key);
    g_free(key);
  }

  // save context
  cairo_save(cr);
  cairo_rectangle(cr, left, top, width, height);
  cairo_clip(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, 0.8, 1.0);
  cairo_set_dash(cr, &dashes, 1, 0);

  // Move coordinates to local center selection.
  cairo_translate(cr, (width / 2 + left), (height / 2 + top));

  // Flip horizontal.
  if(flip == 1 || flip == 3) cairo_scale(cr, -1, 1);
  // Flip vertical.
  if(flip == 2 || flip == 3) cairo_scale(cr, 1, -1);

  // we do the drawing itself
  guide->draw(cr, -width / 2.0, -height / 2.0, width, height, zoom_scale, guide->user_data);

  cairo_stroke_preserve(cr);
  cairo_set_dash(cr, &dashes, 0, 0);
  dt_draw_set_color_overlay(cr, 0.3, 0.8);
  cairo_stroke(cr);

  cairo_restore(cr);
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

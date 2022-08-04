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
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "gui/guides.h"

#define DEFAULT_GUIDE_NAME "rules of thirds"

static const char *_guide_names[]
  = { N_("Grid"),
      N_("Rules of thirds"),
      N_("Metering"),
      N_("Perspective"),
      N_("Diagonal method"),
      N_("Harmonious triangles"),
      N_("Golden sections"),
      N_("Golden spiral"),
      N_("Golden spiral sections"),
      N_("Golden mean (all guides)"),
      NULL };

typedef enum dt_golden_type_t
{
  GOLDEN_SECTION = 0,
  GOLDEN_SPIRAL,
  GOLDEN_SPIRAL_SECTION,
  GOLDEN_ALL
} dt_golden_type_t;

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
  GtkWidget *g_flip, *g_widgets;
} _guides_settings_t;


// return the index of the guide in the list or -1 if not found
static int _guides_get_value(gchar *name)
{
  int i = 0;
  for(GList *iter = darktable.guides; iter; iter = g_list_next(iter), i++)
  {
    dt_guides_t *guide = (dt_guides_t *)iter->data;
    if(!g_strcmp0(name, guide->name)) return i;
  }
  return -1;
}

static gchar *_conf_get_path(gchar *module_name, gchar *property_1, gchar *property_2)
{
  if(!darktable.view_manager) return NULL;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  // in lighttable, we store panels states per layout
  char lay[32] = "";
  if(g_strcmp0(cv->module_name, "lighttable") == 0)
  {
    if(dt_view_lighttable_preview_state(darktable.view_manager))
      g_snprintf(lay, sizeof(lay), "preview/");
    else
      g_snprintf(lay, sizeof(lay), "%d/", dt_view_lighttable_get_layout(darktable.view_manager));
  }
  else if(g_strcmp0(cv->module_name, "darkroom") == 0)
  {
    g_snprintf(lay, sizeof(lay), "%d/", dt_view_darkroom_get_layout(darktable.view_manager));
  }

  if(property_2)
    return dt_util_dstrcat(NULL, "guides/%s/%s%s/%s/%s", cv->module_name, lay, module_name, property_1, property_2);
  else
    return dt_util_dstrcat(NULL, "guides/%s/%s%s/%s", cv->module_name, lay, module_name, property_1);
}

static dt_guides_t *_conf_get_guide(gchar *module_name)
{
  dt_guides_t *guide = NULL;
  gchar *key = _conf_get_path(module_name, "guide", NULL);
  if(!dt_conf_key_exists(key)) dt_conf_set_string(key, DEFAULT_GUIDE_NAME);
  gchar *val = dt_conf_get_string(key);
  guide = (dt_guides_t *)g_list_nth_data(darktable.guides, _guides_get_value(val));
  g_free(val);
  g_free(key);

  // if we don't have any valid guide, let's fallback to default one
  if(!guide)
  {
    // global case : we fallback to "rule of third"
    guide = (dt_guides_t *)g_list_nth_data(darktable.guides, 1);
  }

  return guide;
}

static gchar *_conf_get_guide_name(gchar *module_name)
{
  dt_guides_t *guide = _conf_get_guide(module_name);
  if(guide) return guide->name;
  return NULL;
}

static void dt_guides_draw_grid(cairo_t *cr, const float x, const float y, const float w, const float h,
                                float zoom_scale, void *data)
{
  // retrieve the grid values in settings
  int nbh = 3, nbv = 3, subdiv = 3;
  gboolean loaded = FALSE;

  // if we want the global setting
  gchar *val = _conf_get_guide_name("global");
  if(val && !g_strcmp0(val, "grid"))
  {
    gchar *key = _conf_get_path("global", "grid_nbh", NULL);
    if(dt_conf_key_exists(key)) nbh = dt_conf_get_int(key);
    g_free(key);
    key = _conf_get_path("global", "grid_nbv", NULL);
    if(dt_conf_key_exists(key)) nbv = dt_conf_get_int(key);
    g_free(key);
    key = _conf_get_path("global", "grid_subdiv", NULL);
    if(dt_conf_key_exists(key)) subdiv = dt_conf_get_int(key);
    g_free(key);
    loaded = TRUE;
  }
  // if stille not loaded that mean we don't want to be here !
  if(!loaded) return;

  float right = x + w;
  float bottom = y + h;
  double dashes = 5.0 / zoom_scale;

  cairo_set_line_width(cr, 1.0 / zoom_scale);

  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_set_color_overlay(cr, FALSE, 0.3);
  dt_draw_horizontal_lines(cr, (1 + nbh) * (1 + subdiv), x, y, right, bottom);
  dt_draw_vertical_lines(cr, (1 + nbv) * (1 + subdiv), x, y, right, bottom);
  cairo_set_dash(cr, &dashes, 1, dashes);
  dt_draw_set_color_overlay(cr, TRUE, 0.3);
  dt_draw_horizontal_lines(cr, (1 + nbh) * (1 + subdiv), x, y, right, bottom);
  dt_draw_vertical_lines(cr, (1 + nbv) * (1 + subdiv), x, y, right, bottom);

  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_set_color_overlay(cr, FALSE, 0.5);
  dt_draw_horizontal_lines(cr, 1 + nbh, x, y, right, bottom);
  dt_draw_vertical_lines(cr, 1 + nbv, x, y, right, bottom);

  cairo_set_dash(cr, &dashes, 1, dashes);
  dt_draw_set_color_overlay(cr, TRUE, 0.5);
  dt_draw_horizontal_lines(cr, 1 + nbh, x, y, right, bottom);
  dt_draw_vertical_lines(cr, 1 + nbv, x, y, right, bottom);
}

static void _grid_horizontal_changed(GtkWidget *w, void *data)
{
  int horizontal = dt_bauhaus_slider_get(w);
  gchar *key = _conf_get_path("global", "grid_nbh", NULL);
  dt_conf_set_int(key, horizontal);
  g_free(key);
  dt_control_queue_redraw_center();
}

static void _grid_vertical_changed(GtkWidget *w, void *data)
{
  int vertical = dt_bauhaus_slider_get(w);
  gchar *key = _conf_get_path("global", "grid_nbv", NULL);
  dt_conf_set_int(key, vertical);
  g_free(key);
  dt_control_queue_redraw_center();
}

static void _grid_subdiv_changed(GtkWidget *w, void *data)
{
  int subdiv = dt_bauhaus_slider_get(w);
  gchar *key = _conf_get_path("global", "grid_subdiv", NULL);
  dt_conf_set_int(key, subdiv);
  g_free(key);
  dt_control_queue_redraw_center();
}

static GtkWidget *_guides_gui_grid(dt_iop_module_t *self, void *user_data)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  GtkWidget *grid_horizontal = dt_bauhaus_slider_new_with_range(NULL, 0, 12, 1, 3, 0);
  dt_bauhaus_slider_set_hard_max(grid_horizontal, 36);
  dt_bauhaus_widget_set_label(grid_horizontal, NULL, N_("Horizontal lines"));
  gtk_widget_set_tooltip_text(grid_horizontal, _("Number of horizontal guide lines"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(grid_horizontal), TRUE, TRUE, 0);
  gchar *key = _conf_get_path("global", "grid_nbh", NULL);
  dt_bauhaus_slider_set(grid_horizontal, dt_conf_key_exists(key) ? dt_conf_get_int(key) : 3);
  g_free(key);
  g_signal_connect(G_OBJECT(grid_horizontal), "value-changed", G_CALLBACK(_grid_horizontal_changed), user_data);

  GtkWidget *grid_vertical = dt_bauhaus_slider_new_with_range(NULL, 0, 12, 1, 3, 0);
  dt_bauhaus_slider_set_hard_max(grid_vertical, 36);
  dt_bauhaus_widget_set_label(grid_vertical, NULL, N_("Vertical lines"));
  gtk_widget_set_tooltip_text(grid_vertical, _("Number of vertical guide lines"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(grid_vertical), TRUE, TRUE, 0);
  key = _conf_get_path("global", "grid_nbv", NULL);
  dt_bauhaus_slider_set(grid_vertical, dt_conf_key_exists(key) ? dt_conf_get_int(key) : 3);
  g_free(key);
  g_signal_connect(G_OBJECT(grid_vertical), "value-changed", G_CALLBACK(_grid_vertical_changed), user_data);

  GtkWidget *grid_subdiv = dt_bauhaus_slider_new_with_range(NULL, 0, 10, 1, 3, 0);
  dt_bauhaus_slider_set_hard_max(grid_subdiv, 30);
  dt_bauhaus_widget_set_label(grid_subdiv, NULL, N_("Subdivisions"));
  gtk_widget_set_tooltip_text(grid_subdiv, _("Number of subdivisions per grid rectangle"));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(grid_subdiv), TRUE, TRUE, 0);
  key = _conf_get_path("global", "grid_subdiv", NULL);
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
  dt_golden_type_t extra = GOLDEN_SECTION;
  if(user_data) extra = GPOINTER_TO_INT(user_data);

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

  dt_guides_draw_golden_mean(
      cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7, (extra == GOLDEN_SECTION || extra == GOLDEN_ALL), FALSE,
      (extra == GOLDEN_SPIRAL_SECTION || extra == GOLDEN_ALL), (extra == GOLDEN_SPIRAL || extra == GOLDEN_ALL));
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

  gchar *key, *val;
  key = _conf_get_path("global", "guide", NULL);
  if(key)
  {
    val = dt_conf_get_string(key);
    if(val)
    {
      const int i = _guides_get_value(val);
      g_free(val);
      dt_bauhaus_combobox_set(darktable.view_manager->guides, i);
    }
    g_free(key);
  }
}

void dt_guides_add_guide(const char *name, dt_guides_draw_callback draw, dt_guides_widget_callback widget, void *user_data, GDestroyNotify free)
{
  _guides_add_guide(&darktable.guides, name, draw, widget, user_data, free, TRUE);

  dt_bauhaus_combobox_add(darktable.view_manager->guides, _(name));
}

GList *dt_guides_init()
{
  GList *guides = NULL;

  const char **names = _guide_names;
  _guides_add_guide(&guides, *names++, _guides_draw_grid, _guides_gui_grid, NULL, NULL, FALSE);
  _guides_add_guide(&guides, *names++, _guides_draw_rules_of_thirds, NULL, NULL, NULL, FALSE);
  _guides_add_guide(&guides, *names++, _guides_draw_metering, NULL, NULL, NULL, FALSE);
  _guides_add_guide(&guides, *names++, _guides_draw_perspective, NULL, NULL, NULL, FALSE); // TODO: make the number of lines configurable with a slider?
  _guides_add_guide(&guides, *names++, _guides_draw_diagonal_method, NULL, NULL, NULL, FALSE);
  _guides_add_guide(&guides, *names++, _guides_draw_harmonious_triangles, NULL, NULL, NULL, TRUE);
  _guides_add_guide(&guides, *names++, _guides_draw_golden_mean, NULL, GINT_TO_POINTER(GOLDEN_SECTION), NULL, TRUE);
  _guides_add_guide(&guides, *names++, _guides_draw_golden_mean, NULL, GINT_TO_POINTER(GOLDEN_SPIRAL), NULL, TRUE);
  _guides_add_guide(&guides, *names++, _guides_draw_golden_mean, NULL, GINT_TO_POINTER(GOLDEN_SPIRAL_SECTION), NULL, TRUE);
  _guides_add_guide(&guides, *names++, _guides_draw_golden_mean, NULL, GINT_TO_POINTER(GOLDEN_ALL), NULL, TRUE);

  return guides;
}

static void _settings_update_visibility(_guides_settings_t *gw)
{
  // show or hide the flip and extra widgets for global case
  dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(darktable.view_manager->guides));
  gtk_widget_set_visible(gw->g_flip, (guide && guide->support_flip));
  gtk_widget_set_visible(gw->g_widgets, (guide && guide->widget));
  if((guide && guide->widget))
  {
    GtkWidget *w = gtk_bin_get_child(GTK_BIN(gw->g_widgets));
    if(w) gtk_widget_destroy(w);

    GtkWidget *extra = guide->widget(NULL, guide->user_data);
    gtk_container_add(GTK_CONTAINER(gw->g_widgets), extra);
    gtk_widget_show_all(extra);
  }
}

static void _settings_flip_update(_guides_settings_t *gw)
{
  ++darktable.gui->reset;

  // we retrieve the global settings
  dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(darktable.view_manager->guides));
  if(guide && guide->support_flip)
  {
    gchar *key = _conf_get_path("global", guide->name, "flip");
    dt_bauhaus_combobox_set(gw->g_flip, dt_conf_get_int(key));
    g_free(key);
  }

  --darktable.gui->reset;
}

static void _settings_guides_changed(GtkWidget *w, _guides_settings_t *gw)
{
  // we save the new setting
  dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(darktable.view_manager->guides));
  gchar *key = _conf_get_path("global", "guide", NULL);
  dt_conf_set_string(key, guide ? guide->name : "rule of thirds");
  g_free(key);

  // we update the flip combo
  _settings_flip_update(gw);
  // we update the gui
  _settings_update_visibility(gw);
  // we update global button state
  dt_guides_update_button_state();

  // we update the drawing
  dt_control_queue_redraw_center();
}

static void _settings_flip_changed(GtkWidget *w, _guides_settings_t *gw)
{
  // we save the new setting
  dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, dt_bauhaus_combobox_get(darktable.view_manager->guides));
  if(guide)
  {
    gchar *key = _conf_get_path("global", guide->name, "flip");
    dt_conf_set_int(key, dt_bauhaus_combobox_get(w));
    g_free(key);
  }

  // we update the drawing
  dt_control_queue_redraw_center();
}

void dt_guides_set_overlay_colors()
{
  const int overlay_color = dt_conf_get_int("darkroom/ui/overlay_color");

  darktable.gui->overlay_contrast = dt_conf_get_float("darkroom/ui/overlay_contrast");

  darktable.gui->overlay_red = darktable.gui->overlay_green = darktable.gui->overlay_blue = 0.0f;

  if(overlay_color == DT_DEV_OVERLAY_GRAY)
    darktable.gui->overlay_red = darktable.gui->overlay_green = darktable.gui->overlay_blue = 1.0f;
  else if(overlay_color == DT_DEV_OVERLAY_RED)
    darktable.gui->overlay_red = 1.0f;
  else if(overlay_color == DT_DEV_OVERLAY_GREEN)
    darktable.gui->overlay_green = 1.0f;
  else if(overlay_color == DT_DEV_OVERLAY_YELLOW)
    darktable.gui->overlay_red = darktable.gui->overlay_green = 1.0f;
  else if(overlay_color == DT_DEV_OVERLAY_CYAN)
    darktable.gui->overlay_green = darktable.gui->overlay_blue = 1.0f;
  else if(overlay_color == DT_DEV_OVERLAY_MAGENTA)
    darktable.gui->overlay_red = darktable.gui->overlay_blue = 1.0f;
}

static void _settings_colors_changed(GtkWidget *combo, _guides_settings_t *gw)
{
  dt_conf_set_int("darkroom/ui/overlay_color", dt_bauhaus_combobox_get(combo));
  dt_guides_set_overlay_colors();
  dt_control_queue_redraw_center();
}

static void _settings_contrast_changed(GtkWidget *slider, _guides_settings_t *gw)
{
  dt_conf_set_float("darkroom/ui/overlay_contrast", dt_bauhaus_slider_get(slider));
  dt_guides_set_overlay_colors();
  dt_control_queue_redraw_center();
}

// return the box to be included in the settings popup
GtkWidget *dt_guides_popover(dt_view_t *self, GtkWidget *button)
{
  GtkWidget *pop = gtk_popover_new(button);

  // create a new struct for all the widgets
  _guides_settings_t *gw = (_guides_settings_t *)g_malloc0(sizeof(_guides_settings_t));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // title
  GtkWidget *lb = gtk_label_new(_("Global guide overlay settings"));
  gtk_label_set_justify(GTK_LABEL(lb), GTK_JUSTIFY_CENTER);
  dt_gui_add_class(lb, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(vbox), lb, TRUE, TRUE, 0);

  // global guides section
  gw->g_widgets = gtk_event_box_new();
  gtk_box_pack_start(GTK_BOX(vbox), gw->g_widgets, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all(gw->g_widgets, TRUE);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(gw->g_flip, self, N_("Guide lines"), N_("Flip"), _("Flip guides"),
                               0, (GtkCallback)_settings_flip_changed, gw,
                               N_("None"),
                               N_("Horizontally"),
                               N_("Vertically"),
                               N_("Both"));
  gtk_box_pack_start(GTK_BOX(vbox), gw->g_flip, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all(gw->g_flip, TRUE);

  darktable.view_manager->guides = dt_bauhaus_combobox_new_full(DT_ACTION(self), N_("Guide lines"), N_("Type"),
                                                                _("Setup guide lines"),
                                                                0, (GtkCallback)_settings_guides_changed, gw, _guide_names);
  gtk_box_pack_start(GTK_BOX(vbox), darktable.view_manager->guides, TRUE, TRUE, 0);

  // color section
  gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), TRUE, TRUE, 0);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(darktable.view_manager->guides_colors, self, N_("Guide lines"), N_("Overlay color"), _("Set overlay color"),
                               dt_conf_get_int("darkroom/ui/overlay_color"), (GtkCallback)_settings_colors_changed, gw,
                               N_("Gray"),
                               N_("Red"),
                               N_("Green"),
                               N_("Yellow"),
                               N_("Cyan"),
                               N_("Magenta"));
  // NOTE: any change in the number of entries above will require a corresponding change in _overlay_cycle_callback
  // in src/views/darkroom.c
  gtk_box_pack_start(GTK_BOX(vbox), darktable.view_manager->guides_colors, TRUE, TRUE, 0);

  GtkWidget *contrast = darktable.view_manager->guides_contrast = dt_bauhaus_slider_new_action(DT_ACTION(self), 0, 1, 0.005, 0.5, 3);
  dt_bauhaus_widget_set_label(contrast, N_("Guide lines"), N_("Contrast"));
  gtk_widget_set_tooltip_text(contrast, N_("Set the contrast between the lightest and darkest part of the guide overlays"));
  dt_bauhaus_slider_set(contrast, dt_conf_get_float("darkroom/ui/overlay_contrast"));
  gtk_box_pack_start(GTK_BOX(vbox), contrast, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(contrast), "value-changed", G_CALLBACK(_settings_contrast_changed), NULL);

  gtk_container_add(GTK_CONTAINER(pop), vbox);

  gtk_widget_show_all(vbox);

  return pop;
}

void dt_guides_update_button_state()
{
  if(!darktable.view_manager) return;
  GtkWidget *bt = darktable.view_manager->guides_toggle;

  gchar *key = _conf_get_path("global", "show", NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bt), dt_conf_get_bool(key));
  g_free(key);
}

void dt_guides_button_toggled(gboolean active)
{
  gchar *key = _conf_get_path("global", "show", NULL);
  dt_conf_set_bool(key, active);
  g_free(key);
}

void dt_guides_draw(cairo_t *cr, const float left, const float top, const float width, const float height,
                    const float zoom_scale)
{
  const double dashes = DT_PIXEL_APPLY_DPI(5.0) / zoom_scale;

  dt_iop_module_t *module = darktable.develop->gui_module;

  // first, we check if we need to show the guides or not
  gchar *key = _conf_get_path("global", "show", NULL);
  gboolean show = dt_conf_get_bool(key);
  g_free(key);

  // otherwise we try the module autoshow
  if(!show && module)
  {
    key = _conf_get_path(module->op, "autoshow", NULL);
    show = dt_conf_get_bool(key);
    g_free(key);
  }

  // if it's still false, then nothing to draw
  if(!show) return;

  // let's get the guide to show
  dt_guides_t *guide = _conf_get_guide("global");
  if(!guide) return;

  int flip = 0;
  // retrieve guide flip
  if(guide->support_flip)
  {
    key = _conf_get_path("global", guide->name, "flip");
    if(dt_conf_key_exists(key)) flip = dt_conf_get_int(key);
    g_free(key);
  }

  // save context
  cairo_save(cr);
  cairo_rectangle(cr, left, top, width, height);
  cairo_clip(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, FALSE, 0.8);
  cairo_set_dash(cr, &dashes, 0, 0);

  // Move coordinates to local center selection.
  cairo_translate(cr, (width / 2 + left), (height / 2 + top));

  // Flip horizontal.
  if(flip == 1 || flip == 3) cairo_scale(cr, -1, 1);
  // Flip vertical.
  if(flip == 2 || flip == 3) cairo_scale(cr, 1, -1);

  // we do the drawing itself
  guide->draw(cr, -width / 2.0, -height / 2.0, width, height, zoom_scale, guide->user_data);

  cairo_stroke_preserve(cr);
  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_set_color_overlay(cr, TRUE, 1.0);
  cairo_stroke(cr);

  cairo_restore(cr);
}

static void _settings_autoshow_change(GtkWidget *mi, dt_iop_module_t *module)
{
  // we inverse the autoshow value for the module
  gchar *key = _conf_get_path(module->op, "autoshow", NULL);
  dt_conf_set_bool(key, !dt_conf_get_bool(key));
  darktable.gui->reset++;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->guides_combo), dt_conf_get_bool(key));
  darktable.gui->reset--;
  g_free(key);
  dt_control_queue_redraw_center();
}

void dt_guides_add_module_menuitem(void *menu, struct dt_iop_module_t *module)
{
  GtkWidget *mi = gtk_check_menu_item_new_with_label(_("Show guides"));
  gchar *key = _conf_get_path(module->op, "autoshow", NULL);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), dt_conf_get_bool(key));
  g_free(key);
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_settings_autoshow_change), module);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
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

static void _settings_autoshow_change2(GtkWidget *combo, struct dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  gchar *key = _conf_get_path(module->op, "autoshow", NULL);
  dt_conf_set_bool(key, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(combo)));
  g_free(key);
  dt_control_queue_redraw_center();
}

static void _settings_autoshow_menu(GtkWidget *button, struct dt_iop_module_t *module)
{
  GtkWidget *popover = darktable.view_manager->guides_popover;
  gtk_popover_set_relative_to(GTK_POPOVER(popover), button);

  g_object_set(G_OBJECT(popover), "transitions-enabled", FALSE, NULL);

  dt_guides_update_popover_values();

  gtk_widget_show_all(popover);
}

void dt_guides_init_module_widget(GtkWidget *iopw, struct dt_iop_module_t *module)
{
  if(!(module->flags() & IOP_FLAGS_GUIDES_WIDGET)) return;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *cb = module->guides_combo = gtk_check_button_new_with_label(_("Show guides"));
  gtk_widget_set_name(box, "guides-module-combobox");
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(cb))), PANGO_ELLIPSIZE_START);

  gchar *key = _conf_get_path(module->op, "autoshow", NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), dt_conf_get_bool(key));
  g_free(key);

  g_signal_connect(G_OBJECT(cb), "toggled", G_CALLBACK(_settings_autoshow_change2), module);
  gtk_widget_set_tooltip_text(cb, _("Show guide overlay when this module has focus"));
  GtkWidget *ic = dtgtk_button_new(dtgtk_cairo_paint_grid, 0, NULL);
  gtk_widget_set_tooltip_text(ic, _("Change global guide settings\nNote that these settings are applied globally "
                                    "and will impact any module that shows guide overlays"));
  g_signal_connect(G_OBJECT(ic), "clicked", G_CALLBACK(_settings_autoshow_menu), module);

  // we hide it if the preference is set to "off"
  gtk_widget_set_no_show_all(box, TRUE);
  gtk_widget_show(cb);
  gtk_widget_show(ic);

  gtk_box_pack_start(GTK_BOX(box), cb, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(box), ic, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(iopw), box, TRUE, TRUE, 0);
}

void dt_guides_update_module_widget(struct dt_iop_module_t *module)
{
  if(!module->guides_combo) return;

  GtkWidget *box = gtk_widget_get_parent(module->guides_combo);
  gtk_widget_set_visible(box, dt_conf_get_bool("plugins/darkroom/show_guides_in_ui"));
}

void dt_guides_update_popover_values()
{
  // configure the values that may have changed from the last show
  gchar *key = _conf_get_path("global", "guide", NULL);
  if(!dt_conf_key_exists(key)) dt_conf_set_string(key, DEFAULT_GUIDE_NAME);
  gchar *val = dt_conf_get_string(key);
  g_free(key);
  const int i = _guides_get_value(val);
  g_free(val);
  dt_bauhaus_combobox_set(darktable.view_manager->guides, i);
  // colors
  dt_bauhaus_combobox_set(darktable.view_manager->guides_colors, dt_conf_get_int("darkroom/ui/overlay_color"));
  dt_bauhaus_slider_set(darktable.view_manager->guides_contrast, dt_conf_get_float("darkroom/ui/overlay_contrast"));
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


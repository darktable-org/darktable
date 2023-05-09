/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbtable.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

DT_MODULE(1)


typedef struct dt_lib_darktable_t
{
  // logo
  cairo_surface_t *image;
  guint8 *image_buffer;
  int image_width, image_height;
  // text with logo font
  cairo_surface_t *text;
  int text_width, text_height;
} dt_lib_darktable_t;


/* expose function for darktable module */
static gboolean _lib_darktable_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data);
/* button press callback */
static gboolean _lib_darktable_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data);
/* show the about dialog */
static void _lib_darktable_show_about_dialog();

const char *name(dt_lib_module_t *self)
{
  return _("darktable");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_ALL;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1001;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)g_malloc0(sizeof(dt_lib_darktable_t));
  self->data = (void *)d;

  /* create drawing area */
  self->widget = gtk_event_box_new();

  /* connect callbacks */
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_lib_darktable_draw_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-press-event",
                   G_CALLBACK(_lib_darktable_button_press_callback), self);

  /* create a cairo surface of dt icon */

  // first we try the SVG
  d->image = dt_util_get_logo(DT_PIXEL_APPLY_DPI(-1.0));
  if(d->image)
    d->image_buffer = cairo_image_surface_get_data(d->image);
  else
  {
    // let's fall back to the PNG
    char *logo;
    char datadir[PATH_MAX] = { 0 };

    dt_loc_get_datadir(datadir, sizeof(datadir));
    const dt_logo_season_t season = dt_util_get_logo_season();
    if(season != DT_LOGO_SEASON_NONE)
      logo = g_strdup_printf("idbutton-%d.png", (int)season);
    else
      logo = g_strdup("idbutton.png");
    char *filename = g_build_filename(datadir, "pixmaps", logo, NULL);

    cairo_surface_t *surface = cairo_image_surface_create_from_png(filename);
    g_free(logo);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      dt_print(DT_DEBUG_ALWAYS, "warning: can't load darktable logo from PNG file `%s'\n", filename);
      goto done;
    }
    const int png_width = cairo_image_surface_get_width(surface),
              png_height = cairo_image_surface_get_height(surface);

    // blow up the PNG. Ugly, but at least it has the correct size afterwards :-/
    const int width = DT_PIXEL_APPLY_DPI(png_width) * darktable.gui->ppd,
              height = DT_PIXEL_APPLY_DPI(png_height) * darktable.gui->ppd;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    d->image_buffer = (guint8 *)calloc((size_t)stride * height, sizeof(guint8));
    d->image
        = dt_cairo_image_surface_create_for_data(d->image_buffer, CAIRO_FORMAT_ARGB32, width, height, stride);
    if(cairo_surface_status(d->image) != CAIRO_STATUS_SUCCESS)
    {
      dt_print(DT_DEBUG_ALWAYS, "warning: can't load darktable logo from PNG file `%s'\n", filename);
      free(d->image_buffer);
      d->image_buffer = NULL;
      cairo_surface_destroy(d->image);
      d->image = NULL;
      goto done;
    }

    cairo_t *cr = cairo_create(d->image);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_scale(cr, darktable.gui->dpi_factor, darktable.gui->dpi_factor);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(d->image);

done:
    cairo_surface_destroy(surface);
    g_free(filename);
  }

  d->image_width = d->image ? dt_cairo_image_surface_get_width(d->image) : 0;
  d->image_height = d->image ? dt_cairo_image_surface_get_height(d->image) : 0;

  /* try to load program name as svg */
  d->text = dt_util_get_logo_text(DT_PIXEL_APPLY_DPI(-1.0));
  /* no png fallback, we'll use text */
  d->text_width = d->text ? dt_cairo_image_surface_get_width(d->text) : 0;
  d->text_height = d->text ? dt_cairo_image_surface_get_height(d->text) : 0;

  /* set size of drawing area */
  gtk_widget_set_size_request(self->widget, d->image_width + (int)DT_PIXEL_APPLY_DPI(180),
                              d->image_height + (int)DT_PIXEL_APPLY_DPI(8));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;
  cairo_surface_destroy(d->image);
  free(d->image_buffer);

  // don't leak mem via text logo surface data
  guint8 *text_img_buffer = NULL;
  if(d->text)
    text_img_buffer = cairo_image_surface_get_data(d->text);
  cairo_surface_destroy(d->text);
  free(text_img_buffer);
  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_darktable_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  // Get the normal foreground color from the CSS stylesheet
  GdkRGBA *tmpcolor;
  gtk_style_context_get(context, GTK_STATE_FLAG_NORMAL, "color", &tmpcolor, NULL);

  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  PangoFontDescription *font_desc = NULL;
  gtk_style_context_get(context, state, "font", &font_desc, NULL);

  /* paint icon image */
  if(d->image)
  {
    cairo_set_source_surface(cr, d->image, 0, (int)DT_PIXEL_APPLY_DPI(7));
    cairo_rectangle(cr, 0, 0, d->image_width + (int)DT_PIXEL_APPLY_DPI(8),
                    d->image_height + (int)DT_PIXEL_APPLY_DPI(8));
    cairo_fill(cr);
  }

  /* create a pango layout and print fancy name/version string */
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout(widget, NULL);

  /* try to use logo text in svg */
  if(d->text)
  {
    cairo_set_source_surface(cr, d->text, d->image_width + (int)DT_PIXEL_APPLY_DPI(5),
                             (int)DT_PIXEL_APPLY_DPI(12));
    cairo_rectangle(cr, 0, 0, d->image_width + d->text_width + (int)DT_PIXEL_APPLY_DPI(11),
                    d->text_height + (int)DT_PIXEL_APPLY_DPI(13));
    cairo_fill(cr);
  }
  else
  {
    /* fallback using normal text */
    pango_font_description_set_weight(font_desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(font_desc, DT_PIXEL_APPLY_DPI(25) * PANGO_SCALE);

    pango_layout_set_font_description(layout, font_desc);

    pango_layout_set_text(layout, PACKAGE_NAME, -1);
    cairo_set_source_rgba(cr, tmpcolor->red, tmpcolor->green, tmpcolor->blue, 0.7);
    cairo_move_to(cr, d->image_width + DT_PIXEL_APPLY_DPI(3.0), DT_PIXEL_APPLY_DPI(5.0));
    pango_cairo_show_layout(cr, layout);
  }

  /* print version */
  pango_font_description_set_absolute_size(font_desc, DT_PIXEL_APPLY_DPI(10) * PANGO_SCALE);
  pango_layout_set_font_description(layout, font_desc);
  pango_layout_set_text(layout, darktable_package_version, -1);
  cairo_move_to(cr, d->image_width + DT_PIXEL_APPLY_DPI(4.0), DT_PIXEL_APPLY_DPI(32.0));
  cairo_set_source_rgba(cr, tmpcolor->red, tmpcolor->green, tmpcolor->blue, 0.7);
  pango_cairo_show_layout(cr, layout);

  /* cleanup */
  gdk_rgba_free(tmpcolor);
  g_object_unref(layout);
  pango_font_description_free(font_desc);

  return TRUE;
}

static gboolean _lib_darktable_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data)
{
  /* show about box */
  _lib_darktable_show_about_dialog();
  return TRUE;
}

static void _lib_darktable_show_about_dialog()
{
  GtkWidget *dialog = gtk_about_dialog_new();
  gtk_widget_set_name (dialog, "about-dialog");
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), darktable_package_version);
  char *copyright = g_strdup_printf(_("copyright (c) the authors 2009-%s"), darktable_last_commit_year);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), copyright);
  g_free(copyright);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                _("organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "https://www.darktable.org/");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog), "website");
  dt_logo_season_t season = dt_util_get_logo_season();
  char *icon;
  if(season != DT_LOGO_SEASON_NONE)
    icon = g_strdup_printf("darktable-%d", (int)season);
  else
    icon = g_strdup("darktable");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), icon);
  g_free(icon);

  const char *str = _("all those of you that made previous releases possible");

#include "tools/darktable_authors.h"

  const char *final[] = {str, NULL };
  gtk_about_dialog_add_credit_section (GTK_ABOUT_DIALOG(dialog), _("and..."), final);

  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));

  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

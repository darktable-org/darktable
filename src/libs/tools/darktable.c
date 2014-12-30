/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "version.h"

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

DT_MODULE(1)


typedef struct dt_lib_darktable_t
{
  cairo_surface_t *image;
  guint8 *image_buffer;
  int image_width, image_height;
} dt_lib_darktable_t;


/* expose function for darktable module */
static gboolean _lib_darktable_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data);
/* button press callback */
static gboolean _lib_darktable_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data);
/* show the about dialog */
static void _lib_darktable_show_about_dialog();

const char *name()
{
  return _("darktable");
}

uint32_t views()
{
  return DT_VIEW_ALL;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_TOP_LEFT;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1001;
}

void gui_init(dt_lib_module_t *self)
{
  char filename[PATH_MAX] = { 0 };
  char datadir[PATH_MAX] = { 0 };
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
  char *logo;
  dt_logo_season_t season = get_logo_season();
  if(season != DT_LOGO_SEASON_NONE)
    logo = g_strdup_printf("%%s/pixmaps/idbutton-%d.%%s", (int)season);
  else
    logo = g_strdup("%s/pixmaps/idbutton.%s");

  dt_loc_get_datadir(datadir, sizeof(datadir));
  snprintf(filename, sizeof(filename), logo, datadir, "svg");

  // first we try the SVG
  {
    GError *error = NULL;
    RsvgHandle *svg = rsvg_handle_new_from_file(filename, &error);
    if(!svg || error)
    {
      fprintf(stderr,
              "warning: can't load darktable logo from SVG file `%s', falling back to PNG version\n%s\n",
              filename, error->message);
      g_error_free(error);
      error = NULL;
      goto png_fallback;
    }

    cairo_surface_t *surface;
    cairo_t *cr;

    RsvgDimensionData dimension;
    rsvg_handle_get_dimensions(svg, &dimension);

    int width = DT_PIXEL_APPLY_DPI(dimension.width) * darktable.gui->ppd,
        height = DT_PIXEL_APPLY_DPI(dimension.height) * darktable.gui->ppd;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    d->image_buffer = (guint8 *)calloc(stride * height, sizeof(guint8));
    surface
        = dt_cairo_image_surface_create_for_data(d->image_buffer, CAIRO_FORMAT_ARGB32, width, height, stride);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      free(d->image_buffer);
      d->image_buffer = NULL;
      g_object_unref(svg);
      fprintf(stderr, "warning: can't load darktable logo from SVG file `%s', falling back to PNG version\n",
              filename);
      goto png_fallback;
    }

    cr = cairo_create(surface);
    cairo_scale(cr, darktable.gui->dpi_factor, darktable.gui->dpi_factor);
    rsvg_handle_render_cairo(svg, cr);
    cairo_surface_flush(surface);

    d->image = surface;
    g_object_unref(svg);
  }

  goto done;

png_fallback:
  // let's fall back to the PNG
  {
    cairo_surface_t *surface;
    cairo_t *cr;

    snprintf(filename, sizeof(filename), logo, datadir, "png");
    surface = cairo_image_surface_create_from_png(filename);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      fprintf(stderr, "warning: can't load darktable logo from PNG file `%s'\n", filename);
      d->image = NULL;
      goto done;
    }
    int png_width = cairo_image_surface_get_width(surface),
        png_height = cairo_image_surface_get_height(surface);

    // blow up the PNG. Ugly, but at least it has the correct size afterwards :-/
    int width = DT_PIXEL_APPLY_DPI(png_width) * darktable.gui->ppd,
        height = DT_PIXEL_APPLY_DPI(png_height) * darktable.gui->ppd;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    d->image_buffer = (guint8 *)calloc(stride * height, sizeof(guint8));
    d->image
        = dt_cairo_image_surface_create_for_data(d->image_buffer, CAIRO_FORMAT_ARGB32, width, height, stride);
    if(cairo_surface_status(d->image) != CAIRO_STATUS_SUCCESS)
    {
      free(d->image_buffer);
      d->image_buffer = NULL;
      cairo_surface_destroy(surface);
      fprintf(stderr, "warning: can't load darktable logo from PNG file `%s'\n", filename);
      d->image = NULL;
      goto done;
    }

    cr = cairo_create(d->image);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_scale(cr, darktable.gui->dpi_factor, darktable.gui->dpi_factor);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_fill(cr);
    cairo_surface_flush(d->image);

    cairo_surface_destroy(surface);
  }

done:
  g_free(logo);

  d->image_width = d->image ? dt_cairo_image_surface_get_width(d->image) : 0;
  d->image_height = d->image ? dt_cairo_image_surface_get_height(d->image) : 0;

  /* set size of drawing area */
  gtk_widget_set_size_request(self->widget, d->image_width + (int)DT_PIXEL_APPLY_DPI(180),
                              d->image_height + (int)DT_PIXEL_APPLY_DPI(8));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;
  cairo_surface_destroy(d->image);
  free(d->image_buffer);
  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_darktable_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;

  /* get the current style */
  GdkRGBA color;
  PangoFontDescription *font_desc = NULL;
  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_get_background_color(context, state, &color);
  gtk_style_context_get(context, state, "font", &font_desc, NULL);


  /* fill background */
  gdk_cairo_set_source_rgba(cr, &color);
  cairo_paint(cr);

  /* paint icon image */
  if(d->image)
  {
    cairo_set_source_surface(cr, d->image, 0, (int)DT_PIXEL_APPLY_DPI(7));
    cairo_rectangle(cr, 0, 0, d->image_width + (int)DT_PIXEL_APPLY_DPI(8),
                    d->image_height + (int)DT_PIXEL_APPLY_DPI(8));
    cairo_fill(cr);
  }

  /* create a pango layout and print fancy  name/version string */
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout(widget, NULL);
  pango_font_description_set_weight(font_desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(font_desc, DT_PIXEL_APPLY_DPI(25) * PANGO_SCALE);
  pango_layout_set_font_description(layout, font_desc);

  pango_layout_set_text(layout, PACKAGE_NAME, -1);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to(cr, d->image_width + DT_PIXEL_APPLY_DPI(2.0), DT_PIXEL_APPLY_DPI(5.0));
  pango_cairo_show_layout(cr, layout);

  /* print version */
  pango_font_description_set_absolute_size(font_desc, DT_PIXEL_APPLY_DPI(10) * PANGO_SCALE);
  pango_layout_set_font_description(layout, font_desc);
  pango_layout_set_text(layout, PACKAGE_VERSION, -1);
  cairo_move_to(cr, d->image_width + DT_PIXEL_APPLY_DPI(4.0), DT_PIXEL_APPLY_DPI(30.0));
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3);
  pango_cairo_show_layout(cr, layout);

  /* cleanup */
  g_object_unref(layout);

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
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), PACKAGE_VERSION);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), "copyright (c) the authors 2009-2014");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                _("organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "http://www.darktable.org/");
  dt_logo_season_t season = get_logo_season();
  char *icon;
  if(season != DT_LOGO_SEASON_NONE)
    icon = g_strdup_printf("darktable-%d", (int)season);
  else
    icon = g_strdup("darktable");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), icon);
  g_free(icon);
  const char *authors[]
      = { _("* contributors *"),
          "Roman Lebedev", "Pascal de Bruijn", "Tobias Ellinghaus", "Ulrich Pegelow",
          "Pedro Côrte-Real", "Jérémy Rosen", "Pascal Obry", "johannes hanika",
          "michleb", "Dan Torop", "Ger Siemerink", "Edouard Gomez", "Henrik Andersson",
          "Richard Levitte", "parafin", "bartokk", "josepvm", "Dennis Gnad",
          "simonspa", "Moritz Lipp", "Andrew Toskin", "AlicVB", "Victor", "tatica",
          "Thomas Pryds", "Josep V. Moragues", "Ronny Kahl", "Jose Carlos Garcia Sogo",
          "Erik Gustavsson", "Cherrot Luo", "Victor Lamoine", "Torsten Bronger",
          "Mikhail Trishchenkov", "Martijn van Beers", "Jake Probst", "Dimitrios Psychogios",
          "Milan Knížek", "Michel", "Jochem Kossen", "Guilherme Brondani Torri",
          "Fernando R", "Federico Bruni", "Alexandre Prokoudine", "Stéphane Gimenez",
          "Robert William Hutton", "Robert Bieber", "Nazarii Vitak",
          "moopmonster", "Michael Neumann", "Messie1", "Matthieu Volat", "Matthias Gehre",
          "Martin Kyral", "Luis Barrancos", "Krisztian", "Kevin", "Jesper Pedersen",
          "Jean-Sébastien Pédron", "Jean-Luc Coulon (f5ibh)", "Jan Niklas Fingerle",
          "Ilya Popov", "Gabriel Ebner", "Dmitry Ashkadov", "Daniel Kraus (bovender)",
          "Colin Adams", "Besmir Godole", "a3novy",
          "And all those of you that made previous releases possible", NULL };
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

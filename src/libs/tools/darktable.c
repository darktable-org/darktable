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
}
dt_lib_darktable_t;


/* expose function for darktable module */
static gboolean _lib_darktable_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
/* button press callback */
static gboolean _lib_darktable_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
/* show the about dialog */
static void _lib_darktable_show_about_dialog();

const char* name()
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
  char filename[PATH_MAX];
  char datadir[PATH_MAX];
  /* initialize ui widgets */
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)g_malloc(sizeof(dt_lib_darktable_t));
  self->data = (void *)d;
  memset(d,0,sizeof(dt_lib_darktable_t));

  /* create drawing area */
  self->widget = gtk_event_box_new();

  /* connect callbacks */
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_darktable_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_darktable_button_press_callback), self);

  /* create a cairo surface of dt icon */
  const char *logo = is_it_xmas()?"%s/pixmaps/idbutton-2.%s":"%s/pixmaps/idbutton.%s"; // don't you dare to tell anyone
  dt_loc_get_datadir(datadir, sizeof(datadir));
  snprintf(filename, sizeof(filename), logo, datadir, "svg");

  // first we try the SVG
  {
    GError *error = NULL;
    RsvgHandle *svg = rsvg_handle_new_from_file(filename,&error);
    if(!svg || error)
    {
      fprintf(stderr, "warning: can't load darktable logo from SVG file `%s', falling back to PNG version\n%s\n", filename, error->message);
      g_error_free(error);
      error = NULL;
      goto png_fallback;
    }

    cairo_surface_t *surface;
    cairo_t *cr;

    RsvgDimensionData dimension;
    rsvg_handle_get_dimensions(svg,&dimension);

    int width = DT_PIXEL_APPLY_DPI(dimension.width),
        height = DT_PIXEL_APPLY_DPI(dimension.height);
    int stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, width);

    d->image_buffer = (guint8 *)calloc(stride * height, sizeof(guint8));
    surface = cairo_image_surface_create_for_data(d->image_buffer, CAIRO_FORMAT_ARGB32, width, height, stride);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      free(d->image_buffer);
      d->image_buffer = NULL;
      g_object_unref(svg);
      fprintf(stderr, "warning: can't load darktable logo from SVG file `%s', falling back to PNG version\n", filename);
      goto png_fallback;
    }

    cr = cairo_create(surface);
    cairo_scale(cr, darktable.gui->dpi_factor, darktable.gui->dpi_factor);
    rsvg_handle_render_cairo(svg,cr);
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
    int width = DT_PIXEL_APPLY_DPI(png_width),
        height = DT_PIXEL_APPLY_DPI(png_height);
    int stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, width);

    d->image_buffer = (guint8 *)calloc(stride * height, sizeof(guint8));
    d->image = cairo_image_surface_create_for_data(d->image_buffer, CAIRO_FORMAT_ARGB32, width, height, stride);
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
  d->image_width = d->image?cairo_image_surface_get_width(d->image):0;
  d->image_height = d->image?cairo_image_surface_get_height(d->image):0;

  /* set size of drawing area */
  gtk_widget_set_size_request(self->widget, d->image_width + DT_PIXEL_APPLY_DPI(180), d->image_height + DT_PIXEL_APPLY_DPI(8));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;
  cairo_surface_destroy(d->image);
  free(d->image_buffer);
  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_darktable_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;

  /* get the current style */
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  if(!style) style = gtk_rc_get_style(widget);

  cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(widget));

  /* fill background */
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  /* paint icon image */
  if(d->image)
  {
    cairo_set_source_surface(cr, d->image, 0, DT_PIXEL_APPLY_DPI(7));
    cairo_rectangle(cr,0,0,d->image_width + DT_PIXEL_APPLY_DPI(8), d->image_height + DT_PIXEL_APPLY_DPI(8));
    cairo_fill(cr);
  }

  /* create a pango layout and print fancy  name/version string */
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout (widget,NULL);
  pango_font_description_set_weight (style->font_desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size (style->font_desc, DT_PIXEL_APPLY_DPI(25) * PANGO_SCALE);
  pango_layout_set_font_description (layout,style->font_desc);

  pango_layout_set_text (layout,PACKAGE_NAME,-1);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to (cr, d->image_width + DT_PIXEL_APPLY_DPI(2.0), DT_PIXEL_APPLY_DPI(5.0));
  pango_cairo_show_layout (cr, layout);

  /* print version */
  pango_font_description_set_absolute_size (style->font_desc, DT_PIXEL_APPLY_DPI(10) * PANGO_SCALE);
  pango_layout_set_font_description (layout,style->font_desc);
  pango_layout_set_text (layout,PACKAGE_VERSION,-1);
  cairo_move_to (cr, d->image_width + DT_PIXEL_APPLY_DPI(4.0), DT_PIXEL_APPLY_DPI(30.0));
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3);
  pango_cairo_show_layout (cr, layout);

  /* cleanup */
  g_object_unref (layout);
  cairo_destroy(cr);

  return TRUE;
}

static gboolean _lib_darktable_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
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
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), _("organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "http://www.darktable.org/");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), is_it_xmas()?"darktable-2":"darktable");
  const char *authors[] =
  {
    _("* developers *"),
    "Henrik Andersson",
    "Johannes Hanika",
    "Tobias Ellinghaus",
    "Ulrich Pegelow",
    "",
    _("* ubuntu packaging, color management, video tutorials *"),
    "Pascal de Bruijn",
    "",
    _("* OpenCL pipeline: *"),
    "Ulrich Pegelow",
    "",
    _("* networking, battle testing, translation expert *"),
    "Alexandre Prokoudine",
    "",
    _("* contributors *"),
    "Aldric Renaudin",
    "Alexandre Prokoudine",
    "Alexey Dokuchaev",
    "Ammon Riley",
    "Anton Keks",
    "Antony Dovgal",
    "Ari Makela",
    "Benjamin Cahill",
    "Brian Teague",
    "Bruce Guenter",
    "Cherrot Luo",
    "Chris Mason",
    "Christian Tellefsen",
    "David Morel",
    "Denis Cheremisov",
    "Dennis Gnad",
    "Diego Segura",
    "Dimitrios Psychogios",
    "Eckhart Pedersen",
    "Edouard Gomez",
    "Edward Herr",
    "František Šidák",
    "Gaspard Jankowiak",
    "Ger Siemerink",
    "Gianluigi Calcaterra",
    "Guilherme Brondani Torri",
    "Ivan Tarozzi",
    "James C. McPherson",
    "Jan Kundrát",
    "Jean-Sébastien Pédron",
    "Jérémy Rosen",
    "Jesper Pedersen",
    "Joao Trindade",
    "Jon Leighton",
    "Jose Carlos Garcia Sogo",
    "Josef Wells",
    "Julian J. M",
    "Mattias Eriksson",
    "Michal Babej",
    "Michał Prędotka",
    "Moritz Lipp",
    "Olivier Tribout",
    "Pascal de Bruijn",
    "Pascal Obry",
    "parafin",
    "Petr Styblo",
    "Pierre Le Magourou",
    "Richard Levitte",
    "Richard Tollerton",
    "Robert Bieber",
    "Roland Riegel",
    "Roman Lebedev",
    "Rostyslav Pidgornyi",
    "Sergey Pavlov",
    "Simon Harhues",
    "Simon Spannagel",
    "Stuart Henderson",
    "Terry Jeffress",
    "Tim Harder",
    "Togan Muftuoglu",
    "Tom Vanderpoel",
    "Ulrich Pegelow",
    "Wolfgang Goetz",
    "Wolfgang Kuehnel",
    "Yari Adan",
    "hal",
    "jan",
    "maigl",
    "tuxuser",
    "And all those of you that made previous releases possible",
    NULL
  };
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG (dialog));
  gtk_widget_destroy(dialog);

}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

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

DT_MODULE(1)


typedef struct dt_lib_darktable_t
{
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
  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
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
  /* initialize ui widgets */
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)g_malloc(sizeof(dt_lib_darktable_t));
  self->data = (void *)d;
  memset(d,0,sizeof(dt_lib_darktable_t));

  /* create drawingarea */
  self->widget = gtk_event_box_new();

  /* connect callbacks */
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_darktable_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_darktable_button_press_callback), self);
  
  /* set size of draw area */
  int panel_width = dt_conf_get_int("panel_width");
  gtk_widget_set_size_request(self->widget, panel_width*2, 48);

}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_darktable_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  /* get the current style */
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  
  cairo_t *cr = gdk_cairo_create(widget->window);

  /* fill background */ 
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  /* create a pango layout and print fancy  name/version string */
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout (widget,NULL); 
  pango_font_description_set_weight (style->font_desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size (style->font_desc, 32 * PANGO_SCALE);
  pango_layout_set_font_description (layout,style->font_desc); 
  
  pango_layout_set_text (layout,PACKAGE_NAME,-1);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to (cr, 5.0, 5.0);
  pango_cairo_show_layout (cr, layout);

  /* print version */
  pango_font_description_set_absolute_size (style->font_desc, 10 * PANGO_SCALE);
  pango_layout_set_font_description (layout,style->font_desc);
  pango_layout_set_text (layout,PACKAGE_VERSION,-1);
  cairo_move_to (cr, 7.0, 36.0);
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
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), PACKAGE_VERSION);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), "copyright (c) johannes hanika, henrik andersson, tobias ellinghaus et al. 2009-2011");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), _("organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "http://darktable.sf.net/");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), "darktable");
  const char *authors[] =
    {
      _("* developers *"),
      "Henrik Andersson",
      "Johannes Hanika",
      "Tobias Ellinghaus",
      "",
      _("* ubuntu packaging, color management, video tutorials *"),
      "Pascal de Bruijn",
      "",
      _("* networking, battle testing, translation expert *"),
      "Alexandre Prokoudine",
      "",
      _("* contributors *"),
      "Alexandre Prokoudine",
      "Alexander Rabtchevich",
      "Andrea Purracchio",
      "Andrey Kaminsky",
      "Anton Blanchard",
      "Bernhard Schneider",
      "Boucman",
      "Brian Teague",
      "Bruce Guenter",
      "Christian Fuchs",
      "Christian Himpel",
      "Daniele Giorgis",
      "David Bremner",
      "Ger Siemerink",
      "Gianluigi Calcaterra",
      "Gregor Quade",
      "Jan Rinze",
      "Jochen Schroeder",
      "Jose Carlos Garcia Sogo",
      "Karl Mikaelsson",
      "Klaus Staedtler",
      "Mikko Ruohola",
      "Nao Nakashima",
      "Olivier Tribout",
      "Pascal de Bruijn",
      "Pascal Obry",
      "Robert Park",
      "Richard Hughes",
      "Simon Spannagel",
      "Stephen van den Berg",
      "Stuart Henderson",
      "Thierry Leconte",
      "Wyatt Olson",
      "Xavier Besse",
      "Zeus Panchenko",
    NULL
    };
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG (dialog));
  gtk_widget_destroy(dialog);

}

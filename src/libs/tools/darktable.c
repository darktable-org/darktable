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
  cairo_surface_t *darktable_icon;
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

  d->darktable_icon = cairo_image_surface_create_from_png(DARKTABLE_DATADIR"../icons/hicolor/64x64/apps/darktable.png");

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();
  gtk_widget_set_events(self->widget,
                        GDK_EXPOSURE_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_POINTER_MOTION_HINT_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_STRUCTURE_MASK);
  
  /* connect callbacks */
  //GTK_WIDGET_UNSET_FLAGS (self->widget, GTK_DOUBLE_BUFFERED);
  GTK_WIDGET_SET_FLAGS   (self->widget, GTK_APP_PAINTABLE);
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_darktable_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_darktable_button_press_callback), self);
  
  /* set size of draw area */
  int panel_width = dt_conf_get_int("panel_width");
  gtk_widget_set_size_request(self->widget, panel_width*2, 32);

}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;
  cairo_surface_destroy(d->darktable_icon);
  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_darktable_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;

  int width = widget->allocation.width, height = widget->allocation.height;

  /* get the current style */
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  
  /* fill background */ 
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  /* paint */
  cairo_set_source_surface(cr, d->darktable_icon, 0, 0);
  cairo_rectangle(cr,0,0,width,height);
  cairo_fill(cr);

  /* label */
  cairo_set_source_rgb(cr, style->fg[0].red/65535.0, style->fg[0].green/65535.0, style->fg[0].blue/65535.0);
  cairo_select_font_face(cr, "Sans",
			 CAIRO_FONT_SLANT_NORMAL,
			 CAIRO_FONT_WEIGHT_BOLD);

  cairo_set_font_size(cr, 20);
  cairo_move_to(cr, 20, 30);
  cairo_show_text(cr, PACKAGE_NAME);
  
  /* blit memsurface into widget */
  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  
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
  GtkWidget *win = darktable.gui->widgets.main_window;
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));
  gtk_dialog_run(GTK_DIALOG (dialog));
  gtk_widget_destroy(dialog);

}

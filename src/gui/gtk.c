#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <gtk/gtk.h>
#include <glade/glade.h>
#include <gdk/gdkkeysyms.h>
#include <pthread.h>

#include "common/darktable.h"
#include "library/library.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "gui/metadata.h"
#include "control/control.h"
#include "control/jobs.h"

static gboolean
expose (GtkWidget *da, GdkEventExpose *event, gpointer user_data)
{
  dt_control_expose(NULL);
  gdk_draw_drawable(da->window,
      da->style->fg_gc[GTK_WIDGET_STATE(da)], darktable.gui->pixmap,
      // Only copy the area that was exposed.
      event->area.x, event->area.y,
      event->area.x, event->area.y,
      event->area.width, event->area.height);

  // update other widgets
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "navigation");
  gtk_widget_queue_draw(widget);

  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram");
  gtk_widget_queue_draw(widget);

  widget = glade_xml_get_widget (darktable.gui->main_window, "tonecurve");
  gtk_widget_queue_draw(widget);

  // test quit cond (thread safe, 2nd pass)
  if(!darktable.control->running)
  {
    dt_cleanup();
    gtk_main_quit();
  }
  else
  {
    // TODO: reset modules (iop) !
    widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_expander");
    if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) dt_gui_metadata_update();

    // reset operations, update expanders
    dt_dev_operation_t op;
    DT_CTL_GET_GLOBAL_STR(op, dev_op, 20);
    darktable.gui->reset = 1;
    float gamma, linear, zoom;
    DT_CTL_GET_IMAGE(gamma, dev_gamma_gamma);
    DT_CTL_GET_IMAGE(linear, dev_gamma_linear);
    widget = glade_xml_get_widget (darktable.gui->main_window, "gamma_linear");
    gtk_range_set_value(GTK_RANGE(widget), linear);
    widget = glade_xml_get_widget (darktable.gui->main_window, "gamma_gamma");
    gtk_range_set_value(GTK_RANGE(widget), gamma);

#ifndef DT_USE_GEGL
    // reset non-fixed pipeline:
    dt_iop_gui_reset();
#else
    // TODO: reset all modules..?
#endif

    DT_CTL_GET_GLOBAL(zoom, lib_zoom);
    widget = glade_xml_get_widget (darktable.gui->main_window, "library_zoom");
    gtk_range_set_value(GTK_RANGE(widget), zoom);
    darktable.gui->reset = 0;
  }

	return TRUE;
}

void
export_button_clicked (GtkWidget *widget, gpointer user_data)
{
  // read "export_format" to global settings
  GtkWidget *wid = glade_xml_get_widget (darktable.gui->main_window, "export_format");
  gchar *text = gtk_combo_box_get_active_text(GTK_COMBO_BOX(wid));
  if(text != NULL)
  {
    if     (!strcmp(text, "8-bit jpg"))  { DT_CTL_SET_GLOBAL(dev_export_format, DT_DEV_EXPORT_JPG);   }
    else if(!strcmp(text, "8-bit png"))  { DT_CTL_SET_GLOBAL(dev_export_format, DT_DEV_EXPORT_PNG);   }
    else if(!strcmp(text, "16-bit ppm")) { DT_CTL_SET_GLOBAL(dev_export_format, DT_DEV_EXPORT_PPM16); }
    else if(!strcmp(text, "float pfm"))  { DT_CTL_SET_GLOBAL(dev_export_format, DT_DEV_EXPORT_PFM);   }
    g_free(text);
  }
  pthread_mutex_lock(&(darktable.library->film->images_mutex));
  darktable.library->film->last_exported = 0;
  pthread_mutex_unlock(&(darktable.library->film->images_mutex));
  for(int k=0;k<MAX(1,dt_ctl_get_num_procs()-1);k++) // keep one proc for the user.
  {
    dt_job_t j;
    dt_dev_export_init(&j);
    dt_control_add_job(darktable.control, &j);
  }
}

void
film_button_clicked (GtkWidget *widget, gpointer user_data)
{
  long int num = (long int)user_data;
  (void)dt_film_roll_open_recent(darktable.library->film, num);
}

void
history_button_clicked (GtkWidget *widget, gpointer user_data)
{
  // revert to given history item.
  long int num = (long int)user_data;
  if(num != 0) num += darktable.control->history_start - 1;
  dt_dev_pop_history_items(darktable.develop, num);
}

void
import_button_clicked (GtkWidget *widget, gpointer user_data)
{
  GtkWidget *win = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  GtkWidget *filechooser = gtk_file_chooser_dialog_new ("Import file or directory",
				      GTK_WINDOW (win),
				      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, //GTK_FILE_CHOOSER_ACTION_OPEN,
				      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				      GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
				      NULL);

  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filename;
    filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
    dt_film_roll_import(darktable.library->film, filename);
    g_free (filename);
  }
  gtk_widget_destroy (filechooser);
  win = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(win);
}

static gboolean
scrolled (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int zoom;
  DT_CTL_GET_GLOBAL(zoom, lib_zoom);
  if(event->direction == GDK_SCROLL_UP)
  {
    zoom-=2;
    if(zoom < 1) zoom = 1;
    DT_CTL_SET_GLOBAL(lib_zoom, zoom);
  }
  else if(event->direction == GDK_SCROLL_DOWN)
  {
    zoom+=2;
    if(zoom > 2*DT_LIBRARY_MAX_ZOOM) zoom = 2*DT_LIBRARY_MAX_ZOOM;
    DT_CTL_SET_GLOBAL(lib_zoom, zoom);
  }
  GtkWidget *range = glade_xml_get_widget (darktable.gui->main_window, "library_zoom");
  gtk_range_set_value(GTK_RANGE(range), zoom);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

void quit()
{
  // thread safe quit, 1st pass:
  pthread_mutex_lock(&darktable.control->cond_mutex);
  darktable.control->running = 0;
  pthread_mutex_unlock(&darktable.control->cond_mutex);
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
}

static gboolean
configure (GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  static int oldw = 0;
  static int oldh = 0;
  //make our selves a properly sized pixmap if our window has been resized
  if (oldw != event->width || oldh != event->height)
  {
    //create our new pixmap with the correct size.
    GdkPixmap *tmppixmap = gdk_pixmap_new(da->window, event->width,  event->height, -1);
    //copy the contents of the old pixmap to the new pixmap.  This keeps ugly uninitialized
    //pixmaps from being painted upon resize
    int minw = oldw, minh = oldh;
    if(event->width  < minw) minw = event->width;
    if(event->height < minh) minh = event->height;
    gdk_draw_drawable(tmppixmap, da->style->fg_gc[GTK_WIDGET_STATE(da)], darktable.gui->pixmap, 0, 0, 0, 0, minw, minh);
    //we're done with our old pixmap, so we can get rid of it and replace it with our properly-sized one.
    g_object_unref(darktable.gui->pixmap); 
    darktable.gui->pixmap = tmppixmap;
  }
  oldw = event->width;
  oldh = event->height;

  return dt_control_configure(da, event, user_data);
}

static void
zoom (GtkRange *range, gpointer user_data)
{
  GtkWidget *widget;
  int zoom;
  zoom = gtk_range_get_value(range);
  DT_CTL_SET_GLOBAL(lib_zoom, zoom);
  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
}

static void
gamma (GtkRange *range, gpointer user_data)
{
  if(darktable.gui->reset) return;
  if(user_data == (gpointer)0)
  {
    float linear = gtk_range_get_value(range);
    DT_CTL_SET_IMAGE(dev_gamma_linear, linear);
  }
  else
  {
    float gamma = gtk_range_get_value(range);
    DT_CTL_SET_IMAGE(dev_gamma_gamma, gamma);
  }
  dt_dev_add_history_item(darktable.develop, "gamma");
}

static gboolean
key_pressed (GtkWidget *w, GdkEventKey *event, gpointer user_data)
{
  return dt_control_key_pressed(event->hardware_keycode);
}

static gboolean
button_pressed (GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_control_button_pressed(event->x, event->y, event->button, event->type, event->state);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean
button_released (GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_control_button_released(event->x, event->y, event->button, event->state);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean
mouse_moved (GtkWidget *w, GdkEventMotion *event, gpointer user_data)
{
  dt_control_mouse_moved(event->x, event->y, event->state);
  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

gboolean center_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_leave();
  return TRUE;
}


int
dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[])
{
  GtkWidget *widget;

  if (!g_thread_supported ()) g_thread_init(NULL);
  gdk_threads_init();
  gdk_threads_enter();
  gtk_init (&argc, &argv);

  char path[1024];
  snprintf(path, 1023, "darktable.gtkrc");
  if(g_file_test(path, G_FILE_TEST_EXISTS)) gtk_rc_parse (path);
  else
  {
    snprintf(path, 1023, "%s/darktable.gtkrc", DATADIR);
    if(g_file_test(path, G_FILE_TEST_EXISTS)) gtk_rc_parse (path);
    else
    {
      fprintf(stderr, "[gtk_init] could not find darktable.gtkrc in . or %s!\n", DATADIR);
      exit(1);
    }
  }

  /* load the interface */
#ifdef DT_USE_GEGL
  snprintf(path, 1023, "darktable_gegl.glade");
#else
  snprintf(path, 1023, "darktable.glade");
#endif
  if(g_file_test(path, G_FILE_TEST_EXISTS)) darktable.gui->main_window = glade_xml_new (path, NULL, NULL);
  else
  {
#ifdef DT_USE_GEGL
    snprintf(path, 1023, "%s/darktable_gegl.glade", DATADIR);
#else
    snprintf(path, 1023, "%s/darktable.glade", DATADIR);
#endif
    if(g_file_test(path, G_FILE_TEST_EXISTS)) darktable.gui->main_window = glade_xml_new (path, NULL, NULL);
    else
    {
      fprintf(stderr, "[gtk_init] could not find darktable.glade in . or %s!\n", DATADIR);
      exit(1);
    }
  }

  /* connect the signals in the interface */

  widget = glade_xml_get_widget (darktable.gui->main_window, "button_import");
  g_signal_connect (G_OBJECT (widget), "clicked",
                    G_CALLBACK (import_button_clicked),
                    NULL);

  /* Have the delete event (window close) end the program */
  widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  g_signal_connect (G_OBJECT (widget), "delete_event",
                    G_CALLBACK (quit), NULL);
  g_signal_connect (G_OBJECT (widget), "key-press-event",
                    G_CALLBACK (key_pressed), NULL);

  gtk_widget_show_all(widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
 
	g_signal_connect (G_OBJECT (widget), "configure-event",
                    G_CALLBACK (configure), NULL);
	g_signal_connect (G_OBJECT (widget), "expose-event",
                    G_CALLBACK (expose), NULL);
	g_signal_connect (G_OBJECT (widget), "motion-notify-event",
                    G_CALLBACK (mouse_moved), NULL);
	g_signal_connect (G_OBJECT (widget), "leave-notify-event",
                    G_CALLBACK (center_leave), NULL);
	g_signal_connect (G_OBJECT (widget), "button-press-event",
                    G_CALLBACK (button_pressed), NULL);
	g_signal_connect (G_OBJECT (widget), "button-release-event",
                    G_CALLBACK (button_released), NULL);
	g_signal_connect (G_OBJECT (widget), "scroll-event",
                    G_CALLBACK (scrolled), NULL);
  // TODO: left, right, top, bottom:
  //leave-notify-event

  // lib zoom
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_zoom");
	g_signal_connect (G_OBJECT (widget), "value-changed",
                    G_CALLBACK (zoom), NULL);

  // gamma correction expander
  widget = glade_xml_get_widget (darktable.gui->main_window, "gamma_linear");
	g_signal_connect (G_OBJECT (widget), "value-changed",
                    G_CALLBACK (gamma), (gpointer)0);

  widget = glade_xml_get_widget (darktable.gui->main_window, "gamma_gamma");
	g_signal_connect (G_OBJECT (widget), "value-changed",
                    G_CALLBACK (gamma), (gpointer)1);

  // tone curve
  widget = glade_xml_get_widget (darktable.gui->main_window, "tonecurve");
  dt_gui_curve_editor_init(&gui->tonecurve, widget);

  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation");
  dt_gui_navigation_init(&gui->navigation, widget);

  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram");
  dt_gui_histogram_init(&gui->histogram, widget);

  // film history
  for(long int k=1;k<5;k++)
  {
    char wdname[20];
    snprintf(wdname, 20, "recent_film_%ld", k);
    widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    g_signal_connect (G_OBJECT (widget), "clicked",
                      G_CALLBACK (film_button_clicked),
                      (gpointer)(k-1));
  }

  // image op history
  for(long int k=0;k<10;k++)
  {
    char wdname[20];
    snprintf(wdname, 20, "history_%02ld", k);
    widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    g_signal_connect (G_OBJECT (widget), "clicked",
                      G_CALLBACK (history_button_clicked),
                      (gpointer)k);
  }

  widget = glade_xml_get_widget (darktable.gui->main_window, "export_button");
  g_signal_connect (G_OBJECT (widget), "clicked",
                    G_CALLBACK (export_button_clicked),
                    (gpointer)0);

  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  GTK_WIDGET_UNSET_FLAGS (widget, GTK_DOUBLE_BUFFERED);
  // GTK_WIDGET_SET_FLAGS (widget, GTK_DOUBLE_BUFFERED);
  GTK_WIDGET_SET_FLAGS   (widget, GTK_APP_PAINTABLE);

  darktable.gui->reset = 0;
#ifndef DT_USE_GEGL
  dt_iop_gui_init();
#else
  // TODO: need anything here? modules are inited anyways when they are needed.
#endif
  return 0;
}

void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui)
{
  dt_gui_curve_editor_cleanup(&gui->tonecurve);
  dt_gui_navigation_cleanup(&gui->navigation);
  dt_gui_histogram_cleanup(&gui->histogram);
}

void dt_gui_gtk_run(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  darktable.gui->pixmap = gdk_pixmap_new(widget->window, widget->allocation.width, widget->allocation.height, -1);
  /* start the event loop */
  gtk_main ();
  gdk_threads_leave();
}


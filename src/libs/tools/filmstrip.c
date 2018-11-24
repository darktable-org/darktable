/*
    This file is part of darktable,
    copyright (c) 2011-2012 Henrik Andersson.

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

#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/hist_dialog.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef enum dt_lib_filmstrip_select_t
{
  DT_LIB_FILMSTRIP_SELECT_NONE,
  DT_LIB_FILMSTRIP_SELECT_SINGLE,
  DT_LIB_FILMSTRIP_SELECT_TOGGLE,
  DT_LIB_FILMSTRIP_SELECT_RANGE
} dt_lib_filmstrip_select_t;

typedef struct dt_lib_filmstrip_t
{
  GtkWidget *filmstrip;

  /* state vars */
  int32_t last_selected_id;
  int32_t mouse_over_id;
  int32_t offset;
  int32_t collection_count;
  int32_t history_copy_imgid;
  gdouble pointerx, pointery;
  dt_view_image_over_t image_over;

  gboolean size_handle_is_dragging;
  gint size_handle_x, size_handle_y;
  int32_t size_handle_height;

  int32_t activated_image;
  dt_lib_filmstrip_select_t select;
  int32_t select_id;

  dt_gui_hist_dialog_t dg;
} dt_lib_filmstrip_t;

/* proxy function to center filmstrip on imgid */
static void _lib_filmstrip_scroll_to_image(dt_lib_module_t *self, gint imgid, gboolean activate);
/* proxy function for retrieving last activate request image id */
static int32_t _lib_filmstrip_get_activated_imgid(dt_lib_module_t *self);
static GtkWidget *_lib_filmstrip_get_widget(dt_lib_module_t *self);

static gboolean _lib_filmstrip_size_handle_button_callback(GtkWidget *w, GdkEventButton *e,
                                                           gpointer user_data);
static gboolean _lib_filmstrip_size_handle_motion_notify_callback(GtkWidget *w, GdkEventButton *e,
                                                                  gpointer user_data);
static gboolean _lib_filmstrip_size_handle_cursor_callback(GtkWidget *w, GdkEventCrossing *e,
                                                           gpointer user_data);

/* motion notify event handler */
static gboolean _lib_filmstrip_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, gpointer user_data);
/* motion leave event handler */
static gboolean _lib_filmstrip_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data);
/* scroll event */
static gboolean _lib_filmstrip_scroll_callback(GtkWidget *w, GdkEventScroll *e, gpointer user_data);
/* expose function for filmstrip module */
static gboolean _lib_filmstrip_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data);
/* button press callback */
static gboolean _lib_filmstrip_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data);
/* button release callback */
static gboolean _lib_filmstrip_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                       gpointer user_data);
/* signal callback for collection change */
static void _lib_filmstrip_collection_changed_callback(gpointer instance, gpointer user_data);

/* key accelerators callback */
static gboolean _lib_filmstrip_copy_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                               GObject *aceeleratable, guint keyval,
                                                               GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_copy_history_parts_key_accel_callback(GtkAccelGroup *accel_group,
                                                                     GObject *aceeleratable, guint keyval,
                                                                     GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_paste_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                                GObject *aceeleratable, guint keyval,
                                                                GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_paste_history_parts_key_accel_callback(GtkAccelGroup *accel_group,
                                                                      GObject *aceeleratable, guint keyval,
                                                                      GdkModifierType modifier,
                                                                      gpointer data);
static gboolean _lib_filmstrip_discard_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                                  GObject *aceeleratable, guint keyval,
                                                                  GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_duplicate_image_key_accel_callback(GtkAccelGroup *accel_group,
                                                                  GObject *aceeleratable, guint keyval,
                                                                  GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_ratings_key_accel_callback(GtkAccelGroup *accel_group, GObject *aceeleratable,
                                                          guint keyval, GdkModifierType modifier,
                                                          gpointer data);
static gboolean _lib_filmstrip_colorlabels_key_accel_callback(GtkAccelGroup *accel_group,
                                                              GObject *aceeleratable, guint keyval,
                                                              GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_select_key_accel_callback(GtkAccelGroup *accel_group, GObject *aceeleratable,
                                                         guint keyval, GdkModifierType modifier,
                                                         gpointer data);

/* drag'n'drop callbacks */
static void _lib_filmstrip_dnd_get_callback(GtkWidget *widget, GdkDragContext *context,
                                            GtkSelectionData *selection_data, guint target_type, guint time,
                                            gpointer user_data);
static void _lib_filmstrip_dnd_begin_callback(GtkWidget *widget, GdkDragContext *context, gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("filmstrip");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", "tethering", "map", "print", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

void init_key_accels(dt_lib_module_t *self)
{
  /* setup rating key accelerators */
  dt_accel_register_lib(self, NC_("accel", "rate 0"), GDK_KEY_0, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 1"), GDK_KEY_1, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 2"), GDK_KEY_2, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 3"), GDK_KEY_3, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 4"), GDK_KEY_4, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 5"), GDK_KEY_5, 0);
  dt_accel_register_lib(self, NC_("accel", "rate reject"), GDK_KEY_r, 0);


  /* setup history key accelerators */
  dt_accel_register_lib(self, NC_("accel", "copy history"), GDK_KEY_c, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "copy history parts"), GDK_KEY_c,
                        GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib(self, NC_("accel", "paste history"), GDK_KEY_v, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "paste history parts"), GDK_KEY_v,
                        GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib(self, NC_("accel", "discard history"), 0, 0);

  dt_accel_register_lib(self, NC_("accel", "duplicate image"), GDK_KEY_d, GDK_CONTROL_MASK);


  /* setup color label accelerators */
  dt_accel_register_lib(self, NC_("accel", "color red"), GDK_KEY_F1, 0);
  dt_accel_register_lib(self, NC_("accel", "color yellow"), GDK_KEY_F2, 0);
  dt_accel_register_lib(self, NC_("accel", "color green"), GDK_KEY_F3, 0);
  dt_accel_register_lib(self, NC_("accel", "color blue"), GDK_KEY_F4, 0);
  dt_accel_register_lib(self, NC_("accel", "color purple"), GDK_KEY_F5, 0);

  /* setup selection accelerators */
  dt_accel_register_lib(self, NC_("accel", "select all"), GDK_KEY_a, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "select none"), GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib(self, NC_("accel", "invert selection"), GDK_KEY_i, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "select film roll"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "select untouched"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{

  // Rating accels
  dt_accel_connect_lib(self, "rate 0", g_cclosure_new(G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
                                                      GINT_TO_POINTER(DT_VIEW_DESERT), NULL));
  dt_accel_connect_lib(self, "rate 1", g_cclosure_new(G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
                                                      GINT_TO_POINTER(DT_VIEW_STAR_1), NULL));
  dt_accel_connect_lib(self, "rate 2", g_cclosure_new(G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
                                                      GINT_TO_POINTER(DT_VIEW_STAR_2), NULL));
  dt_accel_connect_lib(self, "rate 3", g_cclosure_new(G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
                                                      GINT_TO_POINTER(DT_VIEW_STAR_3), NULL));
  dt_accel_connect_lib(self, "rate 4", g_cclosure_new(G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
                                                      GINT_TO_POINTER(DT_VIEW_STAR_4), NULL));
  dt_accel_connect_lib(self, "rate 5", g_cclosure_new(G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
                                                      GINT_TO_POINTER(DT_VIEW_STAR_5), NULL));
  dt_accel_connect_lib(self, "rate reject",
                       g_cclosure_new(G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
                                      GINT_TO_POINTER(DT_VIEW_REJECT), NULL));

  // History key accels
  dt_accel_connect_lib(
      self, "copy history",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_copy_history_key_accel_callback), (gpointer)self->data, NULL));
  dt_accel_connect_lib(self, "copy history parts",
                       g_cclosure_new(G_CALLBACK(_lib_filmstrip_copy_history_parts_key_accel_callback),
                                      (gpointer)self->data, NULL));
  dt_accel_connect_lib(self, "paste history",
                       g_cclosure_new(G_CALLBACK(_lib_filmstrip_paste_history_key_accel_callback),
                                      (gpointer)self->data, NULL));
  dt_accel_connect_lib(self, "paste history parts",
                       g_cclosure_new(G_CALLBACK(_lib_filmstrip_paste_history_parts_key_accel_callback),
                                      (gpointer)self->data, NULL));
  dt_accel_connect_lib(self, "discard history",
                       g_cclosure_new(G_CALLBACK(_lib_filmstrip_discard_history_key_accel_callback),
                                      (gpointer)self->data, NULL));

  dt_accel_connect_lib(self, "duplicate image",
                       g_cclosure_new(G_CALLBACK(_lib_filmstrip_duplicate_image_key_accel_callback),
                                      (gpointer)self->data, NULL));

  // Color label accels
  dt_accel_connect_lib(
      self, "color red",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback), GINT_TO_POINTER(0), NULL));
  dt_accel_connect_lib(
      self, "color yellow",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback), GINT_TO_POINTER(1), NULL));
  dt_accel_connect_lib(
      self, "color green",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback), GINT_TO_POINTER(2), NULL));
  dt_accel_connect_lib(
      self, "color blue",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback), GINT_TO_POINTER(3), NULL));
  dt_accel_connect_lib(
      self, "color purple",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback), GINT_TO_POINTER(4), NULL));

  // Selection accels
  dt_accel_connect_lib(self, "select all", g_cclosure_new(G_CALLBACK(_lib_filmstrip_select_key_accel_callback),
                                                          GINT_TO_POINTER(0), NULL));
  dt_accel_connect_lib(
      self, "select none",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_select_key_accel_callback), GINT_TO_POINTER(1), NULL));
  dt_accel_connect_lib(
      self, "invert selection",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_select_key_accel_callback), GINT_TO_POINTER(2), NULL));
  dt_accel_connect_lib(
      self, "select film roll",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_select_key_accel_callback), GINT_TO_POINTER(3), NULL));
  dt_accel_connect_lib(
      self, "select untouched",
      g_cclosure_new(G_CALLBACK(_lib_filmstrip_select_key_accel_callback), GINT_TO_POINTER(4), NULL));
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_filmstrip_t *d = (dt_lib_filmstrip_t *)calloc(1, sizeof(dt_lib_filmstrip_t));
  self->data = (void *)d;

  d->last_selected_id = -1;
  d->history_copy_imgid = -1;
  d->activated_image = -1;
  d->mouse_over_id = -1;
  d->pointerx = -1;
  d->pointery = -1;
  dt_gui_hist_dialog_init(&d->dg);

  /* creating drawing area */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);


  /* creating filmstrip box*/
  d->filmstrip = gtk_event_box_new();

  /* allow drag&drop of images from the filmstrip. this has to come before the other callbacks are registered!
   */
  gtk_drag_source_set(d->filmstrip, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_COPY);
#ifdef HAVE_MAP
  gtk_drag_dest_set(d->filmstrip, GTK_DEST_DEFAULT_ALL, target_list_internal, n_targets_internal,
                    GDK_ACTION_COPY);
#endif

  g_signal_connect_after(d->filmstrip, "drag-begin", G_CALLBACK(_lib_filmstrip_dnd_begin_callback), self);
  g_signal_connect(d->filmstrip, "drag-data-get", G_CALLBACK(_lib_filmstrip_dnd_get_callback), self);

  gtk_widget_add_events(d->filmstrip, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                      | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                      | darktable.gui->scroll_mask
                                      | GDK_LEAVE_NOTIFY_MASK);

  /* connect callbacks */
  g_signal_connect(G_OBJECT(d->filmstrip), "draw", G_CALLBACK(_lib_filmstrip_draw_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "button-press-event",
                   G_CALLBACK(_lib_filmstrip_button_press_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "button-release-event",
                   G_CALLBACK(_lib_filmstrip_button_release_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "scroll-event", G_CALLBACK(_lib_filmstrip_scroll_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "motion-notify-event",
                   G_CALLBACK(_lib_filmstrip_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(d->filmstrip), "leave-notify-event",
                   G_CALLBACK(_lib_filmstrip_mouse_leave_callback), self);


  /* set size of filmstrip */
  int32_t height = dt_conf_get_int("plugins/lighttable/filmstrip/height");
  gtk_widget_set_size_request(d->filmstrip, -1,
                              CLAMP(height, DT_PIXEL_APPLY_DPI(64), DT_PIXEL_APPLY_DPI(400)));

  /* create the resize handle */
  GtkWidget *size_handle = gtk_event_box_new();
  gtk_widget_set_size_request(size_handle, -1, DT_PIXEL_APPLY_DPI(10));
  gtk_widget_add_events(size_handle, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                     | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                     | GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect(G_OBJECT(size_handle), "button-press-event",
                   G_CALLBACK(_lib_filmstrip_size_handle_button_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "button-release-event",
                   G_CALLBACK(_lib_filmstrip_size_handle_button_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "motion-notify-event",
                   G_CALLBACK(_lib_filmstrip_size_handle_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "leave-notify-event",
                   G_CALLBACK(_lib_filmstrip_size_handle_cursor_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "enter-notify-event",
                   G_CALLBACK(_lib_filmstrip_size_handle_cursor_callback), self);


  gtk_box_pack_start(GTK_BOX(self->widget), size_handle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->filmstrip, FALSE, FALSE, 0);


  /* initialize view manager proxy */
  darktable.view_manager->proxy.filmstrip.module = self;
  darktable.view_manager->proxy.filmstrip.scroll_to_image = _lib_filmstrip_scroll_to_image;
  darktable.view_manager->proxy.filmstrip.activated_image = _lib_filmstrip_get_activated_imgid;
  darktable.view_manager->proxy.filmstrip.widget = _lib_filmstrip_get_widget;

  /* connect signal handler */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_filmstrip_collection_changed_callback), (gpointer)self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_lib_filmstrip_collection_changed_callback), (gpointer)self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect from signals */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_filmstrip_collection_changed_callback),
                               (gpointer)self);

  /* unset viewmanager proxy */
  darktable.view_manager->proxy.filmstrip.module = NULL;

  /* cleanup */
  free(self->data);
  self->data = NULL;
}


static gboolean _lib_filmstrip_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  dt_control_set_mouse_over_id(strip->activated_image);

  /* suppress mouse over highlight upon leave */
  strip->pointery = -1;
  gtk_widget_queue_draw(self->widget);

  return TRUE;
}

static gboolean _lib_filmstrip_size_handle_cursor_callback(GtkWidget *w, GdkEventCrossing *e,
                                                           gpointer user_data)
{
  dt_control_change_cursor((e->type == GDK_ENTER_NOTIFY) ? GDK_SB_V_DOUBLE_ARROW : GDK_LEFT_PTR);
  return TRUE;
}

static gboolean _lib_filmstrip_size_handle_button_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *d = (dt_lib_filmstrip_t *)self->data;

  if(e->button == 1)
  {
    if(e->type == GDK_BUTTON_PRESS)
    {
      /* store current  mousepointer position */
#if GTK_CHECK_VERSION(3, 20, 0)
      gdk_window_get_device_position(e->window,
                                     gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(
                                         gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
                                     &d->size_handle_x, &d->size_handle_y, 0);
#else
      gdk_window_get_device_position(
          gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)),
          gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(
              gdk_window_get_display(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
          &d->size_handle_x, &d->size_handle_y, NULL);
#endif

      gtk_widget_get_size_request(d->filmstrip, NULL, &d->size_handle_height);
      d->size_handle_is_dragging = TRUE;
    }
    else if(e->type == GDK_BUTTON_RELEASE)
      d->size_handle_is_dragging = FALSE;
  }
  return TRUE;
}

static gboolean _lib_filmstrip_size_handle_motion_notify_callback(GtkWidget *w, GdkEventButton *e,
                                                                  gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *d = (dt_lib_filmstrip_t *)self->data;
  if(d->size_handle_is_dragging)
  {
    gint x, y, sx, sy;
#if GTK_CHECK_VERSION(3, 20, 0)
    gdk_window_get_device_position(e->window,
        gdk_seat_get_pointer(gdk_display_get_default_seat(
            gdk_window_get_display(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
        &x, &y, 0);
#else
    gdk_window_get_device_position(
        gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)),
        gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(
            gdk_window_get_display(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
        &x, &y, NULL);
#endif

    gtk_widget_get_size_request(d->filmstrip, &sx, &sy);
    sy = CLAMP(d->size_handle_height + (d->size_handle_y - y), DT_PIXEL_APPLY_DPI(64),
               DT_PIXEL_APPLY_DPI(400));

    dt_conf_set_int("plugins/lighttable/filmstrip/height", sy);

    gtk_widget_set_size_request(d->filmstrip, -1, sy);

    return TRUE;
  }

  return FALSE;
}

static gboolean _lib_filmstrip_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  strip->pointerx = e->x;
  strip->pointery = e->y;

  /* redraw */
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean _lib_filmstrip_scroll_callback(GtkWidget *w, GdkEventScroll *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  /* change the offset */
  int delta_x, delta_y;
  if(dt_gui_get_scroll_unit_deltas(e, &delta_x, &delta_y))
  {
    strip->offset = CLAMP(strip->offset + delta_x + delta_y, 0, strip->collection_count-1);
    gtk_widget_queue_draw(self->widget);
  }

  return TRUE;
}

static gboolean _lib_filmstrip_imgid_in_collection(const dt_collection_t *collection, const int imgid)
{
  sqlite3_stmt *stmt;
  gboolean image_in_collection = TRUE;
  const char *query = dt_collection_get_query(collection);

  char *count_query = g_strdup_printf("SELECT count(id) FROM (%s) WHERE id = ?3", query);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), count_query, -1, &stmt, NULL);
  if((collection->params.query_flags & COLLECTION_QUERY_USE_LIMIT)
     && !(collection->params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT))
  {
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
  }
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
    image_in_collection = (sqlite3_column_int(stmt, 0) > 0);
  sqlite3_finalize(stmt);
  g_free(count_query);
  return image_in_collection;
}

static gboolean _lib_filmstrip_button_press_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  int32_t mouse_over_id = strip->mouse_over_id;
  strip->select = DT_LIB_FILMSTRIP_SELECT_NONE;

  if(e->button == 1)
  {
    if(e->type == GDK_BUTTON_PRESS)
    {
      /* let check if any thumb controls was clicked */
      switch(strip->image_over)
      {
        case DT_VIEW_DESERT:
          /* is this an activation of image */
          if((e->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == 0)
            strip->select = DT_LIB_FILMSTRIP_SELECT_SINGLE;
          else if((e->state & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
            strip->select = DT_LIB_FILMSTRIP_SELECT_TOGGLE;
          else if((e->state & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
            strip->select = DT_LIB_FILMSTRIP_SELECT_RANGE;
          if(strip->select != DT_LIB_FILMSTRIP_SELECT_NONE)
          {
            strip->select_id = mouse_over_id;
            return TRUE;
          }
          break;
        case DT_VIEW_REJECT:
        case DT_VIEW_STAR_1:
        case DT_VIEW_STAR_2:
        case DT_VIEW_STAR_3:
        case DT_VIEW_STAR_4:
        case DT_VIEW_STAR_5:
        {
          int offset = 0;
          if(mouse_over_id == strip->activated_image) offset = dt_collection_image_offset(mouse_over_id);

          dt_ratings_apply_to_image_or_group(mouse_over_id, strip->image_over);

          if(mouse_over_id == strip->activated_image)
            if(_lib_filmstrip_imgid_in_collection(darktable.collection, mouse_over_id) == 0)
              dt_view_filmstrip_scroll_relative(0, offset);

          gtk_widget_queue_draw(darktable.view_manager->proxy.filmstrip.module->widget);
          return TRUE;
        }

        default:
          return FALSE;
      }
    }
    else if(e->type == GDK_2BUTTON_PRESS)
    {
      if(mouse_over_id > 0)
      {
        strip->activated_image = mouse_over_id;
        dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE);
        return TRUE;
      }
    }
  }

  return FALSE;
}

static gboolean _lib_filmstrip_button_release_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  int32_t mouse_over_id = strip->mouse_over_id;
  int32_t select_id = strip->select_id;
  gboolean result = FALSE;

  if(mouse_over_id == select_id && mouse_over_id > 0)
  {
    result = TRUE;
    if(strip->select == DT_LIB_FILMSTRIP_SELECT_SINGLE)
      dt_selection_select_single(darktable.selection, mouse_over_id);
    else if(strip->select == DT_LIB_FILMSTRIP_SELECT_TOGGLE)
      dt_selection_toggle(darktable.selection, mouse_over_id);
    else if(strip->select == DT_LIB_FILMSTRIP_SELECT_RANGE)
      dt_selection_select_range(darktable.selection, mouse_over_id);
    else
      result = FALSE;
  }

  strip->select = DT_LIB_FILMSTRIP_SELECT_NONE;
  strip->select_id = -1;

  /* redraw filmstrip */
  if(result && darktable.view_manager->proxy.filmstrip.module)
    gtk_widget_queue_draw(darktable.view_manager->proxy.filmstrip.module->widget);

  return result;
}

static gboolean _lib_filmstrip_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int32_t width = allocation.width;
  int32_t height = allocation.height;

  gdouble pointerx = strip->pointerx;
  gdouble pointery = strip->pointery;

  if(darktable.gui->center_tooltip == 1) darktable.gui->center_tooltip++;

  int mouse_over_id = -1;
  strip->image_over = DT_VIEW_DESERT;

  /* fill background */
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  int offset = strip->offset;

  const float wd = height;
  const float ht = height;

  int max_cols = (int)(width / (float)wd) + 2;
  if(max_cols % 2 == 0) max_cols += 1;

  const int col_start = max_cols / 2 - strip->offset;
  const int empty_edge = (width - (max_cols * wd)) / 2;
  int step_res = SQLITE_ROW;

  sqlite3_stmt *stmt = NULL;

  /* mouse over image position in filmstrip */
  pointerx -= empty_edge;
  const int seli = (pointery > 0 && pointery <= ht) ? pointerx / (float)wd : -1;
  const int img_pointerx = (int)fmodf(pointerx, wd);
  const int img_pointery = (int)pointery;


  /* get the count of current collection */
  strip->collection_count = dt_collection_get_count(darktable.collection);

  /* get the collection query */
  const gchar *query = dt_collection_get_query(darktable.collection);
  if(!query) return FALSE;

  if(offset < 0) strip->offset = offset = 0;
  if(offset > strip->collection_count - 1) strip->offset = offset = strip->collection_count - 1;

  // dt_view_set_scrollbar(self, offset, count, max_cols, 0, 1, 1);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset - max_cols / 2);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_cols);


  cairo_save(cr);
  cairo_translate(cr, empty_edge, 0.0f);
  for(int col = 0; col < max_cols; col++)
  {
    if(col < col_start)
    {
      cairo_translate(cr, wd, 0.0f);
      continue;
    }

    if(step_res != SQLITE_DONE)
    {
      step_res = sqlite3_step(stmt);
    }

    if(step_res == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
      // set mouse over id
      if(seli == col)
      {
        strip->mouse_over_id = id;
        mouse_over_id = id;
      }
      cairo_save(cr);
      // FIXME find out where the y translation is done, how big the value is and use it directly instead of
      // getting it from the matrix ...
      cairo_matrix_t m;
      cairo_get_matrix(cr, &m);
      dt_view_image_expose(&(strip->image_over), id, cr, wd, ht, max_cols, img_pointerx, img_pointery, FALSE, FALSE);
      cairo_restore(cr);
    }
    else if(step_res == SQLITE_DONE)
    {
      /* do nothing, just add some empty thumb frames */
    }
    else
      goto failure;
    cairo_translate(cr, wd, 0.0f);
  }
failure:
  cairo_restore(cr);
  sqlite3_finalize(stmt);

  // don't reset the global mouse_over_id when the cursor isn't even over the filmstrip
  if(pointerx >= 0 && pointery >= 0)
    dt_control_set_mouse_over_id(mouse_over_id);

  if(darktable.gui->center_tooltip == 1) // set in this round
  {
    char *tooltip = dt_history_get_items_as_string(strip->mouse_over_id);
    if(tooltip != NULL)
    {
      gtk_widget_set_tooltip_text(strip->filmstrip, tooltip);
      g_free(tooltip);
    }
  }
  else if(darktable.gui->center_tooltip == 2) // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    gtk_widget_set_tooltip_text(strip->filmstrip, "");
  }

#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
#endif

  return TRUE;
}

static void _lib_filmstrip_collection_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_control_queue_redraw_widget(self->widget);
}

static void _lib_filmstrip_scroll_to_image(dt_lib_module_t *self, gint imgid, gboolean activate)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  /* if no imgid just bail out */
  if(imgid <= 0) return;

  strip->activated_image = imgid;

  strip->offset = dt_collection_image_offset(imgid);

  dt_control_set_mouse_over_id(strip->activated_image);

  /* activate the image if requested */
  if(activate)
  {
    strip->activated_image = imgid;
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE);
  }

  /* redraw filmstrip. since this is a proxy function it could be used from another thread */
  dt_control_queue_redraw_widget(self->widget);
}

static int32_t _lib_filmstrip_get_activated_imgid(dt_lib_module_t *self)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;
  return strip->activated_image;
}

static GtkWidget *_lib_filmstrip_get_widget(dt_lib_module_t *self)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;
  return strip->filmstrip;
}

static gboolean _lib_filmstrip_copy_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                               GObject *aceeleratable, guint keyval,
                                                               GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id <= 0) return FALSE;
  strip->history_copy_imgid = mouse_over_id;
  strip->dg.selops = NULL;

  /* check if images is currently loaded in darkroom */
  if(dt_dev_is_current_image(darktable.develop, mouse_over_id)) dt_dev_write_history(darktable.develop);
  return TRUE;
}

static gboolean _lib_filmstrip_copy_history_parts_key_accel_callback(GtkAccelGroup *accel_group,
                                                                     GObject *aceeleratable, guint keyval,
                                                                     GdkModifierType modifier, gpointer data)
{
  if(_lib_filmstrip_copy_history_key_accel_callback(accel_group, aceeleratable, keyval, modifier, data))
  {
    dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
    if(dt_gui_hist_dialog_new(&(strip->dg), strip->history_copy_imgid, TRUE) == GTK_RESPONSE_CANCEL)
      return FALSE;
    return TRUE;
  }
  else
    return FALSE;
}

static gboolean _lib_filmstrip_paste_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                                GObject *aceeleratable, guint keyval,
                                                                GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  int mode = dt_conf_get_int("plugins/lighttable/copy_history/pastemode");

  if(dt_history_copy_and_paste_on_selection(strip->history_copy_imgid, (mode == 0) ? TRUE : FALSE,
                                            strip->dg.selops) != 0)
  {
    int32_t mouse_over_id = dt_control_get_mouse_over_id();
    if(mouse_over_id <= 0) return FALSE;

    dt_history_copy_and_paste_on_image(strip->history_copy_imgid, mouse_over_id, (mode == 0) ? TRUE : FALSE,
                                       strip->dg.selops);
  }

  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_filmstrip_paste_history_parts_key_accel_callback(GtkAccelGroup *accel_group,
                                                                      GObject *aceeleratable, guint keyval,
                                                                      GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  int mode = dt_conf_get_int("plugins/lighttable/copy_history/pastemode");

  // get mouse over before launching the dialog
  int32_t mouse_over_id = dt_control_get_mouse_over_id();

  int res = dt_gui_hist_dialog_new(&(strip->dg), strip->history_copy_imgid, FALSE);
  if(res == GTK_RESPONSE_CANCEL) return FALSE;

  if(dt_history_copy_and_paste_on_selection(strip->history_copy_imgid, (mode == 0) ? TRUE : FALSE,
                                            strip->dg.selops) != 0)
  {
    if(mouse_over_id <= 0) return FALSE;

    dt_history_copy_and_paste_on_image(strip->history_copy_imgid, mouse_over_id, (mode == 0) ? TRUE : FALSE,
                                       strip->dg.selops);
  }

  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_filmstrip_discard_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                                  GObject *aceeleratable, guint keyval,
                                                                  GdkModifierType modifier, gpointer data)
{
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id <= 0) return FALSE;

  dt_history_delete_on_image(mouse_over_id);
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_filmstrip_duplicate_image_key_accel_callback(GtkAccelGroup *accel_group,
                                                                  GObject *aceeleratable, guint keyval,
                                                                  GdkModifierType modifier, gpointer data)
{
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id <= 0) return FALSE;

  /* check if images is currently loaded in darkroom */
  if(dt_dev_is_current_image(darktable.develop, mouse_over_id)) dt_dev_write_history(darktable.develop);

  int32_t newimgid = dt_image_duplicate(mouse_over_id);
  if(newimgid != -1) dt_history_copy_and_paste_on_image(mouse_over_id, newimgid, FALSE, NULL);

  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_filmstrip_ratings_key_accel_callback(GtkAccelGroup *accel_group, GObject *aceeleratable,
                                                          guint keyval, GdkModifierType modifier,
                                                          gpointer data)
{
  int num = GPOINTER_TO_INT(data);
  switch(num)
  {
    case DT_VIEW_DESERT:
    case DT_VIEW_REJECT:
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
    case 666:
    {
      int32_t mouse_over_id = dt_control_get_mouse_over_id();
      if(mouse_over_id <= 0) return FALSE;
      /* get image from cache */

      int32_t activated_image = -1;

      activated_image = darktable.view_manager->proxy.filmstrip.activated_image(
          darktable.view_manager->proxy.filmstrip.module);

      int offset = 0;
      if(mouse_over_id == activated_image) offset = dt_collection_image_offset(mouse_over_id);

      dt_ratings_apply_to_image_or_group(mouse_over_id, num);

      dt_collection_hint_message(darktable.collection); // More than this, we need to redraw all

      if(mouse_over_id == activated_image)
        if(_lib_filmstrip_imgid_in_collection(darktable.collection, mouse_over_id) == 0)
          dt_view_filmstrip_scroll_relative(0, offset);

      /* redraw all */
      dt_control_queue_redraw();
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static gboolean _lib_filmstrip_colorlabels_key_accel_callback(GtkAccelGroup *accel_group,
                                                              GObject *acceleratable, guint keyval,
                                                              GdkModifierType modifier, gpointer data)
{
  dt_colorlabels_key_accel_callback(NULL, NULL, 0, 0, data);
  /* redraw filmstrip */
  if(darktable.view_manager->proxy.filmstrip.module)
    gtk_widget_queue_draw(darktable.view_manager->proxy.filmstrip.module->widget);
  return TRUE;
}

static gboolean _lib_filmstrip_select_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                         guint keyval, GdkModifierType modifier,
                                                         gpointer data)
{
  switch(GPOINTER_TO_INT(data))
  {
    case 0: // all
      dt_selection_select_all(darktable.selection);
      break;
    case 1: // none
      dt_selection_clear(darktable.selection);
      break;
    case 2: // invert
      dt_selection_invert(darktable.selection);
      break;
    case 4: // untouched
      dt_selection_select_unaltered(darktable.selection);
      break;
    default: // case 3: same film roll
      dt_selection_select_filmroll(darktable.selection);
  }

  if(darktable.view_manager->proxy.filmstrip.module)
    gtk_widget_queue_draw(darktable.view_manager->proxy.filmstrip.module->widget);

  return TRUE;
}

static void _lib_filmstrip_dnd_get_callback(GtkWidget *widget, GdkDragContext *context,
                                            GtkSelectionData *selection_data, guint target_type, guint time,
                                            gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  g_assert(selection_data != NULL);

  int mouse_over_id = strip->mouse_over_id;
  int count = dt_collection_get_selected_count(NULL);
  switch(target_type)
  {
    case DND_TARGET_IMGID:
    {
      int id = ((count == 1) ? mouse_over_id : -1);
      gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _DWORD,
                             (guchar *)&id, sizeof(id));
      break;
    }
    default:             // return the location of the file as a last resort
    case DND_TARGET_URI: // TODO: add all images from the selection
    {
      if(count == 1)
      {
        gchar pathname[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        dt_image_full_path(mouse_over_id, pathname, sizeof(pathname), &from_cache);
        gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri, strlen(uri));
        g_free(uri);
      }
      else
      {
        sqlite3_stmt *stmt;
        GList *images = NULL;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT imgid FROM main.selected_images", -1, &stmt, NULL);
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          int id = sqlite3_column_int(stmt, 0);
          gchar pathname[PATH_MAX] = { 0 };
          gboolean from_cache = TRUE;
          dt_image_full_path(id, pathname, sizeof(pathname), &from_cache);
          gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
          images = g_list_append(images, uri);
        }
        sqlite3_finalize(stmt);
        gchar *uri_list = dt_util_glist_to_str("\r\n", images);
        g_list_free_full(images, g_free);
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri_list, strlen(uri_list));
        g_free(uri_list);
      }
      break;
    }
  }
}

static void _lib_filmstrip_dnd_begin_callback(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  const int ts = DT_PIXEL_APPLY_DPI(64);

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  int imgid = strip->mouse_over_id;

  // imgid part of selection -> do nothing
  // otherwise               -> select the current image
  strip->select = DT_LIB_FILMSTRIP_SELECT_NONE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images WHERE imgid=?1 LIMIT 1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    dt_selection_select_single(darktable.selection, imgid);
    /* redraw filmstrip */
    if(darktable.view_manager->proxy.filmstrip.module)
      gtk_widget_queue_draw(darktable.view_manager->proxy.filmstrip.module->widget);
  }
  sqlite3_finalize(stmt);

  // if we are dragging a single image -> use the thumbnail of that image
  // otherwise use the generic d&d icon
  // TODO: have something pretty in the 2nd case, too.
  if(dt_collection_get_selected_count(NULL) == 1)
  {
    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r');

    if(buf.buf)
    {
      for(size_t i = 3; i < (size_t)4 * buf.width * buf.height; i += 4) buf.buf[i] = UINT8_MAX;

      int w = ts, h = ts;
      if(buf.width < buf.height)
        w = (buf.width * ts) / buf.height; // portrait
      else
        h = (buf.height * ts) / buf.width; // landscape

      GdkPixbuf *source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, buf.width,
                                                   buf.height, buf.width * 4, NULL, NULL);
      GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
      gtk_drag_set_icon_pixbuf(context, scaled, 0, h);

      if(source) g_object_unref(source);
      if(scaled) g_object_unref(scaled);
    }

    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

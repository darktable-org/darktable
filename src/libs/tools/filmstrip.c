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

#include "views/view.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_filmstrip_t
{
  GtkWidget *filmstrip;

  /* state vars */
  int32_t last_selected_id;
  int32_t mouse_over_id;
  int32_t offset;
  int32_t collection_count;
  int32_t history_copy_imgid;
  gdouble pointerx,pointery;
  dt_view_image_over_t image_over;

  gboolean size_handle_is_dragging;
  gint size_handle_x,size_handle_y;
  int32_t size_handle_height;

  int32_t activated_image;
}
dt_lib_filmstrip_t;

/* proxy function to center filmstrip on imgid */
static void _lib_filmstrip_scroll_to_image(dt_lib_module_t *self, gint imgid, gboolean activate);
/* proxy function for retrieving last activate request image id */
static int32_t _lib_filmstrip_get_activated_imgid(dt_lib_module_t *self);

static gboolean _lib_filmstrip_size_handle_button_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data);
static gboolean _lib_filmstrip_size_handle_motion_notify_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data);
static gboolean _lib_filmstrip_size_handle_cursor_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data);

/* motion notify event handler */
static gboolean _lib_filmstrip_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, gpointer user_data);
/* motion leave event handler */
static gboolean _lib_filmstrip_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data);
/* scroll event */
static gboolean _lib_filmstrip_scroll_callback(GtkWidget *w,GdkEventScroll *e, gpointer user_data);
/* expose function for filmstrip module */
static gboolean _lib_filmstrip_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
/* button press callback */
static gboolean _lib_filmstrip_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
/* signal callback for collection change */
static void _lib_filmstrip_collection_changed_callback(gpointer instance, gpointer user_data);

/* key accelerators callback */
static gboolean _lib_filmstrip_copy_history_key_accel_callback(GtkAccelGroup *accel_group,
                                GObject *aceeleratable, guint keyval,
                                GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_paste_history_key_accel_callback(GtkAccelGroup *accel_group,
                                GObject *aceeleratable, guint keyval,
                                GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_discard_history_key_accel_callback(GtkAccelGroup *accel_group,
                                GObject *aceeleratable, guint keyval,
                                GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_ratings_key_accel_callback(GtkAccelGroup *accel_group,
                                GObject *aceeleratable, guint keyval,
                                GdkModifierType modifier, gpointer data);
static gboolean _lib_filmstrip_colorlabels_key_accel_callback(GtkAccelGroup *accel_group,
                                GObject *aceeleratable, guint keyval,
                                GdkModifierType modifier, gpointer data);

const char* name()
{
  return _("filmstrip");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_BOTTOM;
}

int expandable() 
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
  dt_accel_register_lib(self, NC_("accel", "rate desert"), GDK_0, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 1"), GDK_1, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 2"), GDK_2, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 3"), GDK_3, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 4"), GDK_4, 0);
  dt_accel_register_lib(self, NC_("accel", "rate 5"), GDK_5, 0);
  dt_accel_register_lib(self, NC_("accel", "rate reject"), GDK_r, 0);

  
  /* setup history key accelerators */
  dt_accel_register_lib(self, NC_("accel", "copy history"),
                        GDK_c, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "paste history"),
                        GDK_v, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "discard history"),
                        GDK_d, GDK_CONTROL_MASK);

  
  
  /* setup color label accelerators */
  dt_accel_register_lib(self, NC_("accel", "color red"), GDK_F1, 0);
  dt_accel_register_lib(self, NC_("accel", "color yellow"), GDK_F2, 0);
  dt_accel_register_lib(self, NC_("accel", "color green"), GDK_F3, 0);
  dt_accel_register_lib(self, NC_("accel", "color blue"), GDK_F4, 0);
  dt_accel_register_lib(self, NC_("accel", "color purple"), GDK_F5, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{

  // Rating accels
  dt_accel_connect_lib(
      self, "rate desert",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
          (gpointer)DT_VIEW_DESERT,NULL));
  dt_accel_connect_lib(
      self, "rate 1",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
          (gpointer)DT_VIEW_STAR_1,NULL));
  dt_accel_connect_lib(
      self, "rate 2",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
          (gpointer)DT_VIEW_STAR_2,NULL));
  dt_accel_connect_lib(
      self, "rate 3",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
          (gpointer)DT_VIEW_STAR_3,NULL));
  dt_accel_connect_lib(
      self, "rate 4",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
          (gpointer)DT_VIEW_STAR_4,NULL));
  dt_accel_connect_lib(
      self, "rate 5",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
          (gpointer)DT_VIEW_STAR_5,NULL));
  dt_accel_connect_lib(
      self, "rate reject",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_ratings_key_accel_callback),
          (gpointer)DT_VIEW_REJECT,NULL));

  // History key accels
  dt_accel_connect_lib(
      self, "copy history",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_copy_history_key_accel_callback),
          (gpointer)self->data,NULL));
  dt_accel_connect_lib(
      self, "paste history",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_paste_history_key_accel_callback),
          (gpointer)self->data,NULL));
  dt_accel_connect_lib(
      self, "discard history",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_discard_history_key_accel_callback),
          (gpointer)self->data,NULL));

  // Color label accels
  dt_accel_connect_lib(
      self, "color red",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback),
          (gpointer)0,NULL));
  dt_accel_connect_lib(
      self, "color yellow",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback),
          (gpointer)1,NULL));
  dt_accel_connect_lib(
      self, "color green",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback),
          (gpointer)2,NULL));
  dt_accel_connect_lib(
      self, "color blue",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback),
          (gpointer)3,NULL));
  dt_accel_connect_lib(
      self, "color purple",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_colorlabels_key_accel_callback),
          (gpointer)4,NULL));
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_filmstrip_t *d = (dt_lib_filmstrip_t *)g_malloc(sizeof(dt_lib_filmstrip_t));
  self->data = (void *)d;
  memset(d,0,sizeof(dt_lib_filmstrip_t));

  d->last_selected_id = -1;
  d->history_copy_imgid = -1;
  d->activated_image = -1;
  d->mouse_over_id = -1;

  /* create drawingarea */
  self->widget = gtk_vbox_new(FALSE,0);
  
  
  /* createing filmstrip box*/
  d->filmstrip = gtk_event_box_new();

  gtk_widget_add_events(d->filmstrip, 
              GDK_POINTER_MOTION_MASK | 
              GDK_POINTER_MOTION_HINT_MASK | 
              GDK_BUTTON_PRESS_MASK | 
              GDK_BUTTON_RELEASE_MASK |
              GDK_SCROLL_MASK |
              GDK_LEAVE_NOTIFY_MASK);

  /* connect callbacks */
  g_signal_connect (G_OBJECT (d->filmstrip), "expose-event",
                    G_CALLBACK (_lib_filmstrip_expose_callback), self);
  g_signal_connect (G_OBJECT (d->filmstrip), "button-press-event",
                    G_CALLBACK (_lib_filmstrip_button_press_callback), self);
  g_signal_connect (G_OBJECT (d->filmstrip), "scroll-event",
                    G_CALLBACK (_lib_filmstrip_scroll_callback), self);
  g_signal_connect (G_OBJECT (d->filmstrip), "motion-notify-event",
                    G_CALLBACK(_lib_filmstrip_motion_notify_callback), self);
  g_signal_connect (G_OBJECT (d->filmstrip), "leave-notify-event",
                    G_CALLBACK(_lib_filmstrip_mouse_leave_callback), self);

  
  /* set size of filmstrip */
  int32_t height = dt_conf_get_int("plugins/lighttable/filmstrip/height");
  gtk_widget_set_size_request(d->filmstrip, -1, CLAMP(height,64,400));

  /* create the resize handle */
  GtkWidget *size_handle = gtk_event_box_new();
  gtk_widget_set_size_request(size_handle,-1,10);
  gtk_widget_add_events(size_handle, 
              GDK_POINTER_MOTION_MASK | 
              GDK_POINTER_MOTION_HINT_MASK | 
              GDK_BUTTON_PRESS_MASK | 
              GDK_BUTTON_RELEASE_MASK |
              GDK_ENTER_NOTIFY_MASK |
              GDK_LEAVE_NOTIFY_MASK
              );

  g_signal_connect (G_OBJECT (size_handle), "button-press-event",
                    G_CALLBACK (_lib_filmstrip_size_handle_button_callback), self);
  g_signal_connect (G_OBJECT (size_handle), "button-release-event",
                    G_CALLBACK (_lib_filmstrip_size_handle_button_callback), self);
  g_signal_connect (G_OBJECT (size_handle), "motion-notify-event",
                    G_CALLBACK (_lib_filmstrip_size_handle_motion_notify_callback), self);
  g_signal_connect (G_OBJECT (size_handle), "leave-notify-event",
                    G_CALLBACK(_lib_filmstrip_size_handle_cursor_callback), self);
  g_signal_connect (G_OBJECT (size_handle), "enter-notify-event",
                    G_CALLBACK(_lib_filmstrip_size_handle_cursor_callback), self);


  gtk_box_pack_start(GTK_BOX(self->widget), size_handle, FALSE, FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->filmstrip, FALSE, FALSE,0);


  /* initialize view manager proxy */
  darktable.view_manager->proxy.filmstrip.module = self;
  darktable.view_manager->proxy.filmstrip.scroll_to_image = _lib_filmstrip_scroll_to_image;
  darktable.view_manager->proxy.filmstrip.activated_image = _lib_filmstrip_get_activated_imgid;

  /* connect signal handler */
  dt_control_signal_connect(darktable.signals, 
                  DT_SIGNAL_COLLECTION_CHANGED,
                  G_CALLBACK(_lib_filmstrip_collection_changed_callback),
                  (gpointer)self);
  dt_control_signal_connect(darktable.signals, 
                  DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                  G_CALLBACK(_lib_filmstrip_collection_changed_callback),
                  (gpointer)self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect from signals */
  dt_control_signal_disconnect(darktable.signals,
                    G_CALLBACK(_lib_filmstrip_collection_changed_callback),
                    (gpointer)self);

  /* unset viewmanager proxy */
  darktable.view_manager->proxy.filmstrip.module = NULL;

  /* cleaup */
  g_free(self->data);
  self->data = NULL;
}


static gboolean _lib_filmstrip_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, strip->activated_image);

  /* suppress mouse over highlight upon leave */
  strip->pointery = -1;
  gtk_widget_queue_draw(self->widget);

  return TRUE;
}

static gboolean _lib_filmstrip_size_handle_cursor_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  dt_control_change_cursor( (e->type==GDK_ENTER_NOTIFY)?GDK_SB_V_DOUBLE_ARROW:GDK_LEFT_PTR);
  return TRUE;
}

static gboolean _lib_filmstrip_size_handle_button_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *d = (dt_lib_filmstrip_t *)self->data;

  if (e->button == 1)
  {
    if (e->type == GDK_BUTTON_PRESS)
    {
      /* store current  mousepointer position */
      gdk_window_get_pointer(dt_ui_main_window(darktable.gui->ui)->window, &d->size_handle_x, &d->size_handle_y, NULL);
      gtk_widget_get_size_request(d->filmstrip, NULL, &d->size_handle_height);
      d->size_handle_is_dragging = TRUE;
    }
    else if (e->type == GDK_BUTTON_RELEASE) 
      d->size_handle_is_dragging = FALSE;
  }
  return TRUE;
}

static gboolean _lib_filmstrip_size_handle_motion_notify_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *d = (dt_lib_filmstrip_t *)self->data;
  if (d->size_handle_is_dragging)
  {
    gint x,y,sx,sy;
    gdk_window_get_pointer (dt_ui_main_window(darktable.gui->ui)->window, &x, &y, NULL);
    gtk_widget_get_size_request (d->filmstrip,&sx,&sy);
    sy = CLAMP(d->size_handle_height+(d->size_handle_y - y), 64,400);

    dt_conf_set_int("plugins/lighttable/filmstrip/height", sy);

    gtk_widget_set_size_request(d->filmstrip,-1,sy);

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

static gboolean _lib_filmstrip_scroll_callback(GtkWidget *w,GdkEventScroll *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  /* change the offset */
  if (strip->offset > 0 && (e->direction == GDK_SCROLL_UP || e->direction == GDK_SCROLL_LEFT)) 
    strip->offset--;
  else if(strip->offset < strip->collection_count-1 && (e->direction == GDK_SCROLL_DOWN || e->direction == GDK_SCROLL_RIGHT)) 
    strip->offset++;
  else
    return TRUE;

  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean _lib_filmstrip_button_press_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  int32_t mouse_over_id = strip->mouse_over_id;

  /* is this an activation of image */
  if (e->button == 1 && e->type == GDK_2BUTTON_PRESS)
    if (mouse_over_id > 0)
    {
      strip->activated_image = mouse_over_id;
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE);
      return TRUE;
    }


  /* let check if any thumb controls was clicked */
  switch(strip->image_over)
  {
    case DT_VIEW_DESERT:
      break;
    case DT_VIEW_REJECT:
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
    {
      const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, mouse_over_id);
      dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);
      if(strip->image_over == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
      else if(strip->image_over == DT_VIEW_REJECT && ((image->flags & 0x7) == 6)) image->flags &= ~0x7;
      else
      {
        image->flags &= ~0x7;
        image->flags |= strip->image_over;
      }
      dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
      dt_image_cache_read_release(darktable.image_cache, image);
      break;
    }

    default:
      return FALSE;
  }

  return TRUE;
}

static gboolean _lib_filmstrip_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  int32_t width = widget->allocation.width;
  int32_t height = widget->allocation.height;

  gdouble pointerx = strip->pointerx;
  gdouble pointery = strip->pointery;

  if(darktable.gui->center_tooltip == 1)
    darktable.gui->center_tooltip++;

  strip->image_over = DT_VIEW_DESERT;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);

  /* create cairo surface */
  cairo_t *cr = gdk_cairo_create(widget->window); 

  /* fill background */
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  int offset = strip->offset;

  const float wd = height;
  const float ht = height;

  int max_cols = (int)(width/(float)wd) + 2;
  if (max_cols%2 == 0)
    max_cols += 1;

  const int col_start = max_cols/2 - strip->offset;
  const int empty_edge = (width - (max_cols * wd))/2;
  int step_res = SQLITE_ROW;

  sqlite3_stmt *stmt = NULL;

  /* mouse over image position in filmstrip */
  pointerx -= empty_edge;
  const int seli = (pointery > 0 && pointery <= ht) ? pointerx / (float)wd : -1;
  const int img_pointerx = (int)fmodf(pointerx, wd);
  const int img_pointery = (int)pointery;


  /* get the count of current collection */
  strip->collection_count = dt_collection_get_count (darktable.collection);

  /* get the collection query */
  const gchar *query=dt_collection_get_query (darktable.collection);
  if(!query)
    return FALSE;

  if(offset < 0)                
    strip->offset = offset = 0;
  if(offset > strip->collection_count-1) 
    strip->offset = offset = strip->collection_count-1;

  // dt_view_set_scrollbar(self, offset, count, max_cols, 0, 1, 1);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset - max_cols/2);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_cols);
  

  cairo_save(cr);
  cairo_translate(cr, empty_edge, 0.0f);
  for(int col = 0; col < max_cols; col++)
  {
    if(col < col_start) {
      cairo_translate(cr, wd, 0.0f);
      continue;
    }

    if(step_res != SQLITE_DONE) {
      step_res = sqlite3_step(stmt);
    }

    if(step_res == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
      // set mouse over id
      if(seli == col)
      {
        strip->mouse_over_id = id;
        DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, strip->mouse_over_id);
      }
      cairo_save(cr);
      // FIXME find out where the y translation is done, how big the value is and use it directly instead of getting it from the matrix ...
      cairo_matrix_t m;
      cairo_get_matrix(cr, &m);
      dt_view_image_expose(&(strip->image_over), id, cr, wd, ht, max_cols, img_pointerx, img_pointery);
      cairo_restore(cr);
    }
    else if (step_res == SQLITE_DONE) {
      /* do nothing, just add some empty thumb frames */
    }
    else goto failure;
    cairo_translate(cr, wd, 0.0f);
  }
failure:
  cairo_restore(cr);
  sqlite3_finalize(stmt);

  if(darktable.gui->center_tooltip == 1) // set in this round
  {
    char* tooltip = dt_history_get_items_as_string(strip->mouse_over_id);
    if(tooltip != NULL)
    {
      g_object_set(G_OBJECT(strip->filmstrip), "tooltip-text", tooltip, (char *)NULL);
      g_free(tooltip);
    }
  } else if(darktable.gui->center_tooltip == 2) // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    g_object_set(G_OBJECT(strip->filmstrip), "tooltip-text", "", (char *)NULL);
  }

#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
#endif

  /* cleanup */
  cairo_destroy(cr); 
    
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

  char query[1024];
  const gchar *qin = dt_collection_get_query (darktable.collection);
  if(qin)
  {
    snprintf(query, 1024, "select rowid from (%s) where id=?3", qin);
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1,  0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      strip->offset = sqlite3_column_int(stmt, 0) - 1;
    }
    sqlite3_finalize(stmt);
  }

  /* activate the image if requested */
  if (activate)
  {
    strip->activated_image = imgid;
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE);
  }

  /* redraw filmstrip */
  gboolean owns_lock = dt_control_gdk_lock();
  gtk_widget_queue_draw(self->widget);
  if(owns_lock) dt_control_gdk_unlock();
}

int32_t _lib_filmstrip_get_activated_imgid(dt_lib_module_t *self)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;
  return strip->activated_image;
}

static gboolean _lib_filmstrip_copy_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                           GObject *aceeleratable, guint keyval,
                                                           GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return FALSE;
  strip->history_copy_imgid = mouse_over_id;

  /* check if images is currently loaded in darkroom */
  if (dt_dev_is_current_image(darktable.develop, mouse_over_id))
    dt_dev_write_history(darktable.develop);
  return TRUE;
}

static gboolean _lib_filmstrip_paste_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                            GObject *aceeleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  if (strip->history_copy_imgid==-1) return FALSE;

  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return FALSE;

  int mode = dt_conf_get_int("plugins/lighttable/copy_history/pastemode");

  dt_history_copy_and_paste_on_image(strip->history_copy_imgid, mouse_over_id, (mode == 0)?TRUE:FALSE);
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_filmstrip_discard_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                              GObject *aceeleratable, guint keyval,
                                                              GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  if (strip->history_copy_imgid==-1) return FALSE;

  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return FALSE;

  dt_history_delete_on_image(mouse_over_id);
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_filmstrip_ratings_key_accel_callback(GtkAccelGroup *accel_group,
                                                      GObject *aceeleratable, guint keyval,
                                                      GdkModifierType modifier, gpointer data)
{
  long int num = (long int)data;
  switch (num)
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
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      if (mouse_over_id <= 0) return FALSE;
      /* get image from cache */
      const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, mouse_over_id);
      dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);
      if (num == 666) 
        image->flags &= ~0xf;
      else if (num == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) 
        image->flags &= ~0x7;
      else
      {
        image->flags &= ~0x7;
        image->flags |= num;
      }
      dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
      dt_image_cache_read_release(darktable.image_cache, image);

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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

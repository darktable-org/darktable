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

#include "views/view.h"
#include "common/history.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_filmstrip_t
{
  /* state vars */
  int32_t last_selected_id;
  int32_t offset;
  int32_t history_copy_imgid;
  gdouble pointerx,pointery;
  dt_view_image_over_t image_over;

  int32_t activated_image;
}
dt_lib_filmstrip_t;

/* proxy function to center filmstrip on imgid */
static void _lib_filmstrip_scroll_to_image(dt_lib_module_t *self, gint imgid);
/* proxy function for retrieving last activate request image id */
static int32_t _lib_filmstrip_get_activated_imgid(dt_lib_module_t *self);

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
static void _lib_filmstrip_copy_history_key_accel_callback(GtkAccelGroup *accel_group,
							   GObject *aceeleratable, guint keyval,
							   GdkModifierType modifier, gpointer data);
static void _lib_filmstrip_paste_history_key_accel_callback(GtkAccelGroup *accel_group,
							    GObject *aceeleratable, guint keyval,
							    GdkModifierType modifier, gpointer data);
static void _lib_filmstrip_discard_history_key_accel_callback(GtkAccelGroup *accel_group,
							      GObject *aceeleratable, guint keyval,
							      GdkModifierType modifier, gpointer data);
static void _lib_filmstrip_ratings_key_accel_callback(GtkAccelGroup *accel_group,
						      GObject *aceeleratable, guint keyval,
						      GdkModifierType modifier, gpointer data);
static void _lib_filmstrip_colorlabels_key_accel_callback(GtkAccelGroup *accel_group,
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
          (gpointer)self,NULL));
  dt_accel_connect_lib(
      self, "paste history",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_paste_history_key_accel_callback),
          (gpointer)self,NULL));
  dt_accel_connect_lib(
      self, "discard history",
      g_cclosure_new(
          G_CALLBACK(_lib_filmstrip_discard_history_key_accel_callback),
          (gpointer)self,NULL));

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
  
  /* create drawingarea */
  self->widget = gtk_event_box_new();
  gtk_widget_add_events(self->widget, 
			GDK_POINTER_MOTION_MASK | 
			GDK_POINTER_MOTION_HINT_MASK | 
			GDK_BUTTON_PRESS_MASK | 
			GDK_BUTTON_RELEASE_MASK |
			GDK_SCROLL_MASK |
			GDK_LEAVE_NOTIFY_MASK);

  /* connect callbacks */
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_filmstrip_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_filmstrip_button_press_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "scroll-event",
                    G_CALLBACK (_lib_filmstrip_scroll_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "motion-notify-event",
		    G_CALLBACK(_lib_filmstrip_motion_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "leave-notify-event",
		    G_CALLBACK(_lib_filmstrip_mouse_leave_callback), self);

  
  /* set size of filmstrip */
  gtk_widget_set_size_request(self->widget, -1, 64);


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
			       (gpointer)self)
;
  /* unset viewmanager proxy */
  darktable.view_manager->proxy.filmstrip.module = NULL;
  
  /* cleaup */
  g_free(self->data);
  self->data = NULL;
}


static gboolean _lib_filmstrip_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  return TRUE;
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

  if (e->direction == GDK_SCROLL_UP || e->direction == GDK_SCROLL_LEFT) strip->offset--;
  else strip->offset++;
  gtk_widget_queue_draw(self->widget);
  return TRUE;
}

static gboolean _lib_filmstrip_button_press_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;

  int32_t mouse_over_id = -1;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);

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
      dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
      image->dirty = 1;
      if(strip->image_over == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
      else if(strip->image_over == DT_VIEW_REJECT && ((image->flags & 0x7) == 6)) image->flags &= ~0x7;
      else
      {
        image->flags &= ~0x7;
        image->flags |= strip->image_over;
      }
      dt_image_cache_flush(image);
      dt_image_cache_release(image, 'r');
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
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);

  /* create cairo surface */
  cairo_t *cr = gdk_cairo_create(widget->window); 

  /* fill background */
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  int offset = strip->offset;

  const float wd = height;
  const float ht = height;

  const int seli = pointerx / (float)wd;

  const int img_pointerx = (int)fmodf(pointerx, wd);
  const int img_pointery = (int)pointery;

  const int max_cols = (int)(1+width/(float)wd);
  sqlite3_stmt *stmt = NULL;

  /* get the count of current collection */
  int count = dt_collection_get_count (darktable.collection);

  /* get the collection query */
  const gchar *query=dt_collection_get_query (darktable.collection);
  if(!query)
    return FALSE;

  if(offset < 0)                strip->offset = offset = 0;
  if(offset > count-max_cols+1) strip->offset = offset = count-max_cols+1;
  // dt_view_set_scrollbar(self, offset, count, max_cols, 0, 1, 1);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_cols);
  

  cairo_save(cr);
  for(int col = 0; col < max_cols; col++)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
      dt_image_t *image = dt_image_cache_get(id, 'r');
      // set mouse over id
      if(seli == col)
      {
        mouse_over_id = id;
        DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
      }
      cairo_save(cr);
      // FIXME find out where the y translation is done, how big the value is and use it directly instead of getting it from the matrix ...
      cairo_matrix_t m;
      cairo_get_matrix(cr, &m);
      dt_view_image_expose(image, &(strip->image_over), id, cr, wd, ht, max_cols, img_pointerx, img_pointery);
      cairo_restore(cr);
      dt_image_cache_release(image, 'r');
    }
    else goto failure;
    cairo_translate(cr, wd, 0.0f);
  }
failure:
  cairo_restore(cr);
  sqlite3_finalize(stmt);

  if(darktable.gui->center_tooltip == 2) // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    g_object_set(G_OBJECT(dt_ui_center(darktable.gui->ui)), "tooltip-text", "", (char *)NULL);
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

static void _lib_filmstrip_scroll_to_image(dt_lib_module_t *self, gint imgid)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)self->data;
 
  /* if no imgid just bail out */
  if(imgid <= 0) return;

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

static void _lib_filmstrip_copy_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                           GObject *aceeleratable, guint keyval,
                                                           GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return;
  strip->history_copy_imgid = mouse_over_id;

  /* check if images is currently loaded in darkroom */
  if (dt_dev_is_current_image(darktable.develop, mouse_over_id))
    dt_dev_write_history(darktable.develop);
}

static void _lib_filmstrip_paste_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                            GObject *aceeleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  if (strip->history_copy_imgid==-1) return;

  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return;

  int mode = dt_conf_get_int("plugins/lighttable/copy_history/pastemode");

  dt_history_copy_and_paste_on_image(strip->history_copy_imgid, mouse_over_id, (mode == 0)?TRUE:FALSE);
  dt_control_queue_redraw_center();
}

static void _lib_filmstrip_discard_history_key_accel_callback(GtkAccelGroup *accel_group,
                                                              GObject *aceeleratable, guint keyval,
                                                              GdkModifierType modifier, gpointer data)
{
  dt_lib_filmstrip_t *strip = (dt_lib_filmstrip_t *)data;
  if (strip->history_copy_imgid==-1) return;

  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return;

  dt_history_delete_on_image(mouse_over_id);
  dt_control_queue_redraw_center();
}

static void _lib_filmstrip_ratings_key_accel_callback(GtkAccelGroup *accel_group,
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
      if (mouse_over_id <= 0) return;
      /* get image from cache */
      dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
      image->dirty = 1;
      if (num == 666) 
	image->flags &= ~0xf;
      else if (num == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) 
	image->flags &= ~0x7;
      else
      {
	image->flags &= ~0x7;
	image->flags |= num;
      }

      /* flush and release image */
      dt_image_cache_flush(image);
      dt_image_cache_release(image, 'r');

      /* redraw all */
      dt_control_queue_redraw();
      break;
    }
    default:
      break;
    }
}

static void _lib_filmstrip_colorlabels_key_accel_callback(GtkAccelGroup *accel_group,
							  GObject *acceleratable, guint keyval,
							  GdkModifierType modifier, gpointer data)
{
  dt_colorlabels_key_accel_callback(NULL, NULL, 0, 0, data);
  /* redraw filmstrip */
  if(darktable.view_manager->proxy.filmstrip.module)
    gtk_widget_queue_draw(darktable.view_manager->proxy.filmstrip.module->widget);
}

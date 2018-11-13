/*
    This file is part of darktable,
    copyright (c) 2016 Aldric Renaudin.

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
#include "common/metadata.h"
#include "common/mipmap_cache.h"
#include "common/history.h"
#include "common/styles.h"
#include "common/selection.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/styles.h"

DT_MODULE(1)

typedef enum _lib_duplicate_select_t
{
  DT_DUPLICATE_SELECT_NONE = 0,
  DT_DUPLICATE_SELECT_FIRST = 1,
  DT_DUPLICATE_SELECT_CURRENT = 2
} dt_lib_duplicate_select_t;

typedef struct dt_lib_duplicate_t
{
  GtkWidget *duplicate_box;
  int imgid;
  dt_lib_duplicate_select_t select;
} dt_lib_duplicate_t;

const char *name(dt_lib_module_t *self)
{
  return _("duplicate manager");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 850;
}

static void _lib_duplicate_init_callback(gpointer instance, dt_lib_module_t *self);

static gboolean _lib_duplicate_caption_out_callback(GtkWidget *widget, GdkEvent *event, dt_lib_module_t *self)
{
  int imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"imgid"));

  // we write the content of the textbox to the caption field
  dt_metadata_set(imgid, "Xmp.dc.title", gtk_entry_get_text(GTK_ENTRY(widget)));
  dt_image_synch_xmp(imgid);

  return FALSE;
}

static void _do_select(int imgid)
{
  // to select the duplicate, we reuse the filmstrip proxy
  dt_selection_select_single(darktable.selection, imgid);
  dt_control_set_mouse_over_id(imgid);
  dt_view_filmstrip_scroll_to_image(darktable.view_manager,imgid,TRUE);
}

static void _lib_duplicate_new_clicked_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  d->select = DT_DUPLICATE_SELECT_NONE;
  const int imgid = darktable.develop->image_storage.id;
  const int newid = dt_image_duplicate(imgid);
  if (newid <= 0) return;
  dt_history_delete_on_image(newid);
  // to select the duplicate, we reuse the filmstrip proxy
  dt_view_filmstrip_scroll_to_image(darktable.view_manager,newid,TRUE);
}
static void _lib_duplicate_duplicate_clicked_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  d->select = DT_DUPLICATE_SELECT_NONE;
  const int imgid = darktable.develop->image_storage.id;
  const int newid = dt_image_duplicate(imgid);
  if (newid <= 0) return;
  dt_history_copy_and_paste_on_image(imgid,newid,FALSE,NULL);
  // to select the duplicate, we reuse the filmstrip proxy
  dt_view_filmstrip_scroll_to_image(darktable.view_manager,newid,TRUE);
}

static void _lib_duplicate_delete(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  const int imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "imgid"));

  d->select = (imgid == darktable.develop->image_storage.id) ? DT_DUPLICATE_SELECT_FIRST : DT_DUPLICATE_SELECT_CURRENT;

  dt_selection_select_single(darktable.selection, imgid);
  dt_control_set_mouse_over_id(imgid);
  dt_control_delete_images();

  _lib_duplicate_init_callback(NULL, self);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
}

static void _lib_duplicate_thumb_press_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  int imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"imgid"));

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS)
    {
      dt_develop_t *dev = darktable.develop;
      if(!dev) return;
      dt_dev_zoom_t zoom;
      int closeup;
      float zoom_x, zoom_y;
      closeup = dt_control_get_dev_closeup();
      float scale = 0;

      zoom = DT_ZOOM_FIT;
      scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);

      dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
      dt_control_set_dev_zoom_scale(scale);
      dt_control_set_dev_zoom(zoom);
      dt_control_set_dev_closeup(closeup);
      dt_control_set_dev_zoom_x(zoom_x);
      dt_control_set_dev_zoom_y(zoom_y);
      dt_dev_invalidate(dev);
      dt_control_queue_redraw();

      dt_dev_invalidate(darktable.develop);

      d->imgid = imgid;
      dt_control_queue_redraw_center();
    }
    else if(event->type == GDK_2BUTTON_PRESS)
    {
      // to select the duplicate, we reuse the filmstrip proxy
      _do_select(imgid);
    }
  }
}

static void _lib_duplicate_thumb_release_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = 0;
  dt_control_queue_redraw_center();
}

void gui_post_expose(dt_lib_module_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  if (d->imgid == 0) return;

  const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  int nw = width-2*tb;
  int nh = height-2*tb;

  dt_mipmap_buffer_t buf;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, nw, nh);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, d->imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');

  int img_wd = buf.width;
  int img_ht = buf.height;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  // and now we get the values to "fit the screen"
  int px,py,nimgw,nimgh;
  if (img_ht*nw > img_wd*nh)
  {
    nimgh = nh;
    nimgw = img_wd*nh/img_ht;
    py=0;
    px=(nw-nimgw)/2;
  }
  else
  {
    nimgw = nw;
    nimgh = img_ht*nw/img_wd;
    px=0;
    py=(nh-nimgh)/2;
  }

  // we erase everything
  cairo_set_source_rgb(cri, .2, .2, .2);
  cairo_paint(cri);

  //we draw the cached image
  dt_view_image_over_t image_over = DT_VIEW_DESERT;
  dt_view_image_expose(&image_over, d->imgid, cri, nw, nh, 1, px+tb, py+tb, TRUE, TRUE);

  //and the nice border line
  cairo_rectangle(cri, tb+px, tb+py, nimgw, nimgh);
  cairo_set_line_width(cri, 1.0);
  cairo_set_source_rgb(cri, .3, .3, .3);
  cairo_stroke(cri);
}

static gboolean _lib_duplicate_thumb_draw_callback (GtkWidget *widget, cairo_t *cr, dt_lib_module_t *self)
{
  guint width, height;
  width = gtk_widget_get_allocated_width (widget);
  height = gtk_widget_get_allocated_height (widget);
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  int imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"imgid"));
  dt_view_image_over_t image_over = DT_VIEW_DESERT;
  dt_view_image_expose(&image_over, imgid, cr, width, height, 5, 0, 0, FALSE, FALSE);

 return FALSE;
}

static void _lib_duplicate_init_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = 0;
  gtk_container_foreach(GTK_CONTAINER(d->duplicate_box), (GtkCallback)gtk_widget_destroy, 0);
  // retrieve all the versions of the image
  sqlite3_stmt *stmt;
  dt_develop_t *dev = darktable.develop;

  int first_imgid = -1;

  // we get a summarize of all versions of the image
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT i.version, i.id, m.value FROM images AS "
                                                             "i LEFT JOIN meta_data AS m ON m.id = i.id AND "
                                                             "m.key = ?3 WHERE film_id = ?1 AND filename = "
                                                             "?2 ORDER BY i.version",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, dev->image_storage.filename, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, DT_METADATA_XMP_DC_TITLE);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *dr = gtk_drawing_area_new();
    const int imgid = sqlite3_column_int(stmt, 1);

    // select original picture
    if (first_imgid == -1) first_imgid = imgid;

    gtk_widget_set_size_request (dr, 100, 100);
    g_object_set_data (G_OBJECT (dr),"imgid",GINT_TO_POINTER(imgid));
    gtk_widget_add_events(dr, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect (G_OBJECT (dr), "draw", G_CALLBACK (_lib_duplicate_thumb_draw_callback), self);
    if (imgid != dev->image_storage.id)
    {
      g_signal_connect(G_OBJECT(dr), "button-press-event", G_CALLBACK(_lib_duplicate_thumb_press_callback), self);
      g_signal_connect(G_OBJECT(dr), "button-release-event", G_CALLBACK(_lib_duplicate_thumb_release_callback), self);
    }

    gchar chl[256];
    gchar *path = (gchar *)sqlite3_column_text(stmt, 2);
    g_snprintf(chl, sizeof(chl), "%d", sqlite3_column_int(stmt, 0));

    GtkWidget *tb = gtk_entry_new();
    if(path) gtk_entry_set_text(GTK_ENTRY(tb), path);
    gtk_entry_set_width_chars(GTK_ENTRY(tb), 15);
    g_object_set_data (G_OBJECT (tb),"imgid",GINT_TO_POINTER(imgid));
    g_signal_connect(G_OBJECT(tb), "focus-out-event", G_CALLBACK(_lib_duplicate_caption_out_callback), self);
    dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(tb));
    GtkWidget *lb = gtk_label_new (g_strdup(chl));
    GtkWidget *bt = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
    g_object_set_data(G_OBJECT(bt), "imgid", GINT_TO_POINTER(imgid));
    gtk_widget_set_size_request(bt, DT_PIXEL_APPLY_DPI(13), DT_PIXEL_APPLY_DPI(13));
    g_signal_connect(G_OBJECT(bt), "clicked", G_CALLBACK(_lib_duplicate_delete), self);

    gtk_box_pack_start(GTK_BOX(hb), dr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), tb, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(hb), lb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), bt, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(d->duplicate_box), hb, FALSE, FALSE, 0);
  }
  sqlite3_finalize (stmt);

  switch(d->select)
  {
     case DT_DUPLICATE_SELECT_FIRST:
       _do_select(first_imgid);
       break;
     case DT_DUPLICATE_SELECT_CURRENT:
         _do_select(darktable.develop->image_storage.id);
       break;
     case DT_DUPLICATE_SELECT_NONE:
     default:
       break;
  }
  d->select = DT_DUPLICATE_SELECT_NONE;

  gtk_widget_show_all(d->duplicate_box);
}

static void _lib_duplicate_mipmap_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  gtk_widget_queue_draw (d->duplicate_box);
  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)g_malloc0(sizeof(dt_lib_duplicate_t));
  self->data = (void *)d;

  d->imgid = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_name(self->widget, "duplicate-ui");

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), DT_PIXEL_APPLY_DPI(300));
  d->duplicate_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *bt = gtk_label_new(_("existing duplicates"));
  gtk_box_pack_start(GTK_BOX(hb), bt, FALSE, FALSE, 0);
  bt = dtgtk_button_new(dtgtk_cairo_paint_plusminus, CPF_ACTIVE | CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(bt), "tooltip-text", _("create a 'virgin' duplicate of the image without any developpement"), (char *)NULL);
  g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_lib_duplicate_new_clicked_callback), self);
  gtk_box_pack_end(GTK_BOX(hb), bt, FALSE, FALSE, 0);
  bt = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(bt), "tooltip-text", _("create a duplicate of the image with same history stack"), (char *)NULL);
  g_signal_connect(G_OBJECT(bt), "button-press-event", G_CALLBACK(_lib_duplicate_duplicate_clicked_callback), self);
  gtk_box_pack_end(GTK_BOX(hb), bt, FALSE, FALSE, 0);


  /* add duplicate list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget), hb, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(sw), d->duplicate_box);
  gtk_box_pack_start(GTK_BOX(self->widget), sw, FALSE, FALSE, 0);

  gtk_widget_show_all(self->widget);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED, G_CALLBACK(_lib_duplicate_init_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE, G_CALLBACK(_lib_duplicate_init_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_duplicate_init_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, G_CALLBACK(_lib_duplicate_mipmap_updated_callback), (gpointer)self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_duplicate_init_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_duplicate_mipmap_updated_callback), self);
  g_free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

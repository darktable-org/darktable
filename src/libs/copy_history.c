#include "common/darktable.h"
#include "common/image_cache.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <glade/glade.h>

typedef struct dt_lib_copy_history_t
{
  int32_t imageid;
  GtkButton *paste;
}
dt_lib_copy_history_t;


const char*
name ()
{
  return _("copy history stack");
}

static void
copy_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    d->imageid = sqlite3_column_int(stmt, 0);
    gtk_widget_set_sensitive(GTK_WIDGET(d->paste), TRUE);
  }
  sqlite3_finalize(stmt);
}

static void
paste_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  if(d->imageid < 0) return;

  int rc;
  sqlite3_stmt *stmt, *stmt2;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int32_t imgid = sqlite3_column_int(stmt, 0);
    rc = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt2, NULL);
    rc = sqlite3_bind_int(stmt2, 1, imgid);
    rc = sqlite3_step(stmt2);
    rc = sqlite3_finalize(stmt2);
    rc = sqlite3_prepare_v2(darktable.db, "insert into history (imgid, num, module, operation, op_params, enabled) select ?1, num, module, operation, op_params, enabled from history where imgid = ?2", -1, &stmt2, NULL);
    rc = sqlite3_bind_int(stmt2, 1, imgid);
    rc = sqlite3_bind_int(stmt2, 2, d->imageid);
    rc = sqlite3_step(stmt2);
    rc = sqlite3_finalize(stmt2);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1", -1, &stmt2, NULL);
    rc = sqlite3_bind_int(stmt2, 1, imgid);
    rc = sqlite3_step(stmt2);
    rc = sqlite3_finalize(stmt2);
    dt_image_t *img = dt_image_cache_use(imgid, 'r');
    pthread_mutex_lock(&(darktable.mipmap_cache->mutex));
    for(int k=0;(int)k<(int)DT_IMAGE_MIPF;k++) dt_image_free(img, k);
    pthread_mutex_unlock(&(darktable.mipmap_cache->mutex));
    dt_image_cache_release(img, 'r');
  }
  sqlite3_finalize(stmt);
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;
  d->imageid = -1;
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)malloc(sizeof(dt_lib_copy_history_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 5);

  GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 0));
  GtkWidget *copy = gtk_button_new_with_label(_("copy"));
  gtk_object_set(GTK_OBJECT(copy), "tooltip-text", _("copy history stack of\nfirst selected image"), NULL);
  d->paste = GTK_BUTTON(gtk_button_new_with_label(_("paste")));
  gtk_object_set(GTK_OBJECT(d->paste), "tooltip-text", _("paste history stack to\nall selected images"), NULL);
  d->imageid = -1;
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
  gtk_box_pack_start(hbox, copy, TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->paste), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (copy), "clicked",
                    G_CALLBACK (copy_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (d->paste), "clicked",
                    G_CALLBACK (paste_button_clicked),
                    (gpointer)self);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}



#include "control/jobs.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "gui/gtk.h"
#include <glade/glade.h>
#include <glib.h>
#include <glib/gstdio.h>

void dt_image_load_job_init(dt_job_t *job, int32_t id, dt_image_buffer_t mip)
{
  dt_control_job_init(job, "load image %d mip %d", id, mip);
  job->execute = &dt_image_load_job_run;
  dt_image_load_t *t = (dt_image_load_t *)job->param;
  t->imgid = id;
  t->mip = mip;
}

void dt_image_load_job_run(dt_job_t *job)
{
  dt_image_load_t *t = (dt_image_load_t *)job->param;
  dt_image_t *img = dt_image_cache_get(t->imgid, 'r');
  int ret = dt_image_load(img, t->mip);
  // drop read lock, as this is only speculative async loading.
  if(!ret) dt_image_release(img, t->mip, 'r');
  dt_image_cache_release(img, 'r');
}

void dt_film_import1_init(dt_job_t *job, dt_film_t *film)
{
  dt_control_job_init(job, "cache load raw images for preview");
  job->execute = &dt_film_import1_run;
  dt_film_import1_t *t = (dt_film_import1_t *)job->param;
  t->film = film;
}

void dt_film_import1_run(dt_job_t *job)
{
  dt_film_import1_t *t = (dt_film_import1_t *)job->param;
  dt_film_import1(t->film);
}

// ====================
// develop:
// ====================

void dt_dev_raw_load_job_run(dt_job_t *job)
{
  dt_dev_raw_load_t *t = (dt_dev_raw_load_t *)job->param;
  dt_dev_raw_load(t->dev, t->image);
}

void dt_dev_raw_load_job_init(dt_job_t *job, dt_develop_t *dev, dt_image_t *image)
{
  dt_control_job_init(job, "develop load raw image %s", image->filename);
  job->execute =&dt_dev_raw_load_job_run;
  dt_dev_raw_load_t *t = (dt_dev_raw_load_t *)job->param;
  t->dev = dev;
  t->image = image;
}

void dt_dev_process_preview_job_run(dt_job_t *job)
{
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  dt_dev_process_preview_job(t->dev);
}

void dt_dev_process_preview_job_init(dt_job_t *job, dt_develop_t *dev)
{
  dt_control_job_init(job, "develop process preview");
  job->execute = &dt_dev_process_preview_job_run;
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  t->dev = dev;
}

void dt_dev_process_image_job_run(dt_job_t *job)
{
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  dt_dev_process_image_job(t->dev);
}

void dt_dev_process_image_job_init(dt_job_t *job, dt_develop_t *dev)
{
  dt_control_job_init(job, "develop image preview");
  job->execute = &dt_dev_process_image_job_run;
  dt_dev_process_t *t = (dt_dev_process_t *)job->param;
  t->dev = dev;
}

typedef struct dt_control_image_enumerator_t
{
  GList *index;
}
dt_control_image_enumerator_t;

void dt_control_write_dt_files_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_t *img = dt_image_cache_use(imgid, 'r');
    char dtfilename[520];
    dt_image_full_path(img, dtfilename, 512);
    char *c = dtfilename + strlen(dtfilename);
    for(;c>dtfilename && *c != '.';c--);
    sprintf(c, ".dt");
    dt_imageio_dt_write(imgid, dtfilename);
    sprintf(c, ".dttags");
    dt_imageio_dttags_write(imgid, dtfilename);
    dt_image_cache_release(img, 'r');
    t = g_list_delete_link(t, t);
  }
}

void dt_control_duplicate_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_duplicate(imgid);
    t = g_list_delete_link(t, t);
  }
}

void dt_control_remove_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_remove(imgid);
    t = g_list_delete_link(t, t);
  }
}

void dt_control_delete_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_t *img = dt_image_cache_use(imgid, 'r');
    char dtfilename[512];
    dt_image_full_path(img, dtfilename, 512);
    int rc;
    sqlite3_stmt *stmt;
    // remove from db:
    rc = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, imgid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize (stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from images where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, imgid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize (stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, imgid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize (stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, imgid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize (stmt);
    // remove from disk:
    (void)g_unlink(dtfilename);
    char *c = dtfilename + strlen(dtfilename);
    for(;c>dtfilename && *c != '.';c--);
    sprintf(c, ".dt");
    (void)g_unlink(dtfilename);
    sprintf(c, ".dttags");
    (void)g_unlink(dtfilename);
    dt_image_cache_release(img, 'r');
    t = g_list_delete_link(t, t);
  }
}

void dt_control_image_enumerator_job_init(dt_control_image_enumerator_t *t)
{
  t->index = NULL;
  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    long int imgid = sqlite3_column_int(stmt, 0);
    t->index = g_list_prepend(t->index, (gpointer)imgid);
  }
  sqlite3_finalize(stmt);
}

void dt_control_write_dt_files_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "write dt files");
  job->execute = &dt_control_write_dt_files_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_write_dt_files()
{
  dt_job_t j;
  dt_control_write_dt_files_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_duplicate_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "duplicate images");
  job->execute = &dt_control_duplicate_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_remove_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "remove images");
  job->execute = &dt_control_remove_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_delete_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "delete images");
  job->execute = &dt_control_delete_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_duplicate_images()
{
  dt_job_t j;
  dt_control_duplicate_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_remove_images()
{
  if(gconf_client_get_bool(darktable.control->gconf, DT_GCONF_DIR"/ask_before_remove", NULL))
  {
    GtkWidget *dialog;
    GtkWidget *win = glade_xml_get_widget (darktable.gui->main_window, "main_window");
    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        _("do you really want to remove all selected images from the collection?"));
    gtk_window_set_title(GTK_WINDOW(dialog), _("remove images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES) return;
  }
  dt_job_t j;
  dt_control_remove_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_delete_images()
{
  if(gconf_client_get_bool(darktable.control->gconf, DT_GCONF_DIR"/ask_before_delete", NULL))
  {
    GtkWidget *dialog;
    GtkWidget *win = glade_xml_get_widget (darktable.gui->main_window, "main_window");
    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        _("do you really want to physically delete all selected images from disk?"));
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES) return;
  }
  dt_job_t j;
  dt_control_delete_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_export_job_run(dt_job_t *job)
{
  char filename[1024], dirname[1024];
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_t *img = dt_image_cache_use(imgid, 'r');

    dt_image_export_path(img, filename, 1024);
    strncpy(dirname, filename, 1024);

    char *c = dirname + strlen(dirname);
    for(;c>dirname && *c != '/';c--);
    *c = '\0';
    if(g_mkdir_with_parents(dirname, 0755))
    {
      fprintf(stderr, "[export_job] could not create directory %s!\n", dirname);
      dt_image_cache_release(img, 'r');
      return;
    }
    c = filename + strlen(filename);
    for(;c>filename && *c != '.';c--);
    if(c <= filename) c = filename + strlen(filename);

    // read type from global config.
    dt_dev_export_format_t fmt =
      gconf_client_get_int  (darktable.control->gconf, DT_GCONF_DIR"/plugins/lighttable/export/format", NULL);
    switch(fmt)
    {
      case DT_DEV_EXPORT_JPG:
        // avoid name clashes for single images:
        if(img->film_id == 1 && !strcmp(c, ".jpg")) { strncpy(c, "_dt", 3); c += 3; }
        strncpy(c, ".jpg", 4);
        dt_imageio_export_8(img, filename);
        break;
      case DT_DEV_EXPORT_PNG:
        if(img->film_id == 1 && !strcmp(c, ".png")) { strncpy(c, "_dt", 3); c += 3; }
        strncpy(c, ".png", 4);
        dt_imageio_export_8(img, filename);
        break;
      case DT_DEV_EXPORT_PPM16:
        if(img->film_id == 1 && !strcmp(c, ".ppm")) { strncpy(c, "_dt", 3); c += 3; }
        strncpy(c, ".ppm", 4);
        dt_imageio_export_16(img, filename);
        break;
      case DT_DEV_EXPORT_PFM:
        if(img->film_id == 1 && !strcmp(c, ".pfm")) { strncpy(c, "_dt", 3); c += 3; }
        strncpy(c, ".pfm", 4);
        dt_imageio_export_f(img, filename);
        break;
      default:
        break;
    }

    dt_image_cache_release(img, 'r');
    printf("[export_job] exported to `%s'\n", filename);
    t = g_list_delete_link(t, t);
  }
}

void dt_control_export_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "export");
  job->execute = &dt_control_export_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

void dt_control_export()
{
  dt_job_t j;
  dt_control_export_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}




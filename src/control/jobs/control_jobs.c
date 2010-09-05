/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#include <glib.h>
#include <glib/gstdio.h>
#include <glade/glade.h>

#include "common/collection.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/jobs/control_jobs.h"

#include "gui/gtk.h"

void dt_control_write_dt_files()
{
  dt_job_t j;
  dt_control_write_dt_files_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_write_dt_files_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "write dt files");
  job->execute = &dt_control_write_dt_files_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_init(t);
}

int32_t dt_control_write_dt_files_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_t *img = dt_image_cache_get(imgid, 'r');
    char dtfilename[520];
    dt_image_full_path(img, dtfilename, 512);
    char *c = dtfilename + strlen(dtfilename);
    sprintf(c, ".dt");
    dt_imageio_dt_write(imgid, dtfilename);
    sprintf(c, ".dttags");
    dt_imageio_dttags_write(imgid, dtfilename);
    dt_image_cache_release(img, 'r');
    t = g_list_delete_link(t, t);
  }
  return 0;
}

int32_t dt_control_duplicate_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]={0};
  double fraction=0;
  snprintf(message, 512, ngettext ("duplicating %d image", "duplicating %d images", total), total );
  const dt_gui_job_t *j = dt_gui_background_jobs_new( DT_JOB_PROGRESS, message);
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_duplicate(imgid);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_gui_background_jobs_set_progress(j, fraction);
  }
  dt_gui_background_jobs_destroy (j);
  return 0;
}

int32_t dt_control_remove_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]={0};
  double fraction=0;
  snprintf(message, 512, ngettext ("removing %d image", "removing %d images", total), total );
  const dt_gui_job_t *j = dt_gui_background_jobs_new( DT_JOB_PROGRESS, message);
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_remove(imgid);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_gui_background_jobs_set_progress(j, fraction);
  }
  dt_gui_background_jobs_destroy (j);
  return 0;
}

int32_t dt_control_delete_images_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]={0};
  double fraction=0;
  snprintf(message, 512, ngettext ("deleting %d image", "deleting %d images", total), total );
  const dt_gui_job_t *j = dt_gui_background_jobs_new(DT_JOB_PROGRESS, message);
  while(t)
  {
    imgid = (long int)t->data;
    dt_image_t *img = dt_image_cache_get(imgid, 'r');
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
    sprintf(c, ".dt");
    (void)g_unlink(dtfilename);
    sprintf(c, ".dttags");
    (void)g_unlink(dtfilename);
    dt_image_cache_release(img, 'r');
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_gui_background_jobs_set_progress(j, fraction);
  }
  dt_gui_background_jobs_destroy (j);
  return 0;
}

void dt_control_image_enumerator_job_init(dt_control_image_enumerator_t *t)
{
  /* get sorted list of selected images */
  t->index = dt_collection_get_selected(darktable.collection);
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
  if(dt_conf_get_bool("ask_before_remove"))
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
  if(dt_conf_get_bool("ask_before_delete"))
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

int32_t dt_control_export_job_run(dt_job_t *job)
{
  long int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  const int total = g_list_length(t);
  int size = 0;
  dt_imageio_module_format_t  *mformat  = dt_imageio_get_format();
  g_assert(mformat);
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage();
  g_assert(mstorage);
  
  // Get max dimensions...
  uint32_t w,h,fw,fh,sw,sh;
  w=h=fw=fh=sw=sh=0; // We are all equals!!!
  mstorage->dimension(mstorage, &sw,&sh);
  mformat->dimension(mformat, &fw,&fh);

  if( sw==0 || fw==0) w=sw>fw?sw:fw;
  else w=sw<fw?sw:fw;

  if( sh==0 || fh==0) h=sh>fh?sh:fh;
  else h=sh<fh?sh:fh;

  // get shared storage param struct (global sequence counter, one picasa connection etc)
  dt_imageio_module_data_t *sdata = mstorage->get_params(mstorage, &size);
  if(sdata == NULL)
  {
    dt_control_log(_("failed to get parameters from storage module, aborting export.."));
    return 1;
  }
  dt_control_log(ngettext ("exporting %d image..", "exporting %d images..", total), total);
  char message[512]={0};
  snprintf(message, 512, ngettext ("exporting %d image to %s", "exporting %d images to %s", total), total, mstorage->name() );
  const dt_gui_job_t *j = dt_gui_background_jobs_new( DT_JOB_PROGRESS, message );
  double fraction=0;
#ifdef _OPENMP
  // limit this to num threads = num full buffers - 1 (keep one for darkroom mode)
  // use min of user request and mipmap cache entries
  const int full_entries = dt_conf_get_int ("mipmap_cache_full_images");
  const int num_threads = MAX(1, MIN(full_entries, darktable.mipmap_cache->num_entries[DT_IMAGE_FULL]) - 1);
  #pragma omp parallel shared(j, fraction) num_threads(num_threads)
  {
  // get a thread-safe fdata struct (one jpeg struct per thread etc):
  dt_imageio_module_data_t *fdata = mformat->get_params(mformat, &size);
  fdata->max_width  = dt_conf_get_int ("plugins/lighttable/export/width");
  fdata->max_height = dt_conf_get_int ("plugins/lighttable/export/height");
  fdata->max_width = (w!=0 && fdata->max_width >w)?w:fdata->max_width;
  fdata->max_height = (h!=0 && fdata->max_height >h)?h:fdata->max_height;
#endif
  while(t)
  {
#ifdef _OPENMP
  #pragma omp critical
#endif
    {
      imgid = (long int)t->data;
      t = g_list_delete_link(t, t);
    }
    // check if image still exists:
    char imgfilename[1024];
    dt_image_t *image = dt_image_cache_get(imgid, 'r');
    if(image)
    {
      dt_image_full_path(image, imgfilename, 1024);
      dt_image_cache_release(image, 'r');
      if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
      {
        dt_control_log(_("image does no longer exist"));
        dt_image_remove(imgid);
      }
      else
      {
        mstorage->store(sdata, imgid, mformat, fdata, total-g_list_length(t), total);
      }
    }
#ifdef _OPENMP
  #pragma omp critical
#endif
    {
      fraction+=1.0/total;
      dt_gui_background_jobs_set_progress( j, fraction );
    }
  }
#ifdef _OPENMP
  #pragma omp barrier
  #pragma omp master
#endif
  {
    dt_gui_background_jobs_destroy (j);
    if(mstorage->finalize_store) mstorage->finalize_store(mstorage, sdata);
    mstorage->free_params(mstorage, sdata);
  }
  // all threads free their fdata
  mformat->free_params (mformat, fdata);
  }
  return 0;
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

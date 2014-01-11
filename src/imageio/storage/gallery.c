/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/variables.h"
#include "common/metadata.h"
#include "common/debug.h"
#include "common/utility.h"
#include "common/imageio_storage.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

// gui data
typedef struct gallery_t
{
  GtkEntry *entry;
  GtkEntry *title_entry;
}
gallery_t;

// saved params
typedef struct dt_imageio_gallery_t
{
  char filename[DT_MAX_PATH_LEN];
  char title[1024];
  char cached_dirname[DT_MAX_PATH_LEN]; // expanded during first img store, not stored in param struct.
  dt_variables_params_t *vp;
  GList *l;
}
dt_imageio_gallery_t;

// sorted list of all images
typedef struct pair_t
{
  char line[4096];
  int pos;
}
pair_t;


const char*
name (const struct dt_imageio_module_storage_t *self)
{
  return _("website gallery");
}

static void
button_clicked (GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  gallery_t *d = (gallery_t *)self->gui_data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("select directory"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  gchar *old = g_strdup(gtk_entry_get_text(d->entry));
  char *c = g_strstr_len(old, -1, "$");
  if(c) *c = '\0';
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), old);
  g_free(old);
  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
    char composed[DT_MAX_PATH_LEN];
    snprintf(composed, DT_MAX_PATH_LEN, "%s/$(FILE_NAME)", dir);
    gtk_entry_set_text(GTK_ENTRY(d->entry), composed);
    dt_conf_set_string("plugins/imageio/storage/gallery/file_directory", composed);
    g_free(dir);
  }
  gtk_widget_destroy (filechooser);
}

void
gui_init (dt_imageio_module_storage_t *self)
{
  gallery_t *d = (gallery_t *)malloc(sizeof(gallery_t));
  self->gui_data = (void *)d;
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  GtkWidget *widget;

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  gchar *dir = dt_conf_get_string("plugins/imageio/storage/gallery/file_directory");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), dir);
    g_free(dir);
  }
  d->entry = GTK_ENTRY(widget);
  dt_gui_key_accel_block_on_focus_connect (GTK_WIDGET (d->entry));

  dt_gtkentry_completion_spec compl_list[] =
  {
    { "ROLL_NAME", _("$(ROLL_NAME) - roll of the input image") },
    { "FILE_FOLDER", _("$(FILE_FOLDER) - folder containing the input image") },
    { "FILE_NAME", _("$(FILE_NAME) - basename of the input image") },
    { "FILE_EXTENSION", _("$(FILE_EXTENSION) - extension of the input image") },
    { "SEQUENCE", _("$(SEQUENCE) - sequence number") },
    { "YEAR", _("$(YEAR) - year") },
    { "MONTH", _("$(MONTH) - month") },
    { "DAY", _("$(DAY) - day") },
    { "HOUR", _("$(HOUR) - hour") },
    { "MINUTE", _("$(MINUTE) - minute") },
    { "SECOND", _("$(SECOND) - second") },
    { "EXIF_YEAR", _("$(EXIF_YEAR) - EXIF year") },
    { "EXIF_MONTH", _("$(EXIF_MONTH) - EXIF month") },
    { "EXIF_DAY", _("$(EXIF_DAY) - EXIF day") },
    { "EXIF_HOUR", _("$(EXIF_HOUR) - EXIF hour") },
    { "EXIF_MINUTE", _("$(EXIF_MINUTE) - EXIF minute") },
    { "EXIF_SECOND", _("$(EXIF_SECOND) - EXIF second") },
    { "STARS", _("$(STARS) - star rating") },
    { "LABELS", _("$(LABELS) - colorlabels") },
    { "PICTURES_FOLDER", _("$(PICTURES_FOLDER) - pictures folder") },
    { "HOME", _("$(HOME) - home folder") },
    { "DESKTOP", _("$(DESKTOP) - desktop folder") },
    { NULL, NULL }
  };

  dt_gtkentry_setup_completion(GTK_ENTRY(widget), compl_list);

  char *tooltip_text = dt_gtkentry_build_completion_tooltip_text (
                         _("enter the path where to put exported images\nrecognized variables:"),
                         compl_list);
  g_object_set(G_OBJECT(widget), "tooltip-text", tooltip_text, (char *)NULL);
  g_free(tooltip_text);

  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, 0);
  gtk_widget_set_size_request(widget, 18, 18);
  g_object_set(G_OBJECT(widget), "tooltip-text", _("select directory"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(hbox), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);

  hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  widget = gtk_label_new(_("title"));
  gtk_misc_set_alignment(GTK_MISC(widget), 0.0f, 0.5f);
  gtk_box_pack_start(GTK_BOX(hbox), widget, FALSE, FALSE, 0);
  d->title_entry = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(d->title_entry), TRUE, TRUE, 0);
  dt_gui_key_accel_block_on_focus_connect (GTK_WIDGET (d->title_entry));
  g_object_set(G_OBJECT(d->title_entry), "tooltip-text", _("enter the title of the website"), (char *)NULL);
  dir = dt_conf_get_string("plugins/imageio/storage/gallery/title");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(d->title_entry), dir);
    g_free(dir);
  }
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
  gallery_t *d = (gallery_t *)self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect (GTK_WIDGET (d->entry));
  dt_gui_key_accel_block_on_focus_disconnect (GTK_WIDGET (d->title_entry));
  free(self->gui_data);
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
  gallery_t *d = (gallery_t *)self->gui_data;
  dt_conf_set_string("plugins/imageio/storage/gallery/file_directory", gtk_entry_get_text(d->entry));
  dt_conf_set_string("plugins/imageio/storage/gallery/title", gtk_entry_get_text(d->title_entry));
}

static gint
sort_pos(pair_t *a, pair_t *b)
{
  return a->pos - b->pos;
}

int
store (dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata,
       const int num, const int total, const gboolean high_quality)
{
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)sdata;

  char filename[DT_MAX_PATH_LEN]= {0};
  char dirname[DT_MAX_PATH_LEN]= {0};
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, dirname, DT_MAX_PATH_LEN, &from_cache);
  // we're potentially called in parallel. have sequence number synchronized:
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {

    char tmp_dir[DT_MAX_PATH_LEN];

    d->vp->filename = dirname;
    d->vp->jobcode = "export";
    d->vp->imgid = imgid;
    d->vp->sequence = num;
    dt_variables_expand(d->vp, d->filename, TRUE);
    g_strlcpy(tmp_dir, dt_variables_get_result(d->vp), DT_MAX_PATH_LEN);

    // if filenamepattern is a directory just let att ${FILE_NAME} as default..
    if ( g_file_test(tmp_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR) || ((d->filename+strlen(d->filename)-1)[0]=='/' || (d->filename+strlen(d->filename)-1)[0]=='\\') )
      snprintf (d->filename+strlen(d->filename), DT_MAX_PATH_LEN-strlen(d->filename), "/$(FILE_NAME)");

    // avoid braindead export which is bound to overwrite at random:
    if(total > 1 && !g_strrstr(d->filename, "$"))
    {
      snprintf(d->filename+strlen(d->filename), DT_MAX_PATH_LEN-strlen(d->filename), "_$(SEQUENCE)");
    }

    gchar* fixed_path = dt_util_fix_path(d->filename);
    g_strlcpy(d->filename, fixed_path, DT_MAX_PATH_LEN);
    g_free(fixed_path);

    dt_variables_expand(d->vp, d->filename, TRUE);
    g_strlcpy(filename, dt_variables_get_result(d->vp), DT_MAX_PATH_LEN);
    g_strlcpy(dirname, filename, DT_MAX_PATH_LEN);

    const char *ext = format->extension(fdata);
    char *c = dirname + strlen(dirname);
    for(; c>dirname && *c != '/'; c--);
    if(*c == '/') *c = '\0';
    if(g_mkdir_with_parents(dirname, 0755))
    {
      fprintf(stderr, "[imageio_storage_gallery] could not create directory: `%s'!\n", dirname);
      dt_control_log(_("could not create directory `%s'!"), dirname);
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
      return 1;
    }

    // store away dir.
    snprintf(d->cached_dirname, DT_MAX_PATH_LEN, "%s", dirname);

    c = filename + strlen(filename);
    for(; c>filename && *c != '.' && *c != '/' ; c--);
    if(c <= filename || *c=='/') c = filename + strlen(filename);

    sprintf(c,".%s",ext);

    // save image to list, in order:
    pair_t *pair = malloc(sizeof(pair_t));

    char *title = NULL, *description = NULL;
    GList *res_title, *res_desc;

    res_title = dt_metadata_get(imgid, "Xmp.dc.title", NULL);
    if(res_title)
    {
      title = res_title->data;
    }

    res_desc = dt_metadata_get(imgid, "Xmp.dc.description", NULL);
    if(res_desc)
    {
      description = res_desc->data;
    }

    char relfilename[256], relthumbfilename[256];
    c = filename + strlen(filename);
    for(; c>filename && *c != '/' ; c--);
    if(*c == '/') c++;
    if(c <= filename) c = filename;
    snprintf(relfilename, 256, "%s", c);
    snprintf(relthumbfilename, 256, "%s", relfilename);
    c = relthumbfilename + strlen(relthumbfilename);
    for(; c>relthumbfilename && *c != '.'; c--);
    if(c <= relthumbfilename) c = relthumbfilename + strlen(relthumbfilename);
    sprintf(c, "-thumb.%s", ext);

    char subfilename[DT_MAX_PATH_LEN], relsubfilename[256];
    snprintf(subfilename, DT_MAX_PATH_LEN, "%s", d->cached_dirname);
    char* sc = subfilename + strlen(subfilename);
    sprintf(sc, "/img_%d.html", num);
    sprintf(relsubfilename, "img_%d.html", num);

    snprintf(pair->line, 4096,
             "\n"
             "      <div><a class=\"dia\" rel=\"lightbox[viewer]\" title=\"%s - %s\" href=\"%s\"><span></span><img src=\"%s\" alt=\"img%d\" class=\"img\"/></a>\n"
             "      <h1>%s</h1>\n"
             "      %s</div>\n", title?title:relfilename, description?description:"&nbsp;", relfilename, relthumbfilename, num, title?title:"&nbsp;", description?description:"&nbsp;");

    char next[256];
    sprintf(next, "img_%d.html", (num)%total+1);

    char prev[256];
    sprintf(prev, "img_%d.html", (num==1)?total:num-1);

    pair->pos = num;
    if(res_title) g_list_free_full(res_title, &g_free);
    if(res_desc) g_list_free_full(res_desc, &g_free);
    d->l = g_list_insert_sorted(d->l, pair, (GCompareFunc)sort_pos);
  } // end of critical block
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  /* export image to file */
  if(dt_imageio_export(imgid, filename, format, fdata, high_quality,FALSE,self,sdata) != 0)
  {
    fprintf(stderr, "[imageio_storage_gallery] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }

  /* also export thumbnail: */
  // write with reduced resolution:
  const int max_width  = fdata->max_width;
  const int max_height = fdata->max_height;
  fdata->max_width  = 200;
  fdata->max_height = 200;
  // alter filename with -thumb:
  char *c = filename + strlen(filename);
  for(; c>filename && *c != '.' && *c != '/' ; c--);
  if(c <= filename || *c=='/') c = filename + strlen(filename);
  const char *ext = format->extension(fdata);
  sprintf(c,"-thumb.%s",ext);
  if(dt_imageio_export(imgid, filename, format, fdata, FALSE,FALSE,self,sdata) != 0)
  {
    fprintf(stderr, "[imageio_storage_gallery] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }
  // restore for next image:
  fdata->max_width = max_width;
  fdata->max_height = max_height;

  printf("[export_job] exported to `%s'\n", filename);
  char *trunc = filename + strlen(filename) - 32;
  if(trunc < filename) trunc = filename;
  dt_control_log(_("%d/%d exported to `%s%s'"), num, total, trunc != filename ? ".." : "", trunc);
  return 0;
}

static void
copy_res(const char *src, const char *dst)
{
  char share[DT_MAX_PATH_LEN];
  dt_loc_get_datadir(share, DT_MAX_PATH_LEN);
  gchar *sourcefile = g_build_filename(share, src, NULL);
  char* content = NULL;
  FILE *fin = fopen(sourcefile, "rb");
  FILE *fout = fopen(dst, "wb");

  if(fin && fout)
  {
    fseek(fin,0,SEEK_END);
    int end = ftell(fin);
    rewind(fin);
    content = (char*)g_malloc(sizeof(char)*end);
    if(content == NULL)
      goto END;
    if(fread(content,sizeof(char),end,fin) != end)
      goto END;
    if(fwrite(content,sizeof(char),end,fout) != end)
      goto END;
  }

END:
  if(fout != NULL)
    fclose(fout);
  if(fin != NULL)
    fclose(fin);

  g_free(content);
  g_free(sourcefile);
}

void
finalize_store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *dd)
{
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)dd;
  char filename[DT_MAX_PATH_LEN];
  snprintf(filename, DT_MAX_PATH_LEN, "%s", d->cached_dirname);
  char *c = filename + strlen(filename);

  // also create style/ subdir:
  sprintf(c, "/style");
  g_mkdir_with_parents(filename, 0755);
  sprintf(c, "/style/style.css");
  copy_res("/style/style.css", filename);
  sprintf(c, "/style/favicon.ico");
  copy_res("/style/favicon.ico", filename);
  sprintf(c, "/style/bullet.gif");
  copy_res("/style/bullet.gif", filename);
  sprintf(c, "/style/close.gif");
  copy_res("/style/close.gif", filename);
  sprintf(c, "/style/closelabel.gif");
  copy_res("/style/closelabel.gif", filename);
  sprintf(c, "/style/donate-button.gif");
  copy_res("/style/donate-button.gif", filename);
  sprintf(c, "/style/download-icon.gif");
  copy_res("/style/download-icon.gif", filename);
  sprintf(c, "/style/image-1.jpg");
  copy_res("/style/image-1.jpg", filename);
  sprintf(c, "/style/lightbox.css");
  copy_res("/style/lightbox.css", filename);
  sprintf(c, "/style/loading.gif");
  copy_res("/style/loading.gif", filename);
  sprintf(c, "/style/nextlabel.gif");
  copy_res("/style/nextlabel.gif", filename);
  sprintf(c, "/style/prevlabel.gif");
  copy_res("/style/prevlabel.gif", filename);
  sprintf(c, "/style/thumb-1.jpg");
  copy_res("/style/thumb-1.jpg", filename);

  // create subdir   js for lightbox2 viewer scripts
  sprintf(c, "/js");
  g_mkdir_with_parents(filename, 0755);
  sprintf(c, "/js/builder.js");
  copy_res("/js/builder.js", filename);
  sprintf(c, "/js/effects.js");
  copy_res("/js/effects.js", filename);
  sprintf(c, "/js/lightbox.js");
  copy_res("/js/lightbox.js", filename);
  sprintf(c, "/js/lightbox-web.js");
  copy_res("/js/lightbox-web.js", filename);
  sprintf(c, "/js/prototype.js");
  copy_res("/js/prototype.js", filename);
  sprintf(c, "/js/scriptaculous.js");
  copy_res("/js/scriptaculous.js", filename);

  sprintf(c, "/index.html");

  const char *title = d->title;

  FILE *f = fopen(filename, "wb");
  if(!f) return;
  fprintf(f,
          "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
          "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
          "  <head>\n"
          "    <meta http-equiv=\"Content-type\" content=\"text/html;charset=UTF-8\" />\n"
          "    <link rel=\"shortcut icon\" href=\"style/favicon.ico\" />\n"
          "    <link rel=\"stylesheet\" href=\"style/style.css\" type=\"text/css\" />\n"
          "    <link rel=\"stylesheet\" href=\"style/lightbox.css\" type=\"text/css\" media=\"screen\" />"
          "    <script type=\"text/javascript\" src=\"js/prototype.js\"></script>\n"
          "    <script type=\"text/javascript\" src=\"js/scriptaculous.js?load=effects,builder\"></script>\n"
          "    <script type=\"text/javascript\" src=\"js/lightbox.js\"></script>\n"
          "    <title>%s</title>\n"
          "  </head>\n"
          "  <body>\n"
          "    <div class=\"title\">%s</div>\n"
          "    <div class=\"page\">\n",
          title, title);

  while(d->l)
  {
    pair_t *p = (pair_t *)d->l->data;
    fprintf(f, "%s", p->line);
    free(p);
    d->l = g_list_delete_link(d->l, d->l);
  }

  fprintf(f,
          "        <p style=\"clear:both;\"></p>\n"
          "    </div>\n"
          "    <div class=\"footer\">\n"
          "      <script language=\"JavaScript\" type=\"text/javascript\">\n"
          "      document.write(\"download all: <em>wget -r -np -nc -k \" + document.documentURI + \"</em>\")\n"
          "      </script><br />\n"
          "      created with darktable "PACKAGE_VERSION"\n"
          "    </div>\n"
          "  </body>\n"
          "</html>\n"
         );
  fclose(f);
}

size_t
params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(dt_imageio_gallery_t) - 2*sizeof(void *) - 1024;
}

void init(dt_imageio_module_storage_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_gallery_t,filename,char_path_length);
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_gallery_t,title,char_1024);
#endif
}

void*
get_params(dt_imageio_module_storage_t *self)
{
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)malloc(sizeof(dt_imageio_gallery_t));
  memset(d, 0, sizeof(dt_imageio_gallery_t));
  gallery_t *g = (gallery_t *)self->gui_data;
  d->vp = NULL;
  d->l = NULL;
  dt_variables_params_init(&d->vp);
  const char *text = gtk_entry_get_text(GTK_ENTRY(g->entry));
  g_strlcpy(d->filename, text, DT_MAX_PATH_LEN);
  dt_conf_set_string("plugins/imageio/storage/gallery/file_directory", d->filename);
  text = gtk_entry_get_text(GTK_ENTRY(g->title_entry));
  g_strlcpy(d->title, text, DT_MAX_PATH_LEN);
  dt_conf_set_string("plugins/imageio/storage/gallery/title", d->title);
  return d;
}

void
free_params(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *params)
{
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int
set_params(dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)params;
  gallery_t *g = (gallery_t *)self->gui_data;
  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);
  dt_conf_set_string("plugins/imageio/storage/gallery/file_directory", d->filename);
  gtk_entry_set_text(GTK_ENTRY(g->title_entry), d->title);
  dt_conf_set_string("plugins/imageio/storage/gallery/title", d->title);
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

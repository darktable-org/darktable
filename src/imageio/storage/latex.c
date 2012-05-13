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
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

// gui data
typedef struct latex_t
{
  GtkEntry *entry;
  GtkEntry *title_entry;
}
latex_t;

// saved params
typedef struct dt_imageio_latex_t
{
  char filename[1024];
  char title[1024];
  char cached_dirname[1024]; // expanded during first img store, not stored in param struct.
  dt_variables_params_t *vp;
  GList *l;
}
dt_imageio_latex_t;

// sorted list of all images
typedef struct pair_t
{
  char line[4096];
  int pos;
}
pair_t;


const char*
name ()
{
  return _("latex book template");
}

static void
button_clicked (GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  latex_t *d = (latex_t *)self->gui_data;
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
    char composed[1024];
    snprintf(composed, 1024, "%s/$(FILE_NAME)", dir);
    gtk_entry_set_text(GTK_ENTRY(d->entry), composed);
    dt_conf_set_string("plugins/imageio/storage/latex/file_directory", composed);
    g_free(dir);
  }
  gtk_widget_destroy (filechooser);
}

void
gui_init (dt_imageio_module_storage_t *self)
{
  latex_t *d = (latex_t *)malloc(sizeof(latex_t));
  self->gui_data = (void *)d;
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  GtkWidget *widget;

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  gchar *dir = dt_conf_get_string("plugins/imageio/storage/latex/file_directory");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), dir);
    g_free(dir);
  }
  d->entry = GTK_ENTRY(widget);
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (d->entry));
  g_object_set(G_OBJECT(widget), "tooltip-text", _("enter the path where to create the latex template:\n"
               "$(ROLL_NAME) - roll of the input image\n"
               "$(FILE_DIRECTORY) - directory of the input image\n"
               "$(FILE_NAME) - basename of the input image\n"
               "$(FILE_EXTENSION) - extension of the input image\n"
               "$(SEQUENCE) - sequence number\n"
               "$(YEAR) - year\n"
               "$(MONTH) - month\n"
               "$(DAY) - day\n"
               "$(HOUR) - hour\n"
               "$(MINUTE) - minute\n"
               "$(SECOND) - second\n"
               "$(EXIF_YEAR) - exif year\n"
               "$(EXIF_MONTH) - exif month\n"
               "$(EXIF_DAY) - exif day\n"
               "$(EXIF_HOUR) - exif hour\n"
               "$(EXIF_MINUTE) - exif minute\n"
               "$(EXIF_SECOND) - exif second\n"
               "$(STARS) - star rating\n"
               "$(LABELS) - colorlabels\n"
               "$(PICTURES_FOLDER) - pictures folder\n"
               "$(HOME_FOLDER) - home folder\n"
               "$(DESKTOP_FOLDER) - desktop folder"
                                                  ), (char *)NULL);
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
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (d->title_entry));
  // TODO: support title, author, subject, keywords (collect tags?)
  g_object_set(G_OBJECT(d->title_entry), "tooltip-text", _("enter the title of the book"), (char *)NULL);
  dir = dt_conf_get_string("plugins/imageio/storage/latex/title");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(d->title_entry), dir);
    g_free(dir);
  }
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
  free(self->gui_data);
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
  latex_t *d = (latex_t *)self->gui_data;
  dt_conf_set_string("plugins/imageio/storage/latex/file_directory", gtk_entry_get_text(d->entry));
  dt_conf_set_string("plugins/imageio/storage/latex/title", gtk_entry_get_text(d->title_entry));
}

static gint
sort_pos(pair_t *a, pair_t *b)
{
  return a->pos - b->pos;
}

int
store (dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total)
{
  dt_imageio_latex_t *d = (dt_imageio_latex_t *)sdata;

  char filename[1024]= {0};
  char dirname[1024]= {0};
  dt_image_full_path(imgid, dirname, 1024);
  // we're potentially called in parallel. have sequence number synchronized:
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {

    char tmp_dir[1024];
    dt_variables_expand(d->vp, d->filename, TRUE);
    g_strlcpy(tmp_dir, dt_variables_get_result(d->vp), 1024);

    // if filenamepattern is a directory just add ${FILE_NAME} as default..
    if ( g_file_test(tmp_dir, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR) || ((d->filename+strlen(d->filename)-1)[0]=='/' || (d->filename+strlen(d->filename)-1)[0]=='\\') )
      snprintf (d->filename+strlen(d->filename), 1024-strlen(d->filename), "/$(FILE_NAME)");

    // avoid braindead export which is bound to overwrite at random:
    if(total > 1 && !g_strrstr(d->filename, "$"))
    {
      snprintf(d->filename+strlen(d->filename), 1024-strlen(d->filename), "_$(SEQUENCE)");
    }

    gchar* fixed_path = dt_util_fix_path(d->filename);
    g_strlcpy(d->filename, fixed_path, 1024);
    g_free(fixed_path);

    d->vp->filename = dirname;
    d->vp->jobcode = "export";
    d->vp->imgid = imgid;
    d->vp->sequence = num;
    dt_variables_expand(d->vp, d->filename, TRUE);
    g_strlcpy(filename, dt_variables_get_result(d->vp), 1024);
    g_strlcpy(dirname, filename, 1024);

    const char *ext = format->extension(fdata);
    char *c = dirname + strlen(dirname);
    for(; c>dirname && *c != '/'; c--);
    if(*c == '/') *c = '\0';
    if(g_mkdir_with_parents(dirname, 0755))
    {
      fprintf(stderr, "[imageio_storage_latex] could not create directory: `%s'!\n", dirname);
      dt_control_log(_("could not create directory `%s'!"), dirname);
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
      return 1;
    }

    // store away dir.
    snprintf(d->cached_dirname, 1024, "%s", dirname);

    c = filename + strlen(filename);
    for(; c>filename && *c != '.' && *c != '/' ; c--);
    if(c <= filename || *c=='/') c = filename + strlen(filename);

    sprintf(c,".%s",ext);

    // save image to list, in order:
    pair_t *pair = malloc(sizeof(pair_t));


#if 0 // let's see if we actually want titles and such to be exported:
    char *title = NULL, *description = NULL, *tags = NULL;
    GList *res;

    res = dt_metadata_get(imgid, "Xmp.dc.title", NULL);
    if(res)
    {
      title = res->data;
      g_list_free(res);
    }

    res = dt_metadata_get(imgid, "Xmp.dc.description", NULL);
    if(res)
    {
      description = res->data;
      g_list_free(res);
    }

    unsigned int count = 0;
    res = dt_metadata_get(imgid, "Xmp.dc.subject", &count);
    if(res)
    {
      // don't show the internal tags (darktable|...)
      res = g_list_first(res);
      GList *iter = res;
      while(iter)
      {
        GList *next = g_list_next(iter);
        if(g_str_has_prefix(iter->data, "darktable|"))
        {
          g_free(iter->data);
          res = g_list_delete_link(res, iter);
          count--;
        }
        iter = next;
      }
      tags = dt_util_glist_to_str(", ", res, count);
    }
#endif

    char relfilename[256];
    c = filename + strlen(filename);
    for(; c>filename && *c != '/' ; c--);
    if(*c == '/') c++;
    if(c <= filename) c = filename;
    snprintf(relfilename, 256, "%s", c);

    snprintf(pair->line, 4096,
        "\\vbox to \\imgheight {\n"
        " \\hfil\\includegraphics[width=\\imgwidth,height=\\imgheight,keepaspectratio]{%s}\\hfil\n"
        "  \\vfil\n"
        "}\n"
        "\\drawtrimcorners\n"
        "\\newpage\n\n", relfilename);

    pair->pos = num;
    // g_free(title);
    // g_free(description);
    // g_free(tags);
    d->l = g_list_insert_sorted(d->l, pair, (GCompareFunc)sort_pos);
  } // end of critical block
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  /* export image to file */
  dt_imageio_export(imgid, filename, format, fdata);

  printf("[export_job] exported to `%s'\n", filename);
  char *trunc = filename + strlen(filename) - 32;
  if(trunc < filename) trunc = filename;
  dt_control_log(_("%d/%d exported to `%s%s'"), num, total, trunc != filename ? ".." : "", trunc);
  return 0;
}

static void
copy_res(const char *src, const char *dst)
{
  char share[1024];
  dt_util_get_datadir(share, 1024);
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
finalize_store(dt_imageio_module_storage_t *self, void *dd)
{
  dt_imageio_latex_t *d = (dt_imageio_latex_t *)dd;
  char filename[1024];
  snprintf(filename, 1024, "%s", d->cached_dirname);
  char *c = filename + strlen(filename);

  sprintf(c, "/photobook.cls");
  copy_res("/latex/photobook.cls", filename);
  
  sprintf(c, "/main.tex");

  const char *title = d->title;

  FILE *f = fopen(filename, "wb");
  if(!f) return;
  fprintf(f,
    "\\newcommand{\\dttitle}{%s}\n"
    "\\newcommand{\\dtauthor}{the author}\n"
    "\\newcommand{\\dtsubject}{the matter}\n"
    "\\newcommand{\\dtkeywords}{this, that}\n"
    "\\documentclass{photobook} %% use [draftmode] for preview\n"
    "\\begin{document}\n"
    "\\maketitle\n"
    "\\pagestyle{empty}\n",
    title);

  while(d->l)
  {
    pair_t *p = (pair_t *)d->l->data;
    fprintf(f, "%s", p->line);
    free(p);
    d->l = g_list_delete_link(d->l, d->l);
  }

  fprintf(f,
    "\\end{document}"
    "%% created with darktable "PACKAGE_VERSION"\n"
    );
  fclose(f);
}

void*
get_params(dt_imageio_module_storage_t *self, int* size)
{
  dt_imageio_latex_t *d = (dt_imageio_latex_t *)malloc(sizeof(dt_imageio_latex_t));
  memset(d, 0, sizeof(dt_imageio_latex_t));
  // have to return the size of the struct to store (i.e. without all the variable pointers at the end)
  *size = sizeof(dt_imageio_latex_t) - 2*sizeof(void *) - 1024;
  latex_t *g = (latex_t *)self->gui_data;
  d->vp = NULL;
  d->l = NULL;
  dt_variables_params_init(&d->vp);
  const char *text = gtk_entry_get_text(GTK_ENTRY(g->entry));
  g_strlcpy(d->filename, text, 1024);
  dt_conf_set_string("plugins/imageio/storage/latex/file_directory", d->filename);
  text = gtk_entry_get_text(GTK_ENTRY(g->title_entry));
  g_strlcpy(d->title, text, 1024);
  dt_conf_set_string("plugins/imageio/storage/latex/title", d->title);
  return d;
}

void
free_params(dt_imageio_module_storage_t *self, void *params)
{
  dt_imageio_latex_t *d = (dt_imageio_latex_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int
set_params(dt_imageio_module_storage_t *self, void *params, int size)
{
  if(size != sizeof(dt_imageio_latex_t) - 2*sizeof(void *) - 1024) return 1;
  dt_imageio_latex_t *d = (dt_imageio_latex_t *)params;
  latex_t *g = (latex_t *)self->gui_data;
  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);
  dt_conf_set_string("plugins/imageio/storage/latex/file_directory", d->filename);
  gtk_entry_set_text(GTK_ENTRY(g->title_entry), d->title);
  dt_conf_set_string("plugins/imageio/storage/latex/title", d->title);
  return 0;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

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
#include "common/exif.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "lua/dt_lua.h"
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gstdio.h>

DT_MODULE(1)

// gui data
typedef struct disk_t
{
  GtkEntry *entry;
}
disk_t;

// saved params
typedef struct dt_imageio_disk_t
{
  char filename[DT_MAX_PATH_LEN];
  dt_variables_params_t *vp;
}
dt_imageio_disk_t;


const char*
name ()
{
  return _("file on disk");
}

static void
button_clicked (GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
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
    dt_conf_set_string("plugins/imageio/storage/disk/file_directory", composed);
    g_free(dir);
  }
  gtk_widget_destroy (filechooser);
}

typedef struct completion_spec
{
  gchar *varname;
  gchar *description;
} completion_spec;

typedef enum
{
  COMPL_VARNAME = 0,
  COMPL_DESCRIPTION
} CompletionSpecCol;

/** Called when the user selects a variable from the autocomplete list. */
static gboolean
on_match_select(GtkEntryCompletion *widget,
                GtkTreeModel *model,
                GtkTreeIter *iter,
                gpointer user_data)
{

  const gchar *varname;
  GtkEditable *e = (GtkEditable*) gtk_entry_completion_get_entry(widget);
  gchar *s = gtk_editable_get_chars(e, 0, -1);
  gint cur_pos = gtk_editable_get_position(e);
  gint p = cur_pos;
  gchar *end;
  gint del_end_pos = -1;

  GValue value = {0, };
  gtk_tree_model_get_value(model, iter, COMPL_VARNAME, &value);
  varname = g_value_get_string(&value);

  for (p = cur_pos; p - 2 > 0; p--)
  {
    if (strncmp(s + p - 2, "$(", 2) == 0)
    {
      break;
    }
  }

  end = s + cur_pos;

  if (end)
  {
    del_end_pos = end - s + 1;
  }
  else
  {
    del_end_pos = cur_pos;
  }

  gchar *addtext = (gchar*) g_malloc(strlen(varname) + 2);
  sprintf(addtext, "%s)", varname);

  gtk_editable_delete_text(e, p, del_end_pos);
  gtk_editable_insert_text(e, addtext, -1, &p);
  gtk_editable_set_position(e, p);
  g_value_unset(&value);
  return TRUE;
}

/**
 * Case insensitive substring search for a completion match.
 *
 * Based on the default matching function in GtkEntryCompletion.
 *
 * This function is called once for each iter in the GtkEntryCompletion's
 * list of completion entries (model).
 *
 * @param completion Completion object to apply this function on
 * @param key        Complete string from the GtkEntry.
 * @param iter       Item in list of autocomplete database to compare key against.
 * @param user_data  Unused.
 */
static gboolean
on_match_func (GtkEntryCompletion *completion, const gchar *key,
               GtkTreeIter *iter, gpointer user_data)
{
  gchar *item = NULL;
  gchar *normalized_string;
  gchar *case_normalized_string;
  gboolean ret = FALSE;
  GtkTreeModel *model = gtk_entry_completion_get_model (completion);

  GtkEditable *e = (GtkEditable*) gtk_entry_completion_get_entry(completion);
  gint cur_pos = gtk_editable_get_position(e); /* returns 1..* */
  gint p = cur_pos;
  gint var_start;
  gboolean var_present = FALSE;

  for (p = cur_pos; p >= 0; p--)
  {
    gchar *ss = gtk_editable_get_chars(e, p, cur_pos);
    if (strncmp(ss, "$(", 2) == 0)
    {
      var_start = p+2;
      var_present = TRUE;
      g_free(ss);
      break;
    }
    g_free(ss);
  }

  if (var_present)
  {
    gchar *varname = gtk_editable_get_chars(e, var_start, cur_pos);

    gtk_tree_model_get (model, iter, COMPL_VARNAME, &item, -1);

    if (item != NULL)
    {
      // Do utf8-safe case insensitive string compare.
      // Shamelessly stolen from GtkEntryCompletion.
      normalized_string = g_utf8_normalize (item, -1, G_NORMALIZE_ALL);

      if (normalized_string != NULL)
      {
        case_normalized_string = g_utf8_casefold (normalized_string, -1);

        if (!g_ascii_strncasecmp(varname, case_normalized_string,
                                 strlen (varname)))
          ret = TRUE;

        g_free (case_normalized_string);
      }
      g_free (normalized_string);
    }
    g_free (varname);
  }
  g_free (item);

  return ret;
}


static gchar *
build_tooltip_text (const gchar *header, const completion_spec *compl_list)
{
  const unsigned int tooltip_len = 1024;
  gchar *tt = g_malloc0_n(tooltip_len, sizeof(gchar));
  completion_spec const *p;

  g_strlcat(tt, header, sizeof(tt)*tooltip_len);
  g_strlcat(tt, "\n", sizeof(tt)*tooltip_len);

  for(p = compl_list; p->description != NULL; p++)
  {
    g_strlcat(tt, p->description, sizeof(tt)*tooltip_len);
    g_strlcat(tt, "\n", sizeof(tt)*tooltip_len);
  }

  return tt;
}

void
gui_init (dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)malloc(sizeof(disk_t));
  self->gui_data = (void *)d;
  self->widget = gtk_hbox_new(FALSE, 5);
  GtkWidget *widget;

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, TRUE, TRUE, 0);
  gchar *dir = dt_conf_get_string("plugins/imageio/storage/disk/file_directory");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), dir);
    g_free(dir);
  }

  GtkEntryCompletion *completion = gtk_entry_completion_new();
  GtkListStore *model = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeIter iter;

  gtk_entry_completion_set_text_column(completion, COMPL_DESCRIPTION);
  gtk_entry_set_completion(GTK_ENTRY(widget), completion);
  g_signal_connect(G_OBJECT(completion), "match-selected", G_CALLBACK(on_match_select), NULL);

  completion_spec compl_list[] =
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
    { "EXIF_YEAR", _("$(EXIF_YEAR) - exif year") },
    { "EXIF_MONTH", _("$(EXIF_MONTH) - exif month") },
    { "EXIF_DAY", _("$(EXIF_DAY) - exif day") },
    { "EXIF_HOUR", _("$(EXIF_HOUR) - exif hour") },
    { "EXIF_MINUTE", _("$(EXIF_MINUTE) - exif minute") },
    { "EXIF_SECOND", _("$(EXIF_SECOND) - exif second") },
    { "STARS", _("$(STARS) - star rating") },
    { "LABELS", _("$(LABELS) - colorlabels") },
    { "PICTURES_FOLDER", _("$(PICTURES_FOLDER) - pictures folder") },
    { "HOME", _("$(HOME) - home folder") },
    { "DESKTOP", _("$(DESKTOP) - desktop folder") },
    { NULL, NULL }
  };


  for(completion_spec *l=compl_list; l && l->varname; l++)
  {
    gtk_list_store_append(model, &iter);
    gtk_list_store_set(model, &iter, COMPL_VARNAME, l->varname,
                       COMPL_DESCRIPTION, l->description, -1);
  }
  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model));
  gtk_entry_completion_set_match_func(completion, on_match_func, NULL, NULL);

  char *tooltip_text = build_tooltip_text (
                         _("enter the path where to put exported images\nrecognized variables:"),
                         compl_list);

  d->entry = GTK_ENTRY(widget);
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (d->entry));
  g_object_set(G_OBJECT(widget), "tooltip-text", tooltip_text, (char *)NULL);
  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, 0);
  gtk_widget_set_size_request(widget, 18, 18);
  g_object_set(G_OBJECT(widget), "tooltip-text", _("select directory"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);

  g_free(tooltip_text);
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
  free(self->gui_data);
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  // global default can be annoying:
  // gtk_entry_set_text(GTK_ENTRY(d->entry), "$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)");
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", gtk_entry_get_text(d->entry));
}

int
store (dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)sdata;

  char filename[DT_MAX_PATH_LEN]= {0};
  char dirname[DT_MAX_PATH_LEN]= {0};
  dt_image_full_path(imgid, dirname, DT_MAX_PATH_LEN);
  int fail = 0;
  // we're potentially called in parallel. have sequence number synchronized:
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {

    // if filenamepattern is a directory just let att ${FILE_NAME} as default..
    if ( g_file_test(d->filename, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR) || ((d->filename+strlen(d->filename))[0]=='/' || (d->filename+strlen(d->filename))[0]=='\\') )
      snprintf (d->filename+strlen(d->filename), DT_MAX_PATH_LEN-strlen(d->filename), "$(FILE_NAME)");

    // avoid braindead export which is bound to overwrite at random:
    if(total > 1 && !g_strrstr(d->filename, "$"))
    {
      snprintf(d->filename+strlen(d->filename), DT_MAX_PATH_LEN-strlen(d->filename), "_$(SEQUENCE)");
    }

    gchar* fixed_path = dt_util_fix_path(d->filename);
    g_strlcpy(d->filename, fixed_path, DT_MAX_PATH_LEN);
    g_free(fixed_path);

    d->vp->filename = dirname;
    d->vp->jobcode = "export";
    d->vp->imgid = imgid;
    d->vp->sequence = num;
    dt_variables_expand(d->vp, d->filename, TRUE);
    g_strlcpy(filename, dt_variables_get_result(d->vp), DT_MAX_PATH_LEN);
    g_strlcpy(dirname, filename, DT_MAX_PATH_LEN);

    const char *ext = format->extension(fdata);
    char *c = dirname + strlen(dirname);
    for(; c>dirname && *c != '/'; c--);
    if(*c == '/') *c = '\0';
    if(g_mkdir_with_parents(dirname, 0755))
    {
      fprintf(stderr, "[imageio_storage_disk] could not create directory: `%s'!\n", dirname);
      dt_control_log(_("could not create directory `%s'!"), dirname);
      fail = 1;
      goto failed;
    }
    if(g_access(dirname, W_OK) != 0)
    {
      fprintf(stderr, "[imageio_storage_disk] could not write to directory: `%s'!\n", dirname);
      dt_control_log(_("could not write to directory `%s'!"), dirname);
      fail = 1;
      goto failed;
    }

    c = filename + strlen(filename);
    // remove everything after the last '.'. this destroys any file name with dots in it since $(FILE_NAME) already comes without the original extension.
//     for(; c>filename && *c != '.' && *c != '/' ; c--);
//     if(c <= filename || *c=='/') c = filename + strlen(filename);

    sprintf(c,".%s",ext);

    /* prevent overwrite of files */
    int seq=1;
failed:
    if (!fail && g_file_test (filename,G_FILE_TEST_EXISTS))
    {
      do
      {
        sprintf(c,"_%.2d.%s",seq,ext);
        seq++;
      }
      while (g_file_test (filename,G_FILE_TEST_EXISTS));
    }

  } // end of critical block
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(fail) return 1;

  /* export image to file */
  if(dt_imageio_export(imgid, filename, format, fdata) != 0)
  {
    fprintf(stderr, "[imageio_storage_disk] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }

  /* now write xmp into that container, if possible */
  if(dt_exif_xmp_attach(imgid, filename) != 0)
  {
    fprintf(stderr, "[imageio_storage_disk] could not attach xmp data to file: `%s'!\n", filename);
    // don't report that one to gui, as some formats (pfm, ppm, exr) just don't support
    // writing xmp via exiv2, so it might not be to worry.
    return 1;
  }

  printf("[export_job] exported to `%s'\n", filename);
  char *trunc = filename + strlen(filename) - 32;
  if(trunc < filename) trunc = filename;
  dt_control_log(_("%d/%d exported to `%s%s'"), num, total, trunc != filename ? ".." : "", trunc);
  return 0;
}

void*
get_params(dt_imageio_module_storage_t *self, int* size)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)malloc(sizeof(dt_imageio_disk_t));
  memset(d, 0, sizeof(dt_imageio_disk_t));
  // have to return the size of the struct to store (i.e. without all the variable pointers at the end)
  *size = sizeof(dt_imageio_disk_t) - sizeof(void *);
  disk_t *g = (disk_t *)self->gui_data;
  d->vp = NULL;
  dt_variables_params_init(&d->vp);
  const char *text = gtk_entry_get_text(GTK_ENTRY(g->entry));
  g_strlcpy(d->filename, text, DT_MAX_PATH_LEN);
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", d->filename);
  return d;
}

void
free_params(dt_imageio_module_storage_t *self, void *params)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int
set_params(dt_imageio_module_storage_t *self, void *params, int size)
{
  if(size != sizeof(dt_imageio_disk_t) - sizeof(void *)) return 1;
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  disk_t *g = (disk_t *)self->gui_data;
  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", d->filename);
  return 0;
}
/********************************************************************
LUA STUFF
*********************************************************************/
static int dt_lua_tostring(lua_State *L) {
	lua_pushstring(L,"storage/disk");
	return 1;
}


static const luaL_Reg lua_meta[] = {
	{"__tostring", dt_lua_tostring },
	{0,0}

};
int lua_init(lua_State * L) {
	lua_newtable(L);
	luaL_setfuncs(L,lua_meta,0);
	lua_newuserdata(L,1); // placeholder we can't use a table because we can't prevent assignment
	lua_pushvalue(L,-2);
	lua_setmetatable(L,-2);
	dt_lua_push_darktable_lib(L);
	dt_lua_goto_subtable(L,"storage");
	lua_pushvalue(L,-2);
	lua_setfield(L,-2,"disk");
	return 0;
}





// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

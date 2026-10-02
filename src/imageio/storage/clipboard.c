/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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
#include "common/file_location.h"
#include "common/image.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/storage/imageio_storage_api.h"
#include <glib/gstdio.h>
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#ifdef GDK_WINDOWING_WIN32
#include "win/dtwin.h"
#include <gdk/gdkwin32.h>
#endif

DT_MODULE(1)

typedef enum _clipboard_target_t
{
  _TARGET_NATIVE = 0,
  _TARGET_IMAGE,
  _TARGET_URIS,
  _TARGET_GNOME_FILES
} _clipboard_target_t;

// saved params
typedef struct dt_imageio_clipboard_t
{
  gchar *dirname;
  gchar *mime;
  GList *files;
  gboolean failed;
} dt_imageio_clipboard_t;

typedef struct _clipboard_content_t
{
  gchar *dirname;
  gchar *mime;
  GList *files;
  GdkPixbuf *pixbuf;
} _clipboard_content_t;

// its files outlive clipboard ownership, other apps may still refer to them
static _clipboard_content_t *_published = NULL;
static _clipboard_content_t *_owned = NULL;

static void _remove_files(gchar *dirname, GList *files)
{
  for(GList *iter = files; iter; iter = g_list_next(iter))
    g_unlink((gchar *)iter->data);
  if(dirname) g_rmdir(dirname);
}

static void _remove_dir(const gchar *dirname)
{
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(!dir) return;
  const gchar *name;
  while((name = g_dir_read_name(dir)))
  {
    gchar *path = g_build_filename(dirname, name, (char *)NULL);
    if(g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
      _remove_dir(path);
    else
      g_unlink(path);
    g_free(path);
  }
  g_dir_close(dir);
  g_rmdir(dirname);
}

static void _content_free(_clipboard_content_t *c)
{
  g_list_free_full(c->files, g_free);
  if(c->pixbuf) g_object_unref(c->pixbuf);
  g_free(c->dirname);
  g_free(c->mime);
  g_free(c);
}

const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("copy to clipboard");
}

void gui_init(dt_imageio_module_storage_t *self)
{
}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  // keep _published: clearing GTK's selection empties the macOS and Windows clipboards
}

void gui_reset(dt_imageio_module_storage_t *self)
{
}

void init(dt_imageio_module_storage_t *self)
{
}

size_t params_size(dt_imageio_module_storage_t *self)
{
  return 0;
}

void *get_params(dt_imageio_module_storage_t *self)
{
  return g_malloc0(sizeof(dt_imageio_clipboard_t));
}

int set_params(dt_imageio_module_storage_t *self,
               const void *params,
               const int size)
{
  if(size != self->params_size(self)) return 1;
  return 0;
}

void free_params(dt_imageio_module_storage_t *self,
                 dt_imageio_module_data_t *params)
{
  if(!params) return;
  dt_imageio_clipboard_t *d = (dt_imageio_clipboard_t *)params;
  _remove_files(d->dirname, d->files);
  g_list_free_full(d->files, g_free);
  g_free(d->dirname);
  g_free(d->mime);
  g_free(d);
}

gboolean supported(struct dt_imageio_module_storage_t *storage,
                   struct dt_imageio_module_format_t *format)
{
  return g_strcmp0(format->mime(NULL), "x-copy") != 0;
}

int initialize_store(dt_imageio_module_storage_t *self,
                     dt_imageio_module_data_t *data,
                     dt_imageio_module_format_t **format,
                     dt_imageio_module_data_t **fdata,
                     GList **images,
                     const gboolean high_quality,
                     const gboolean upscale)
{
  dt_imageio_clipboard_t *d = (dt_imageio_clipboard_t *)data;

  if(!darktable.gui)
  {
    dt_print(DT_DEBUG_ALWAYS, "[imageio_storage_clipboard] no clipboard without gui");
    return 1;
  }

  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));
  gchar *parent = g_build_filename(cachedir, "clipboard", (char *)NULL);

  // leftovers from the previous session
  static gsize purged = 0;
  if(g_once_init_enter(&purged))
  {
    _remove_dir(parent);
    g_once_init_leave(&purged, 1);
  }

  g_mkdir_with_parents(parent, 0700);
  d->dirname = g_build_filename(parent, "XXXXXX", (char *)NULL);
  g_free(parent);
  if(!g_mkdtemp(d->dirname))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_storage_clipboard] could not create directory: `%s'!",
             d->dirname);
    dt_control_log(_("could not create directory `%s'!"), d->dirname);
    g_free(d->dirname);
    d->dirname = NULL;
    return 1;
  }

  d->mime = g_strdup((*format)->mime(*fdata));
  return 0;
}

int store(dt_imageio_module_storage_t *self,
          dt_imageio_module_data_t *sdata,
          const dt_imgid_t imgid,
          dt_imageio_module_format_t *format,
          dt_imageio_module_data_t *fdata,
          const int num,
          const int total,
          const gboolean high_quality,
          const gboolean upscale,
          const gboolean is_scaling,
          const double scale_factor,
          const gboolean export_masks,
          dt_colorspaces_color_profile_type_t icc_type,
          const gchar *icc_filename,
          dt_iop_color_intent_t icc_intent,
          dt_export_metadata_t *metadata)
{
  dt_imageio_clipboard_t *d = (dt_imageio_clipboard_t *)sdata;

  if(!d->dirname) return 1;

  char basename[PATH_MAX] = { 0 };
  dt_image_full_path(imgid, basename, sizeof(basename), NULL);
  gchar *filename = g_path_get_basename(basename);
  g_strlcpy(basename, filename, sizeof(basename));
  g_free(filename);

  dt_image_path_append_version(imgid, basename, sizeof(basename));

  gchar *dot = g_strrstr(basename, ".");
  if(dot) *dot = '\0';

  gchar *leaf = total > 1
    ? g_strdup_printf("%04d_%s.%s", num, basename, format->extension(fdata))
    : g_strdup_printf("%s.%s", basename, format->extension(fdata));
  gchar *file = g_build_filename(d->dirname, leaf, (char *)NULL);
  g_free(leaf);

  if(dt_imageio_export(imgid, file, format, fdata, high_quality,
                       upscale, is_scaling, scale_factor,
                       TRUE, export_masks, icc_type,
                       icc_filename, icc_intent, self, sdata, num, total, metadata) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_storage_clipboard] could not export to file: `%s'!", file);
    dt_control_log(_("could not export to file `%s'!"), file);
    g_free(file);
    d->failed = TRUE;
    return 1;
  }

  dt_control_log(ngettext("%d/%d exported to clipboard", "%d/%d exported to clipboard", num),
                 num, total);

  DT_OMP_PRAGMA(critical)
  d->files = g_list_insert_sorted(d->files, file, (GCompareFunc)g_strcmp0);

  return 0;
}

static void _clipboard_get(GtkClipboard *clipboard,
                           GtkSelectionData *selection_data,
                           const guint info,
                           gpointer user_data)
{
  _clipboard_content_t *c = (_clipboard_content_t *)user_data;

  switch(info)
  {
    case _TARGET_NATIVE:
    {
      gchar *contents = NULL;
      gsize length = 0;
      if(g_file_get_contents((gchar *)c->files->data, &contents, &length, NULL))
        gtk_selection_data_set(selection_data,
                               gtk_selection_data_get_target(selection_data),
                               8, (const guchar *)contents, length);
      g_free(contents);
      break;
    }
    case _TARGET_IMAGE:
    {
      if(!c->pixbuf)
        c->pixbuf = gdk_pixbuf_new_from_file((gchar *)c->files->data, NULL);
      if(c->pixbuf)
        gtk_selection_data_set_pixbuf(selection_data, c->pixbuf);
      break;
    }
    case _TARGET_URIS:
    case _TARGET_GNOME_FILES:
    {
      const guint n = g_list_length(c->files);
      gchar **uris = g_malloc0_n(n + 1, sizeof(gchar *));
      int k = 0;
      for(GList *iter = c->files; iter; iter = g_list_next(iter))
        uris[k++] = g_filename_to_uri((gchar *)iter->data, NULL, NULL);

      if(info == _TARGET_URIS)
        gtk_selection_data_set_uris(selection_data, uris);
      else
      {
        gchar *list = g_strjoinv("\n", uris);
        gchar *text = g_strconcat("copy\n", list, NULL);
        gtk_selection_data_set(selection_data,
                               gtk_selection_data_get_target(selection_data),
                               8, (const guchar *)text, strlen(text));
        g_free(text);
        g_free(list);
      }
      g_strfreev(uris);
      break;
    }
  }
}

static void _clipboard_clear(GtkClipboard *clipboard,
                             gpointer user_data)
{
  _clipboard_content_t *c = (_clipboard_content_t *)user_data;
  if(c == _owned) _owned = NULL;
  g_clear_object(&c->pixbuf);
}

static gboolean _set_native_files(GList *files)
{
  // release GTK before the old content is freed
  if(_owned) gtk_clipboard_clear(gtk_clipboard_get_default(gdk_display_get_default()));

#if defined(GDK_WINDOWING_QUARTZ)
  return dt_osx_clipboard_set_files(files);
#elif defined(GDK_WINDOWING_WIN32)
  GdkWindow *window = gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui));
  return dtwin_clipboard_set_files(files, (HWND)gdk_win32_window_get_handle(window));
#else
  return FALSE;
#endif
}

static gboolean _set_gtk(_clipboard_content_t *c, const guint n)
{
  GtkTargetList *list = gtk_target_list_new(NULL, 0);
  gint n_images = 0;

  // not gtk_target_list_add_image_targets(), it adds types like text/ico
  if(n == 1)
  {
    if(c->mime && g_str_has_prefix(c->mime, "image/"))
    {
      gtk_target_list_add(list, gdk_atom_intern(c->mime, FALSE), 0, _TARGET_NATIVE);
      n_images++;
    }
    if(g_strcmp0(c->mime, "image/png")
       && gdk_pixbuf_get_file_info((gchar *)c->files->data, NULL, NULL))
    {
      gtk_target_list_add(list, gdk_atom_intern_static_string("image/png"), 0, _TARGET_IMAGE);
      n_images++;
    }
  }
  gtk_target_list_add_uri_targets(list, _TARGET_URIS);
  gtk_target_list_add(list, gdk_atom_intern_static_string("x-special/gnome-copied-files"),
                      0, _TARGET_GNOME_FILES);

  gint n_targets = 0;
  GtkTargetEntry *targets = gtk_target_table_new_from_list(list, &n_targets);
  gtk_target_list_unref(list);

  GtkClipboard *clipboard = gtk_clipboard_get_default(gdk_display_get_default());
  const gboolean ok = gtk_clipboard_set_with_data(clipboard, targets, n_targets,
                                                  _clipboard_get, _clipboard_clear, c);
  if(ok)
  {
    // the files are temporary, only store the image data
    if(n_images) gtk_clipboard_set_can_store(clipboard, targets, n_images);
    _owned = c;
  }

  gtk_target_table_free(targets, n_targets);
  return ok;
}

static gboolean _clipboard_set(gpointer user_data)
{
  _clipboard_content_t *c = (_clipboard_content_t *)user_data;
  const guint n = g_list_length(c->files);

  // GTK only passes the first URI on to macOS and Windows
#if defined(GDK_WINDOWING_QUARTZ) || defined(GDK_WINDOWING_WIN32)
  const gboolean native = n > 1;
#else
  const gboolean native = FALSE;
#endif

  if(native ? _set_native_files(c->files) : _set_gtk(c, n))
  {
    if(_published)
    {
      _remove_files(_published->dirname, _published->files);
      _content_free(_published);
    }
    _published = c;
    dt_control_log(ngettext("%d image copied to clipboard",
                            "%d images copied to clipboard", n), n);
  }
  else
  {
    dt_control_log(_("could not copy to clipboard"));
    _remove_files(c->dirname, c->files);
    _content_free(c);
  }

  return G_SOURCE_REMOVE;
}

void finalize_store(dt_imageio_module_storage_t *self,
                    dt_imageio_module_data_t *params)
{
  dt_imageio_clipboard_t *d = (dt_imageio_clipboard_t *)params;

  if(!d->files || d->failed)
  {
    _remove_files(d->dirname, d->files);
    g_list_free_full(d->files, g_free);
    g_free(d->dirname);
    d->files = NULL;
    d->dirname = NULL;
    return;
  }

  _clipboard_content_t *c = g_malloc0(sizeof(_clipboard_content_t));
  c->dirname = d->dirname;
  c->mime = d->mime;
  c->files = d->files;
  d->dirname = NULL;
  d->mime = NULL;
  d->files = NULL;

  g_main_context_invoke(NULL, _clipboard_set, c);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

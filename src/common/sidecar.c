/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common/sidecar.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean _readable_sidecar(const char *path)
{
  GStatBuf info;
  return !g_access(path, R_OK) && !g_stat(path, &info)
    && S_ISREG(info.st_mode) && info.st_size > 0;
}

static gboolean _numbered_sidecar(const char *entry,
                                  const char *basename,
                                  const size_t stem_length,
                                  const char *suffix)
{
  if(strncmp(entry, basename, stem_length) || entry[stem_length] != '_') return FALSE;

  const char *digits = entry + stem_length + 1;
  const char *end = digits;
  while(g_ascii_isdigit(*end)) end++;
  /* Upstream sidecars use at least two digits (01, 02, ..., 100). Keep the
   * same requirement as the desktop _[0-9]*[0-9] search and reject extra
   * underscores, signs, letters and non-ASCII digits. */
  return end - digits >= 2 && !strcmp(end, suffix);
}

static gint _compare_paths(gconstpointer a, gconstpointer b)
{
  return strcmp(a, b);
}

GList *dt_sidecar_find_duplicates(const char *filename)
{
  if(!filename || !filename[0] || g_str_has_suffix(filename, G_DIR_SEPARATOR_S)) return NULL;

  GList *files = NULL;
  gchar *unnumbered = g_strconcat(filename, ".xmp", NULL);
  if(_readable_sidecar(unnumbered)) files = g_list_prepend(files, unnumbered);
  else g_free(unnumbered);

  gchar *basename = g_path_get_basename(filename);
  const char *extension = strrchr(basename, '.');
  /* Versioned sidecars insert their number before the image extension. An
   * extensionless filename can only have an unnumbered sidecar. In particular,
   * a dot in a parent directory must never be treated as the extension. */
  if(!extension)
  {
    g_free(basename);
    return files;
  }

  gchar *directory = g_path_get_dirname(filename);
  GDir *dir = g_dir_open(directory, 0, NULL);
  g_free(directory);
  if(!dir)
  {
    g_free(basename);
    return files;
  }

  const size_t stem_length = extension - basename;
  gchar *suffix = g_strconcat(extension, ".xmp", NULL);
  /* Preserve the caller's path spelling, including bare relative filenames,
   * instead of adding './' or canonicalizing parent directories. */
  gchar *prefix = g_strndup(filename, strlen(filename) - strlen(basename));
  GList *numbered = NULL;
  const char *entry;
  while((entry = g_dir_read_name(dir)))
  {
    if(!_numbered_sidecar(entry, basename, stem_length, suffix)) continue;
    gchar *path = g_strconcat(prefix, entry, NULL);
    if(_readable_sidecar(path)) numbered = g_list_prepend(numbered, path);
    else g_free(path);
  }
  g_dir_close(dir);
  g_free(prefix);
  g_free(suffix);
  g_free(basename);

  return g_list_concat(files, g_list_sort(numbered, _compare_paths));
}

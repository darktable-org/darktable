/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib.h>
#include <string.h>

/* Android installs native libraries into one trusted, read-only directory.
 * Category prefixes prevent an iop, format and dependency with the same base
 * name from being confused. Logical plugin names stay identical to desktop. */
static inline const char *dt_module_android_prefix(const char *subdir)
{
  if(!strcmp(subdir, "/plugins")) return "libdt_iop_";
  if(!strcmp(subdir, "/plugins/imageio/format")) return "libdt_format_";
  if(!strcmp(subdir, "/plugins/imageio/storage")) return "libdt_storage_";
  if(!strcmp(subdir, "/views")) return "libdt_view_";
  if(!strcmp(subdir, "/plugins/lighttable")) return "libdt_lib_";
  return NULL;
}

static inline gchar *dt_module_name_from_filename(const char *filename,
                                                  const char *prefix,
                                                  const char *suffix)
{
  if(!filename || !prefix || !suffix || strchr(filename, '/') || strchr(filename, '\\')) return NULL;
  const size_t prefix_size = strlen(prefix), suffix_size = strlen(suffix), size = strlen(filename);
  if(size <= prefix_size + suffix_size || !g_str_has_prefix(filename, prefix)
     || !g_str_has_suffix(filename, suffix)) return NULL;
  return g_strndup(filename + prefix_size, size - prefix_size - suffix_size);
}

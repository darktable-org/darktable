/*
    This file is part of darktable,
    Copyright (C) 2012-2025 darktable developers.

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

#pragma once

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

G_BEGIN_DECLS


#define CONFIGDIR_CREATION_FAILED 1
#define CACHEDIR_CREATION_FAILED 2
#define TMPDIR_CREATION_FAILED 3


// Returns the users home directory
gchar *dt_loc_get_home_dir(const gchar *user);

// Print the already-resolved darktable.* paths plus library_path,
// either as an aligned human-readable table or as a single line of
// ready-to-splice --flag value pairs.
void dt_loc_print_paths(FILE *out, const char *library_path, gboolean as_flags);

// Init all dirs
uint8_t dt_loc_init(const char *datadir,
                    const char *moduledir,
                    const char *localedir,
                    const char *configdir,
                    const char *cachedir,
                    const char *tmpdir);

// Init systemwide data dir
void dt_loc_init_datadir(const char *application_directory,
                         const char *datadir);

// Init the plugin dir
void dt_loc_init_plugindir(const char *application_directory,
                           const char *plugindir);

// Init the locale dir
void dt_loc_init_localedir(const char *application_directory,
                           const char *localedir);

// Init share dir
void dt_loc_init_sharedir(const char* application_directory);

// Init user tmp dir
gboolean dt_loc_init_tmp_dir(const char *tmpdir);

// Init user config dir
gboolean dt_loc_init_user_config_dir(const char *configdir);

// Init user cache dir
gboolean dt_loc_init_user_cache_dir(const char *cachedir);

// expand a folder path typed or pasted by the user: trim surrounding blanks,
// remove one pair of surrounding double quotes and expand a leading ~;
// returns NULL when nothing is left, free with g_free()
gchar *dt_loc_expand_user_path(const char *value);

// whether path is absolute and depends neither on the current directory nor,
// on windows, on the current drive
gboolean dt_loc_path_is_absolute(const char *path);

typedef enum dt_loc_cache_dir_check_t
{
  DT_LOC_CACHE_DIR_USABLE = 0,
  DT_LOC_CACHE_DIR_NOT_ABSOLUTE,
  DT_LOC_CACHE_DIR_MISSING,
  DT_LOC_CACHE_DIR_NO_ACCESS
} dt_loc_cache_dir_check_t;

// check a cache dir value (see dt_loc_expand_user_path()) without using it:
// it must be an absolute path to an existing folder darktable can list and
// write to
dt_loc_cache_dir_check_t dt_loc_check_user_cache_dir(const char *cachedir);

// switch the user cache dir to an existing, writable absolute folder, which is
// never created. on failure the current cache dir is kept and FALSE is returned
gboolean dt_loc_set_user_cache_dir(const char *cachedir);

typedef enum dt_loc_cache_dir_source_t
{
  DT_LOC_CACHE_DIR_DEFAULT = 0,
  DT_LOC_CACHE_DIR_COMMAND_LINE,
  DT_LOC_CACHE_DIR_PREF
} dt_loc_cache_dir_source_t;

// where the cache dir in use comes from: the default, --cachedir, or the
// cachedir preference set by dt_loc_set_user_cache_dir()
dt_loc_cache_dir_source_t dt_loc_get_user_cache_dir_source(void);

// whether the cache dir in use comes from this preference value: the folder it
// names was applied at startup, or the default is used for a blank value
gboolean dt_loc_user_cache_dir_is_from(const char *value);

// the default user cache dir, used when neither --cachedir nor the
// preference is set. free with g_free()
gchar *dt_loc_get_default_user_cache_dir(void);

// Init specific dir. Default value is appended to application_directory
// if application_directory is not NULL.
gchar *dt_loc_init_generic(const char *absolute_value,
                           const char *application_directory,
                           const char *default_value);

// Checking if we can open the directory.
gboolean dt_check_opendir(const char* text, const char* directory);

// temporary backward_compatibility
void dt_loc_get_datadir(char *datadir, size_t bufsize);
void dt_loc_get_sharedir(char *sharedir, size_t bufsize);
void dt_loc_get_kerneldir(char *kerneldir, size_t bufsize);
void dt_loc_get_plugindir(char *plugindir, size_t bufsize);
void dt_loc_get_localedir(char *localedir, size_t bufsize);
void dt_loc_get_tmp_dir(char *tmpdir, size_t bufsize);
void dt_loc_get_user_config_dir(char *configdir, size_t bufsize);
void dt_loc_get_user_cache_dir(char *cachedir, size_t bufsize);
// folder for local copies: the cache dir resolved at startup from --cachedir
// or the default, never the cachedir preference
void dt_loc_get_user_local_copy_dir(char *dir, size_t bufsize);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#include "gui/gtk.h"
#include <stdlib.h>

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#ifdef _WIN32
#include "win/main_wrapper.h"
#include "common/datetime.h"
#endif

int main(int argc, char *argv[])
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif
#ifdef _WIN32
  // on Windows we have a hard time showing stuff printed to stdout/stderr to the user.
  // because of that we write it to a log file.
  char datetime[DT_DATETIME_EXIF_LENGTH];
  dt_datetime_now_to_exif(datetime);

  // make sure to not redirect output when the output is already being redirected, either to a file or a pipe.
  int out_type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
  int err_type = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
  gboolean redirect_output = ((out_type != FILE_TYPE_DISK && out_type != FILE_TYPE_PIPE) &&
                              (err_type != FILE_TYPE_DISK && err_type != FILE_TYPE_PIPE));

  for(int k = 1; k < argc; k++)
  {
    // For simple arguments do not redirect stdout
    if(!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h") || !strcmp(argv[k], "/?") || !strcmp(argv[k], "--version"))
    {
      redirect_output = FALSE;
      break;
    }
  }

  if(redirect_output)
  {
    // something like C:\Users\username\AppData\Local\Microsoft\Windows\Temporary Internet Files\darktable\darktable-log.txt
    char *logdir = g_build_filename(g_get_user_cache_dir(), "darktable", NULL);
    char *logfile = g_build_filename(logdir, "darktable-log.txt", NULL);

    g_mkdir_with_parents(logdir, 0700);

    g_freopen(logfile, "a", stdout);
    dup2(fileno(stdout), fileno(stderr));

    // We don't need the console window anymore, free it
    // This ensures that only darktable's main window will be visible
    FreeConsole();

    g_free(logdir);
    g_free(logfile);

    // don't buffer stdout/stderr. we have basically two options: unbuffered or line buffered.
    // unbuffered keeps the order in which things are printed but concurrent threads printing can lead to intermangled output. ugly.
    // line buffered should keep lines together but in my tests the order of things no longer matches. ugly and potentially confusing.
    // thus we are doing the thing that is just ugly (in rare cases) but at least not confusing.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("========================================\n");
    printf("version: %s\n", darktable_package_string);
    printf("start: %s\n", datetime);
    printf("\n");
  }

  // make sure GTK client side decoration is disabled, otherwise windows resizing issues can be observed
  g_setenv("GTK_CSD", "0", TRUE);
#endif

  if(dt_init(argc, argv, TRUE, TRUE, NULL)) exit(1);
  dt_gui_gtk_run(darktable.gui);

#ifdef _WIN32
  if(redirect_output)
  {
    printf("\n");
    printf("end:   %s\n", datetime);
    printf("========================================\n");
    printf("\n");
  }
#endif

  exit(0);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "develop/develop.h"
#include "common/gimp.h"
#include "common/image_cache.h"
#include "gui/gtk.h"
#include <stdlib.h>

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#ifdef _WIN32
#include "win/main_wrapper.h"
#include "common/datetime.h"

typedef struct dt_alloc_console_options_t
{
  int mode;
  BOOL use_show_window;
  WORD show_window;
} dt_alloc_console_options_t;

typedef HRESULT(WINAPI *dt_alloc_console_with_options_t)(
  dt_alloc_console_options_t *options,
  int *result);

static gboolean _allocate_console(void)
{
  const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  const FARPROC allocation_proc = GetProcAddress(kernel, "AllocConsoleWithOptions");
  dt_alloc_console_with_options_t alloc_with_options = NULL;
  memcpy(&alloc_with_options, &allocation_proc, sizeof(alloc_with_options));

  if(alloc_with_options)
  {
    // mode 2 requests ALLOC_CONSOLE_MODE_NO_WINDOW, available since Windows 11 24H2
    dt_alloc_console_options_t options = { 2, FALSE, SW_HIDE };
    if(SUCCEEDED(alloc_with_options(&options, NULL)) && GetConsoleCP() != 0)
      return TRUE;
  }

  return AllocConsole();
}
#endif

#ifdef __APPLE__
int apple_main(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif
#ifdef _WIN32
  // On Windows we have a hard time showing stuff printed to stdout/stderr to the user.
  // Because of that we write it to a log file.
  char datetime[DT_DATETIME_EXIF_LENGTH];
  dt_datetime_now_to_exif(datetime);

  // Users are more accustomed to the ISO 8601 format than to separating
  // the year, month, and day with a colon (which is the Exif convention)
  datetime[4] = datetime[7] = '-';

  // Make sure to not redirect output when the output is already
  // being redirected, either to a file or a pipe.
  if(GetConsoleCP() == 0)
  {
    const HANDLE initial_input_handle = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE initial_output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE initial_error_handle = GetStdHandle(STD_ERROR_HANDLE);
    const DWORD initial_in_type = GetFileType(initial_input_handle);
    const DWORD initial_out_type = GetFileType(initial_output_handle);
    const DWORD initial_err_type = GetFileType(initial_error_handle);
    const gboolean input_redirected =
      initial_in_type == FILE_TYPE_DISK || initial_in_type == FILE_TYPE_PIPE;
    const gboolean output_redirected =
      initial_out_type == FILE_TYPE_DISK || initial_out_type == FILE_TYPE_PIPE;
    const gboolean error_redirected =
      initial_err_type == FILE_TYPE_DISK || initial_err_type == FILE_TYPE_PIPE;

    if(_allocate_console())
    {
      if(input_redirected)
        SetStdHandle(STD_INPUT_HANDLE, initial_input_handle);
      else
        g_freopen("CONIN$", "r", stdin);

      if(output_redirected)
        SetStdHandle(STD_OUTPUT_HANDLE, initial_output_handle);
      else
        g_freopen("CONOUT$", "w", stdout);

      if(error_redirected)
        SetStdHandle(STD_ERROR_HANDLE, initial_error_handle);
      else
        g_freopen("CONOUT$", "w", stderr);
    }
  }

  int out_type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
  int err_type = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
  gboolean redirect_output = ((out_type != FILE_TYPE_DISK && out_type != FILE_TYPE_PIPE) &&
                              (err_type != FILE_TYPE_DISK && err_type != FILE_TYPE_PIPE));

  for(int k = 1; k < argc; k++)
  {
    // For simple arguments do not redirect stdout
    if(!strcmp(argv[k], "--help") ||
       !strcmp(argv[k], "-h") ||
       !strcmp(argv[k], "/?") ||
       !strcmp(argv[k], "--version") ||
       !strcmp(argv[k], "--gimp"))
    {
      redirect_output = FALSE;
      break;
    }
  }

  if(redirect_output)
  {
    // NOTE! If the log file path is changed, reflect the changes at least
    // in the console message in 'usage()' of 'common\darktable.c'
    char *logdir = g_build_filename(g_get_home_dir(), "Documents", "Darktable", NULL);
    char *logfile = g_build_filename(logdir, "darktable-log.txt", NULL);

    g_mkdir_with_parents(logdir, 0700);

    g_freopen(logfile, "a", stdout);
    dup2(fileno(stdout), STDOUT_FILENO);
    dup2(fileno(stdout), STDERR_FILENO);
    dup2(fileno(stdout), fileno(stderr));
    const HANDLE output_handle = (HANDLE)_get_osfhandle(fileno(stdout));
    SetStdHandle(STD_OUTPUT_HANDLE, output_handle);
    SetStdHandle(STD_ERROR_HANDLE, output_handle);

    g_free(logdir);
    g_free(logfile);

    // Don't buffer stdout/stderr. We have basically two options:
    // unbuffered or line buffered.
    // Unbuffered keeps the order in which things are printed but concurrent
    // threads printing can lead to intermangled output. Ugly.
    // Line buffered should keep lines together but the order of things
    // no longer matches. Ugly and potentially confusing.
    // Thus we are doing the thing that is just ugly (in rare cases)
    // but at least not confusing.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("========================================\n");
    printf("version: %s\n", darktable_package_string);
    printf("start: %s\n", datetime);
    printf("\n");
  }

  // Make sure GTK client side decoration is disabled,
  // otherwise windows resizing issues can be observed.
  g_setenv("GTK_CSD", "0", TRUE);
#endif

  if(dt_init(argc, argv, TRUE, TRUE, NULL))
  {
    if(dt_gimpmode())
      printf("\n<<<gimp\nerror\ngimp>>>\n");
    exit(1);
  }

  if(dt_check_gimpmode_ok("version"))
  {
    printf("\n<<<gimp\n%d\ngimp>>>\n", DT_GIMP_VERSION);
    exit(0);
  }

  if(dt_check_gimpmode("version")
    || (dt_check_gimpmode("file") && !dt_check_gimpmode_ok("file"))
    || (dt_check_gimpmode("thumb") && !dt_check_gimpmode_ok("thumb"))
    || darktable.gimp.error)
  {
    printf("\n<<<gimp\nerror\ngimp>>>\n");
    exit(1);
  }

  if(dt_check_gimpmode_ok("file"))
  {
    const dt_imgid_t id = dt_gimp_load_darkroom(darktable.gimp.path);
    if(!dt_is_valid_imgid(id))
      darktable.gimp.error = TRUE;
  }

  if(dt_check_gimpmode_ok("thumb"))
  {
    const dt_imgid_t id = dt_gimp_load_image(darktable.gimp.path);
    if(dt_is_valid_imgid(id))
      darktable.gimp.error = !dt_export_gimp_file(id);
    else
      darktable.gimp.error = TRUE;

    return darktable.gimp.error ? 1 : 0;
  }

  if(!dt_gimpmode() || dt_check_gimpmode_ok("file"))
    dt_gui_gtk_run(darktable.gui);

  dt_cleanup();

  if(dt_gimpmode() && darktable.gimp.error)
    printf("\n<<<gimp\nerror\ngimp>>>\n");

#ifdef _WIN32
  if(redirect_output)
  {
    printf("\n");
    printf("end:   %s\n", datetime);
    printf("========================================\n");
    printf("\n");
  }
#endif

  const int exitcode = dt_gimpmode() ? (darktable.gimp.error ? 1 : 0) : 0;
  exit(exitcode);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

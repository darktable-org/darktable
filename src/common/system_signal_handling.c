/*
    This file is part of darktable,
    Copyright (C) 2016-2023 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"     // for darktable, darktable_t
#include "common/file_location.h" // for dt_loc_get_datadir
#include "common/system_signal_handling.h"
#include <errno.h>       // for errno
#include <glib.h>        // for g_free, g_printerr, g_strdup_printf
#include <glib/gstdio.h> // for g_unlink
#include <limits.h>      // for PATH_MAX
#include <signal.h>      // for signal, SIGSEGV, SIG_ERR
#include <stddef.h>      // for NULL
#include <stdio.h>       // for dprintf, fprintf, stderr
#include <string.h>      // for strerror
#include <unistd.h>      // for STDOUT_FILENO, close, execlp, fork

#ifdef __linux__
#include <sys/prctl.h> // for PR_SET_PTRACER, prctl
#endif

#ifndef _WIN32
#include <sys/wait.h> // for waitpid
#endif

#ifdef _WIN32
#include <exchndl.h>
#endif //_WIN32

#if defined(__linux__) && !defined(PR_SET_PTRACER)
#define PR_SET_PTRACER 0x59616d61
#endif

typedef void(dt_signal_handler_t)(int);

#if !defined(__APPLE__) && !defined(_WIN32)
static dt_signal_handler_t *_dt_sigsegv_old_handler = NULL;
#endif

// deer graphicsmagick, please stop messing with the stuff that you should not be touching at all.
// based on GM's InitializeMagickSignalHandlers() and MagickSignalHandlerMessage()
#if !defined(_WIN32)
static const int _signals_to_preserve[] = { SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGABRT, SIGBUS, SIGFPE,
                                            SIGPIPE, SIGALRM, SIGTERM, SIGCHLD, SIGXCPU, SIGXFSZ };
#else
static const int _signals_to_preserve[] = { SIGABRT, SIGFPE, SIGILL, SIGINT, SIGSEGV, SIGTERM };
static LPTOP_LEVEL_EXCEPTION_FILTER _dt_exceptionfilter_old_handler = NULL;
#endif //! defined (_WIN32)

#define _NUM_SIGNALS_TO_PRESERVE (sizeof(_signals_to_preserve) / sizeof(_signals_to_preserve[0]))
static dt_signal_handler_t *_orig_sig_handlers[_NUM_SIGNALS_TO_PRESERVE] = { NULL };

#if(defined(__FreeBSD_version) && (__FreeBSD_version < 800071)) || (defined(OpenBSD) && (OpenBSD < 201305))       \
    || defined(__SUNOS__)
static int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)))
{
  va_list ap;
  FILE *f = fdopen(fd, "a");
  va_start(ap, fmt);
  int rc = vfprintf(f, fmt, ap);
  fclose(f);
  va_end(ap);
  return rc;
}
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
static void _dt_sigsegv_handler(int param)
{
  pid_t pid;
  gchar *name_used;
  int fout;
  gboolean delete_file = FALSE;
  char datadir[PATH_MAX] = { 0 };

  if((fout = g_file_open_tmp("darktable_bt_XXXXXX.txt", &name_used, NULL)) == -1)
    fout = STDOUT_FILENO; // just print everything to stdout

  dprintf(fout, "this is %s reporting a segfault:\n\n", darktable_package_string);

  if(fout != STDOUT_FILENO) close(fout);

  dt_loc_get_datadir(datadir, sizeof(datadir));
  gchar *pid_arg = g_strdup_printf("%d", (int)getpid());
  gchar *comm_arg = g_strdup_printf("%s/gdb_commands", datadir);
  gchar *log_arg = g_strdup_printf("set logging on %s", name_used);

  if((pid = fork()) != -1)
  {
    if(pid)
    {
#ifdef __linux__
      // Allow the child to ptrace us
      prctl(PR_SET_PTRACER, pid, 0, 0, 0);
#endif
      waitpid(pid, NULL, 0);
      g_printerr("backtrace written to %s\n", name_used);
    }
    else
    {
      if(execlp("gdb", "gdb", darktable.progname, pid_arg, "-batch", "-ex", log_arg, "-x", comm_arg, NULL))
      {
        delete_file = TRUE;
        g_printerr("an error occurred while trying to execute gdb. please check if gdb is installed on your "
                   "system.\n");
      }
    }
  }
  else
  {
    delete_file = TRUE;
    g_printerr("an error occurred while trying to execute gdb.\n");
  }

  if(delete_file) g_unlink(name_used);
  g_free(pid_arg);
  g_free(comm_arg);
  g_free(log_arg);
  g_free(name_used);

  /* pass it further to the old handler*/
  _dt_sigsegv_old_handler(param);
}
#endif

static int _times_handlers_were_set = 0;

#if defined(_WIN32)

static LONG WINAPI dt_toplevel_exception_handler(PEXCEPTION_POINTERS pExceptionInfo)
{
  gchar *name_used;
  int fout;
  BOOL ok;

  // Find a filename for the backtrace file
  if((fout = g_file_open_tmp("darktable_bt_XXXXXX.txt", &name_used, NULL)) == -1)
    fout = STDOUT_FILENO; // just print everything to stdout

  FILE *fd = fdopen(fout, "wb");
  fprintf(fd, "this is %s reporting an exception:\n\n", darktable_package_string);
  fclose(fd);

  if(fout != STDOUT_FILENO) close(fout);


  // Set up logfile name
  ok = ExcHndlSetLogFileNameA(name_used);
  if(!ok)
  {
    g_printerr("backtrace logfile cannot be set to %s\n", name_used);
  }
  else
  {
    gchar *exception_message = g_strdup_printf("An unhandled exception occurred.\nBacktrace will be written to: %s "
                                               "after you click on the OK button.\nIf you report this issue, "
                                               "please share this backtrace with the developers.\n",
                                               name_used);
    wchar_t *wexception_message = g_utf8_to_utf16(exception_message, -1, NULL, NULL, NULL);
    MessageBoxW(0, wexception_message, L"Error!", MB_OK);
    g_free(exception_message);
    g_free(wexception_message);
  }

  g_free(name_used);

  // finally call the original exception handler (which should be drmingw's exception handler)
  return _dt_exceptionfilter_old_handler(pExceptionInfo);
}

void dt_set_unhandled_exception_handler_win()
{
  // Set up drming's exception handler
  ExcHndlInit();
}
#endif // defined(_WIN32)


void dt_set_signal_handlers()
{
  _times_handlers_were_set++;

  dt_signal_handler_t *prev;

  if(1 == _times_handlers_were_set)
  {
    // save original handlers
    for(int i = 0; i < _NUM_SIGNALS_TO_PRESERVE; i++)
    {
      const int signum = _signals_to_preserve[i];

      prev = signal(signum, SIG_DFL);

      if(SIG_ERR == prev) prev = SIG_DFL;

      _orig_sig_handlers[i] = prev;
    }
  }

  // restore handlers
  for(int i = 0; i < _NUM_SIGNALS_TO_PRESERVE; i++)
  {
    const int signum = _signals_to_preserve[i];

    (void)signal(signum, _orig_sig_handlers[i]);
  }

#if !defined(__APPLE__) && !defined(_WIN32)
  // now, set our SIGSEGV handler.
  // FIXME: what about SIGABRT?
  prev = signal(SIGSEGV, &_dt_sigsegv_handler);

  if(SIG_ERR != prev)
  {
    // we want the most original previous signal handler.
    if(1 == _times_handlers_were_set) _dt_sigsegv_old_handler = prev;
  }
  else
  {
    const int errsv = errno;
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_set_signal_handlers] error: signal(SIGSEGV) returned SIG_ERR: %i (%s)\n",
             errsv,
             strerror(errsv));
  }
#elif !defined(__APPLE__)
  /*
  Set up exception handler for backtrace on Windows
  Works when there is NO SIGSEGV handler installed

  SetUnhandledExceptionFilter handler must be saved on the first invocation
  as GraphicsMagick is overwriting SetUnhandledExceptionFilter and all other signals in InitializeMagick()
  Eventually InitializeMagick() should be fixed upstream not to ignore existing exception handlers
  */

  dt_set_unhandled_exception_handler_win();
  if(1 == _times_handlers_were_set)
  {
    // Save UnhandledExceptionFilter handler which just has been set up
    // This should be drmingw's exception handler
    _dt_exceptionfilter_old_handler = SetUnhandledExceptionFilter(dt_toplevel_exception_handler);
  }
  // Restore our UnhandledExceptionFilter handler no matter what GM is doing
  SetUnhandledExceptionFilter(dt_toplevel_exception_handler);

#endif //!defined(__APPLE__) && !defined(_WIN32)
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


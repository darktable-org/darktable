/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2010--2012 henrik andersson.
    copyright (c) 2010--2013 tobias ellinghaus.
    copyright (c) 2012--2013 parafin
    copyright (c) 2014,2016 Roman Lebedev.

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

#ifndef __WIN32__
#include <sys/wait.h> // for waitpid
#endif

#if defined(__linux__) && !defined(PR_SET_PTRACER)
#define PR_SET_PTRACER 0x59616d61
#endif

#if !defined(__APPLE__) && !defined(__WIN32__)
typedef void(dt_signal_handler_t)(int);
// static dt_signal_handler_t *_dt_sigill_old_handler = NULL;
static dt_signal_handler_t *_dt_sigsegv_old_handler = NULL;

// deer graphicsmagick, please stop messing with the stuff that you should not be touching at all.
// based on GM's InitializeMagickSignalHandlers() and MagickSignalHandlerMessage()
static const int _signals_to_preserve[] = { SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGABRT, SIGBUS, SIGFPE,
                                            SIGPIPE, SIGALRM, SIGTERM, SIGCHLD, SIGXCPU, SIGXFSZ };
#define _NUM_SIGNALS_TO_PRESERVE (sizeof(_signals_to_preserve) / sizeof(_signals_to_preserve[0]))
static dt_signal_handler_t *_orig_sig_handlers[_NUM_SIGNALS_TO_PRESERVE] = { NULL };
#endif

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

#if !defined(__APPLE__) && !defined(__WIN32__)
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
void dt_set_signal_handlers()
{
  _times_handlers_were_set++;

#if !defined(__APPLE__) && !defined(__WIN32__)
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
    fprintf(stderr, "[dt_set_signal_handlers] error: signal(SIGSEGV) returned SIG_ERR: %i (%s)\n", errsv,
            strerror(errsv));
  }
#endif
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

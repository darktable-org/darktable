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
#include "mcp/mcp_jsonrpc.h"
#include "mcp/mcp_tools.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include "win/main_wrapper.h" // provides wmain() for the -municode CRT entry
#endif

static gboolean _has_flag(char **argv, int argc, const char *flag)
{
  for(int i = 0; i < argc; i++)
    if(!g_strcmp0(argv[i], flag)) return TRUE;
  return FALSE;
}

static gboolean _has_conf(char **argv, int argc, const char *key_prefix)
{
  for(int i = 0; i + 1 < argc; i++)
    if(!g_strcmp0(argv[i], "--conf")
       && g_str_has_prefix(argv[i + 1], key_prefix))
      return TRUE;
  return FALSE;
}

int main(int argc, char **argv)
{
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  // everything after "--core" is forwarded verbatim to dt_init, so all darktable
  // core options (--configdir, --cachedir, --library, --conf, -d, ...) work as
  // they do for darktable itself
  int core_start = argc;
  for(int i = 1; i < argc; i++)
    if(!g_strcmp0(argv[i], "--core")) { core_start = i + 1; break; }
  const int n_core = argc - core_start;
  char **core = (n_core > 0) ? &argv[core_start] : NULL;

  // build the synthetic argv, injecting defaults only when the user did not
  // supply them: a throwaway in-memory library and no sidecar writes
  GPtrArray *m = g_ptr_array_new();
  g_ptr_array_add(m, (gpointer)"darktable-mcp");
  if(!_has_flag(core, n_core, "--library"))
  {
    g_ptr_array_add(m, (gpointer)"--library");
    g_ptr_array_add(m, (gpointer)":memory:");
  }
  if(!_has_conf(core, n_core, "write_sidecar_files="))
  {
    g_ptr_array_add(m, (gpointer)"--conf");
    g_ptr_array_add(m, (gpointer)"write_sidecar_files=never");
  }
  for(int i = 0; i < n_core; i++) g_ptr_array_add(m, core[i]);
  const int m_argc = m->len;
  char **m_arg = (char **)m->pdata;

  // keep the protocol channel clean: libdarktable prints to stdout in places, so
  // reserve the real stdout for JSON-RPC responses and give darktable stderr
  fflush(stdout);
  const int saved_fd = dup(STDOUT_FILENO);
  FILE *proto_out = fdopen(saved_fd, "w");
  dup2(STDERR_FILENO, STDOUT_FILENO);

  if(dt_init(m_argc, m_arg, FALSE, TRUE, NULL))
  {
    fprintf(stderr, "darktable-mcp: dt_init failed\n");
    g_ptr_array_free(m, TRUE);
    return 1;
  }

  // tool descriptions and schemas live in the data folder, editable without a rebuild
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  gchar *tools_file = g_build_filename(datadir, "mcp-tools.json", NULL);
  if(mcp_tools_load(tools_file) < 0)
    fprintf(stderr, "darktable-mcp: no tools available (could not load '%s')\n",
            tools_file);
  g_free(tools_file);

  mcp_jsonrpc_loop(stdin, proto_out ? proto_out : stdout);

  dt_cleanup();
  if(proto_out) fflush(proto_out);
  g_ptr_array_free(m, TRUE);
  return 0;
}

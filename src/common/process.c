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
#include "common/process.h"

#include <glib.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

int dt_exec_command_sync(const char *cmd)
{
#ifdef _WIN32
  if(!cmd) return -1;

  // system() on Windows runs the command through cmd.exe and propagates its
  // exit code. Keep that semantics (so cmd builtins, batch files, redirection
  // and shell separators behave exactly as before), but use CREATE_NO_WINDOW so
  // a console-subsystem child spawned from a console-less GUI parent
  // (darktable.exe frees its console on startup) doesn't get a fresh, visible
  // console window (issue #17193).
  gchar *utf8 = g_strdup_printf("cmd.exe /d /s /c \"%s\"", cmd);
  gunichar2 *wline = g_utf8_to_utf16(utf8, -1, NULL, NULL, NULL);
  g_free(utf8);
  if(!wline) return -1;

  STARTUPINFOW si = { 0 };
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = { 0 };
  // CreateProcessW writes into the buffer, so pass our copy, not `cmd`.
  const BOOL ok = CreateProcessW(NULL, (LPWSTR)wline, NULL, NULL, FALSE,
                                 CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  g_free(wline);
  if(!ok) return -1;

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)exit_code;
#else
  return cmd ? system(cmd) : -1;
#endif
}

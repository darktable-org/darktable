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
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>

#define DT_COMMAND_PATH_MAX 32768

static gboolean _resolve_command_interpreter(wchar_t *interpreter,
                                             const DWORD capacity)
{
  DWORD length = GetEnvironmentVariableW(L"COMSPEC", interpreter, capacity);
  if(length == 0)
  {
    length = GetSystemDirectoryW(interpreter, capacity);
    if(length == 0 || length >= capacity) return FALSE;

    static const wchar_t suffix[] = L"\\cmd.exe";
    if(length + G_N_ELEMENTS(suffix) > capacity) return FALSE;
    wcscat(interpreter, suffix);
    return TRUE;
  }
  if(length >= capacity) return FALSE;

  if(length >= 2 && interpreter[0] == L'"' && interpreter[length - 1] == L'"')
  {
    memmove(interpreter, interpreter + 1, (length - 2) * sizeof(wchar_t));
    interpreter[length - 2] = L'\0';
  }

  wchar_t resolved[DT_COMMAND_PATH_MAX];
  if(!wcschr(interpreter, L'\\') && !wcschr(interpreter, L'/')
     && !wcschr(interpreter, L':'))
  {
    wchar_t search_path[DT_COMMAND_PATH_MAX];
    const DWORD path_length = GetEnvironmentVariableW(L"PATH", search_path,
                                                       G_N_ELEMENTS(search_path));
    if(path_length == 0 || path_length >= G_N_ELEMENTS(search_path)) return FALSE;
    const DWORD found = SearchPathW(search_path, interpreter, L".exe",
                                    G_N_ELEMENTS(resolved), resolved, NULL);
    if(found == 0 || found >= G_N_ELEMENTS(resolved)) return FALSE;
  }
  else
  {
    const DWORD full_length = GetFullPathNameW(interpreter, G_N_ELEMENTS(resolved),
                                               resolved, NULL);
    if(full_length == 0 || full_length >= G_N_ELEMENTS(resolved)) return FALSE;
  }

  wcscpy(interpreter, resolved);
  return TRUE;
}

static wchar_t *_build_command_line(const wchar_t *interpreter,
                                    const char *cmd)
{
  gunichar2 *wide_command = g_utf8_to_utf16(cmd, -1, NULL, NULL, NULL);
  if(!wide_command) return NULL;

  const size_t capacity = wcslen(interpreter)
                          + wcslen((const wchar_t *)wide_command) + 32;
  wchar_t *command_line = g_new(wchar_t, capacity);
  const int written = swprintf(command_line, capacity,
                               L"\"%ls\" /d /s /c \"%ls\"",
                               interpreter, (const wchar_t *)wide_command);
  g_free(wide_command);
  if(written < 0)
  {
    g_free(command_line);
    return NULL;
  }
  return command_line;
}
#endif

int dt_exec_command_sync(const char *cmd)
{
#ifdef _WIN32
  if(!cmd) return -1;

  wchar_t interpreter[DT_COMMAND_PATH_MAX];
  if(!_resolve_command_interpreter(interpreter, G_N_ELEMENTS(interpreter))) return -1;

  // system() on Windows runs the command through cmd.exe and propagates its
  // exit code; keep that semantics (so cmd builtins, batch files, redirection
  // and shell separators behave exactly as before), but use CREATE_NO_WINDOW so
  // a console-subsystem child spawned from a console-less GUI parent
  // (darktable.exe frees its console on startup) doesn't get a fresh, visible
  // console window (issue #17193)
  wchar_t *command_line = _build_command_line(interpreter, cmd);
  if(!command_line) return -1;

  STARTUPINFOW si = { 0 };
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = { 0 };
  // CreateProcessW writes into the buffer, so pass our copy, not `cmd`
  const BOOL ok = CreateProcessW(interpreter, command_line, NULL, NULL, FALSE,
                                 CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  g_free(command_line);
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

/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "common/opencl.h"

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#ifdef _WIN32
#include <conio.h>
#include "win/main_wrapper.h"
#endif

int main(int argc, char *arg[])
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif
  int result = 1;
  // only used to force-init opencl, so we want these options:
  char *m_arg[] = { "-d", "opencl", "--library", ":memory:"};
  const int m_argc = sizeof(m_arg) / sizeof(m_arg[0]);
  char **argv = malloc(sizeof(arg[0]) * argc + sizeof(m_arg));
  if(!argv) goto end;
  for(int i = 0; i < argc; i++)
    argv[i] = arg[i];
  for(int i = 0; i < m_argc; i++)
    argv[argc + i] = m_arg[i];
  argc += m_argc;
  if(dt_init(argc, argv, FALSE, FALSE, NULL)) goto end;
  dt_cleanup();
  free(argv);

  result = 0;
end:

#ifdef _WIN32
  printf("\npress any key to exit\n");
  FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
  getch();
#endif

  exit(result);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

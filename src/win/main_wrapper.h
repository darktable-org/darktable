#pragma once

#include <glib.h>

// On Windows the command line arguments are ANSI encoded. We want UTF-8 in dt though.
// including this file will add a wrapper that acts together with linker switch -municode

int main(int argc, char *argv[]);

int wmain(int argc, wchar_t *argv[])
{
  char **_argv = g_malloc0((argc + 1) * sizeof(char *));
  for(int i = 0; i < argc; i++)
    _argv[i] = g_utf16_to_utf8(argv[i], -1, NULL, NULL, NULL);
  int res = main(argc, _argv);
  g_strfreev(_argv);
  return res;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


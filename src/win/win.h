#pragma once

#ifdef __MSVCRT_VERSION__
#undef __MSVCRT_VERSION__
#endif
#define __MSVCRT_VERSION__ 0x0700

#undef __STRICT_ANSI__
#define XMD_H

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>

// ugly hack to make our code work. windows.h has some terrible includes which define these things
// that clash with our variable names. Including them can be omitted when adding
// #define WIN32_LEAN_AND_MEAN
// before including windows.h, but then we will miss some defines needed for libraries like libjpeg.
#undef near
#undef grp2
#undef interface

#define sleep(n) Sleep(1000 * n)
#define HAVE_BOOLEAN

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

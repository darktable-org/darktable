#ifndef __WIN_H__
#define __WIN_H__

#ifdef __MSVCRT_VERSION__
#undef __MSVCRT_VERSION__
#endif
#define __MSVCRT_VERSION__ 0x0700

#undef __STRICT_ANSI__
#define XMD_H
#include <windows.h>

// ugly hack to make our code work. windows.h has some terrible includes which define these things
// that clash with our variable names. Including them can be omitted when adding
// #define WIN32_LEAN_AND_MEAN
// before including windows.h, but then we will miss some defines needed for libraries like libjpeg.
#undef near
#undef grp2

#define sleep(n) Sleep(1000 * n)
#define HAVE_BOOLEAN

#endif

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

#pragma once

// CMake uses config.cmake.h to generate config.h within the build folder.
#ifndef DARKTABLE_CONFIG_H
#define DARKTABLE_CONFIG_H

#define GETTEXT_PACKAGE "darktable"
#define DARKTABLE_LOCALEDIR "@RUNTIME_LOCALE_DIR@"
#define DARKTABLE_TMPDIR "@TMP_DIR@"
#define DARKTABLE_CACHEDIR "@CACHE_DIR@"

#define DARKTABLE_LIBDIR "@RUNTIME_INSTALL_PREFIX@/@LIB_INSTALL@/darktable"
#define DARKTABLE_DATADIR "@RUNTIME_INSTALL_PREFIX@/@SHARE_INSTALL@/darktable"
#define DARKTABLE_SHAREDIR "@RUNTIME_INSTALL_PREFIX@/@SHARE_INSTALL@"

#define SHARED_MODULE_PREFIX "@CMAKE_SHARED_MODULE_PREFIX@"
#define SHARED_MODULE_SUFFIX "@CMAKE_SHARED_MODULE_SUFFIX@"

#ifndef __GNUC_PREREQ
// on OSX, gcc-4.6 and clang chokes if this is not here.
#if defined __GNUC__ && defined __GNUC_MINOR__
#define __GNUC_PREREQ(maj, min) ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#else
#define __GNUC_PREREQ(maj, min) 0
#endif
#endif

#if defined(_OPENMP) && __GNUC_PREREQ(4, 9)
#define OPENMP_SIMD_
#define SIMD() simd
#else
#define SIMD()
#endif

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

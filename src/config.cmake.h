// CMake uses config.cmake.h to generate config.h within the build folder.
#pragma once

#include <stddef.h>

// clang-format off
// it butchers @@ and ${} :(

#define PACKAGE_NAME "@CMAKE_PROJECT_NAME@"
#define PACKAGE_BUGREPORT "darktable-dev@lists.darktable.org"

// these will be defined in build/src/version_gen.c
extern const char darktable_package_version[];
extern const char darktable_package_string[];
extern const char darktable_last_commit_year[];

static const char *dt_supported_extensions[] __attribute__((unused)) = {"@DT_SUPPORTED_EXTENSIONS_STRING@", NULL};

#define GETTEXT_PACKAGE "darktable"
#define DARKTABLE_LOCALEDIR "@CMAKE_INSTALL_FULL_LOCALEDIR@"

#define DARKTABLE_LIBDIR "@CMAKE_INSTALL_FULL_LIBDIR@/darktable"
#define DARKTABLE_DATADIR "@CMAKE_INSTALL_FULL_DATAROOTDIR@/darktable"
#define DARKTABLE_SHAREDIR "@CMAKE_INSTALL_FULL_DATAROOTDIR@"

#define SHARED_MODULE_PREFIX "@CMAKE_SHARED_MODULE_PREFIX@"
#define SHARED_MODULE_SUFFIX "@CMAKE_SHARED_MODULE_SUFFIX@"

#define WANTED_STACK_SIZE (@WANTED_STACK_SIZE@)
#define WANTED_THREADS_STACK_SIZE (@WANTED_THREADS_STACK_SIZE@)

#define ISO_CODES_LOCATION "@ISO_CODES_LOCATION@"
#define ISO_CODES_LOCALEDIR "@ISO_CODES_LOCALEDIR@"

// clang-format on

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

// see http://clang.llvm.org/docs/LanguageExtensions.html
#ifndef __has_feature      // Optional of course.
#define __has_feature(x) 0 // Compatibility with non-clang compilers.
#endif
#ifndef __has_extension
#define __has_extension __has_feature // Compatibility with pre-3.0 compilers.
#endif

// see https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

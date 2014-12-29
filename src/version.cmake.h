#pragma once

// CMake uses version.cmake.h to generate version.h within the build folder.
#ifndef DARKTABLE_VERSION_H
#define DARKTABLE_VERSION_H

#include "version_gen.h"

#define PACKAGE_NAME "@CMAKE_PROJECT_NAME@"
#define PACKAGE_STRING PACKAGE_NAME " " PACKAGE_VERSION
#define PACKAGE_BUGREPORT "darktable-devel@lists.sf.net"

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

# Remmina - The GTK+ Remote Desktop Client
#
# Copyright (C) 2011 Marc-Andre Moreau
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, 
# Boston, MA 02111-1307, USA.

include(LibFindMacros)

set(_GTK3_found_all true)

# Gtk

libfind_pkg_check_modules(PC_GTK3 gtk+-3.0)

if(NOT PC_GTK3_FOUND)
  set(_GTK3_found_all false)
endif()

if(GTK3_FIND_VERSION)
  cmake_minimum_required(VERSION 2.6.2)
  set(GTK3_FAILED_VERSION_CHECK true)

  if(GTK3_FIND_VERSION_EXACT)
    if(PC_GTK3_VERSION VERSION_EQUAL GTK3_FIND_VERSION)
      set(GTK3_FAILED_VERSION_CHECK false)
    endif()
  else()
    if(PC_GTK3_VERSION VERSION_EQUAL GTK3_FIND_VERSION OR
       PC_GTK3_VERSION VERSION_GREATER GTK3_FIND_VERSION)
      set(GTK3_FAILED_VERSION_CHECK false)
    endif()
  endif()

  if(GTK3_FAILED_VERSION_CHECK)
    if(GTK3_FIND_REQUIRED AND NOT GTK3_FIND_QUIETLY)
        if(GTK3_FIND_VERSION_EXACT)
            message(FATAL_ERROR "GTK3 version check failed.  Version ${PC_GTK3_VERSION} was found, version ${GTK3_FIND_VERSION} is needed exactly.")
        else(GTK3_FIND_VERSION_EXACT)
            message(FATAL_ERROR "GTK3 version check failed.  Version ${PC_GTK3_VERSION} was found, at least version ${GTK3_FIND_VERSION} is required")
        endif(GTK3_FIND_VERSION_EXACT)
    endif(GTK3_FIND_REQUIRED AND NOT GTK3_FIND_QUIETLY)

    set(_GTK3_found_all false)
  endif(GTK3_FAILED_VERSION_CHECK)
endif(GTK3_FIND_VERSION)

find_path(GTK3_INCLUDE_DIR NAMES gtk/gtk.h
  PATH_SUFFIXES gtk-3.0)

find_library(GTK3_LIBRARY NAMES gtk-3)

# Gdk

find_library(GDK3_LIBRARY NAMES gdk-3)

# Gdk-Pixbuf

libfind_pkg_check_modules(PC_GDKPIXBUF gdk-pixbuf-2.0)

if(NOT PC_GDKPIXBUF_FOUND)
  set(_GTK3_found_all false)
endif()

find_path(GDKPIXBUF_INCLUDE_DIR gdk-pixbuf/gdk-pixbuf.h
  HINTS ${PC_GDKPIXBUF_INCLUDEDIR} ${PC_GDKPIXBUF_INCLUDE_DIRS}
  PATH_SUFFIXES gdk-pixbuf-2.0)

find_library(GDKPIXBUF_LIBRARY NAMES gdk_pixbuf-2.0
  HINTS ${PC_GDKPIXBUF_LIBDIR} ${PC_GDKPIXBUF_LIBRARY_DIRS})

# Glib

find_package(Glib REQUIRED)
if(NOT Glib_FOUND)
  set(_GTK3_found_all false)
endif()

# Pango

libfind_pkg_check_modules(PC_PANGO pango)

if(NOT PC_PANGO_FOUND)
  set(_GTK3_found_all false)
endif()

find_path(PANGO_INCLUDE_DIR pango/pango.h
  HINTS ${PC_PANGO_INCLUDEDIR} ${PC_PANGO_INCLUDE_DIRS}
  PATH_SUFFIXES pango-1.0)

find_library(PANGO_LIBRARY NAMES pango-1.0
  HINTS ${PC_PANGO_LIBDIR} ${PC_PANGO_LIBRARY_DIRS})

# Cairo

set(CAIRO_DEFINITIONS ${PC_CAIRO_CXXFLAGS_OTHER})

find_path(CAIRO_INCLUDE_DIR cairo.h
  HINTS ${PC_CAIRO_INCLUDEDIR} ${PC_CAIRO_INCLUDE_DIRS}
  PATH_SUFFIXES cairo)

find_library(CAIRO_LIBRARY NAMES cairo
  HINTS ${PC_CAIRO_LIBDIR} ${PC_CAIRO_LIBRARY_DIRS})

# Atk

libfind_pkg_check_modules(PC_ATK atk)

if(NOT PC_ATK_FOUND)
  set(_GTK3_found_all false)
endif()

find_path(ATK_INCLUDE_DIR atk/atk.h
  HINTS ${PC_ATK_INCLUDEDIR} ${PC_ATK_INCLUDE_DIRS}
  PATH_SUFFIXES atk-1.0)

find_library(ATK_LIBRARY NAMES atk-1.0
  HINTS ${PC_ATK_LIBDIR} ${PC_ATK_LIBRARY_DIRS})

# Finalize

if(_GTK3_found_all)
  include(FindPackageHandleStandardArgs)

  find_package_handle_standard_args(GTK3 DEFAULT_MSG GTK3_LIBRARY GTK3_INCLUDE_DIR)

  set(GTK3_LIBRARIES ${GTK3_LIBRARY} ${GDK3_LIBRARY} ${GLIB_LIBRARIES} ${PANGO_LIBRARY} ${CAIRO_LIBRARY} ${GDKPIXBUF_LIBRARY} ${ATK_LIBRARY})
  set(GTK3_INCLUDE_DIRS ${GTK3_INCLUDE_DIR} ${GLIB_INCLUDE_DIRS} ${PANGO_INCLUDE_DIR} ${CAIRO_INCLUDE_DIR} ${GDKPIXBUF_INCLUDE_DIR} ${ATK_INCLUDE_DIR})

  mark_as_advanced(GTK3_INCLUDE_DIR GTK3_LIBRARY)

  set(GTK3_FOUND true)
else()
  unset(GTK3_LIBRARY)
  unset(GTK3_INCLUDE_DIR)

  unset(GDK3_LIBRARY)
  unset(GDK3_INCLUDE_DIR)

  set(GTK3_FOUND false)

  if(GTK3_FIND_REQUIRED)
    message(FATAL_ERROR "GTK3 or one of its dependencies not found")
  endif(GTK3_FIND_REQUIRED)
endif()

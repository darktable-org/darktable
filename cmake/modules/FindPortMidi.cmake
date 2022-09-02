#.rst:
# FindPortMidi
# -------
#
# Try to find PortMidi on a Unix system.
#
# This will define the following variables:
#
# ``PortMidi_FOUND``
#     True if PortMidi is available
# ``PortMidi_LIBRARIES``
#     This should be passed to target_compile_options() if the target is not
#     used for linking
# ``PortMidi_INCLUDE_DIRS``
#     This should be passed to target_include_directories() if the target is not
#     used for linking

# SPDX-FileCopyrightText: 2022 Simon Raffeiner <info@simonraffeiner.de>
#
# SPDX-License-Identifier: MIT

find_package(PkgConfig)

set_package_properties(PortMidi PROPERTIES
                       TYPE OPTIONAL
                       URL https://github.com/PortMidi/portmidi
                       DESCRIPTION "Portable MIDI library"
                       PURPOSE "Used for hardware MIDI input devices")

# Use pkg-config to get hints about paths
libfind_pkg_check_modules(PortMidi_PKGCONF portmidi)

find_path(PortMidi_INCLUDE_DIR
    NAMES portmidi.h
    HINTS ${PortMidi_PKGCONF_INCLUDE_DIRS}
        /usr/include/
    DOC "The PortMidi include directory"
)
mark_as_advanced(PortMidi_INCLUDE_DIR)

set(PortMidi_NAMES ${PortMidi_NAMES} portmidi libportmidi)
find_library(PortMidi_LIBRARY
    NAMES ${PortMidi_NAMES}
    HINTS /usr/lib/
          /usr/lib/x86_64-linux-gnu/
    DOC "The Portmidi library"
)
mark_as_advanced(PortMidi_LIBRARY)

# handle the QUIETLY and REQUIRED arguments and set PORTMIDI_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PortMidi
    FOUND_VAR PortMidi_FOUND
    REQUIRED_VARS PortMidi_LIBRARY
                  PortMidi_INCLUDE_DIR
)

IF(PortMidi_FOUND)
  SET(PortMidi_LIBRARIES ${PortMidi_LIBRARY})
  SET(PortMidi_INCLUDE_DIRS ${PortMidi_INCLUDE_DIR})
ENDIF(PortMidi_FOUND)

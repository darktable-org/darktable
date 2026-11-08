# Optional GTK probe source-mirror overlay for the vcpkg 2025.06.13 port.
# The catalog uses the GLib 2.84.2 release tarball (including gvdb), whose
# download.gnome.org host is unavailable in the managed build environment.
# This is the same release's official GitHub mirror at the peeled 2.84.2 tag
# commit, with its exact gvdb gitlink restored below. These archive hashes are
# intentionally different from the catalog's release-tarball hash.
if(NOT VERSION STREQUAL "2.84.2")
    message(FATAL_ERROR "The GLib source-mirror overlay is pinned to 2.84.2")
endif()
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO GNOME/glib
    REF 2371bee17d85318480b3ddeeab4f5107b4889ad7
    SHA512 675b9f1d8fa225ac6062cbeb621584c7f257a5739a0a78901e17ba851dea544411a8fb5901727bcbac12a7ab539e23d2a51192f47d55bab66fe76fd95777e1ed
    PATCHES
        use-libiconv-on-windows.patch
        libintl.patch
)
vcpkg_from_github(
    OUT_SOURCE_PATH GVDB_SOURCE_PATH
    REPO GNOME/gvdb
    REF 2b42fc75f09dbe1cd1057580b5782b08f2dcb400
    SHA512 3fa37107c7f6a31d03c2b2c8d0878fb6ca35ee4ad8854136cdfdefecc9a28d307ef8f404ced369176a22b6bd7eec7ca637e29e48fad1c7bed2912dc853c1f9cf
)
file(REMOVE_RECURSE "${SOURCE_PATH}/subprojects/gvdb")
file(COPY "${GVDB_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/subprojects/gvdb")

set(LANGUAGES C CXX)
if(VCPKG_TARGET_IS_OSX OR VCPKG_TARGET_IS_IOS)
    list(APPEND LANGUAGES OBJC OBJCXX)
endif()

vcpkg_list(SET OPTIONS)
if (selinux IN_LIST FEATURES)
    if(NOT EXISTS "/usr/include/selinux")
        message(WARNING "SELinux was not found in its typical system location. Your build may fail. You can install SELinux with \"apt-get install selinux libselinux1-dev\".")
    endif()
    list(APPEND OPTIONS -Dselinux=enabled)
else()
    list(APPEND OPTIONS -Dselinux=disabled)
endif()

if (libmount IN_LIST FEATURES)
    list(APPEND OPTIONS -Dlibmount=enabled)
else()
    list(APPEND OPTIONS -Dlibmount=disabled)
endif()

vcpkg_list(SET ADDITIONAL_BINARIES)
if(VCPKG_HOST_IS_WINDOWS)
    # Presence of bash and sh enables installation of auxiliary components.
    vcpkg_list(APPEND ADDITIONAL_BINARIES "bash = ['${CMAKE_COMMAND}', '-E', 'false']")
    vcpkg_list(APPEND ADDITIONAL_BINARIES "sh = ['${CMAKE_COMMAND}', '-E', 'false']")
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    LANGUAGES ${LANGUAGES}
    ADDITIONAL_BINARIES
        ${ADDITIONAL_BINARIES}
    OPTIONS
        ${OPTIONS}
        -Ddocumentation=false
        -Dinstalled_tests=false
        -Dintrospection=disabled
        -Dlibelf=disabled
        -Dman-pages=disabled
        -Dtests=false
        -Dxattr=false
)
vcpkg_install_meson(ADD_BIN_TO_PATH)
vcpkg_copy_pdbs()

file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
set(GLIB_SCRIPTS
    gdbus-codegen
    glib-genmarshal
    glib-gettextize
    glib-mkenums
    gtester-report
)
foreach(script IN LISTS GLIB_SCRIPTS)
    file(RENAME "${CURRENT_PACKAGES_DIR}/bin/${script}" "${CURRENT_PACKAGES_DIR}/tools/${PORT}/${script}")
    file(REMOVE "${CURRENT_PACKAGES_DIR}/debug/bin/${script}")
endforeach()

set(GLIB_TOOLS
    gapplication
    gdbus
    gi-compile-repository
    gi-decompile-typelib
    gi-inspect-typelib
    gio
    gio-querymodules
    glib-compile-resources
    glib-compile-schemas
    gobject-query
    gresource
    gsettings
    gtester
)
if(VCPKG_TARGET_IS_WINDOWS)
    list(REMOVE_ITEM GLIB_TOOLS gapplication gtester)
    if(VCPKG_TARGET_ARCHITECTURE MATCHES "x64|arm64")
        list(APPEND GLIB_TOOLS gspawn-win64-helper gspawn-win64-helper-console)
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
        list(APPEND GLIB_TOOLS gspawn-win32-helper gspawn-win32-helper-console)
    endif()
elseif(VCPKG_TARGET_IS_OSX)
    list(REMOVE_ITEM GLIB_TOOLS gapplication)
endif()
vcpkg_copy_tools(TOOL_NAMES ${GLIB_TOOLS} AUTO_CLEAN)

vcpkg_fixup_pkgconfig()

if(VCPKG_TARGET_IS_WINDOWS)
    set(LIBINTL_NAME "intl.lib")
else()
    set(LIBINTL_NAME "libintl")
    if(VCPKG_LIBRARY_LINKAGE STREQUAL dynamic)
        string(APPEND LIBINTL_NAME "${VCPKG_TARGET_SHARED_LIBRARY_SUFFIX}")
    else()
        string(APPEND LIBINTL_NAME "${VCPKG_TARGET_STATIC_LIBRARY_SUFFIX}")
    endif()
endif()

set(pc_replace_intl_path gio glib gmodule-no-export gobject gthread)
foreach(pc_prefix IN LISTS pc_replace_intl_path)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/${pc_prefix}-2.0.pc" "\"" "")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/${pc_prefix}-2.0.pc" "\${prefix}/debug/lib/${LIBINTL_NAME}" "-lintl" IGNORE_UNCHANGED)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/${pc_prefix}-2.0.pc" "\${prefix}/lib/${LIBINTL_NAME}" "-lintl" IGNORE_UNCHANGED)
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/${pc_prefix}-2.0.pc" "\"" "")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/${pc_prefix}-2.0.pc" "\${prefix}/lib/${LIBINTL_NAME}" "-lintl" IGNORE_UNCHANGED)
    endif()
endforeach()

vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/gio-2.0.pc" "\${bindir}" "\${prefix}/tools/${PORT}")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/glib-2.0.pc" "\${bindir}" "\${prefix}/tools/${PORT}")
if(NOT VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/gio-2.0.pc" "\${bindir}" "\${prefix}/../tools/${PORT}")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/glib-2.0.pc" "\${bindir}" "\${prefix}/../tools/${PORT}")
endif()

# Fix python scripts
set(_file "${CURRENT_PACKAGES_DIR}/tools/${PORT}/gdbus-codegen")
file(READ "${_file}" _contents)
string(REPLACE "elif os.path.basename(filedir) == 'bin':" "elif os.path.basename(filedir) == 'tools':" _contents "${_contents}")
string(REPLACE "path = os.path.join(filedir, '..', 'share', 'glib-2.0')" "path = os.path.join(filedir, '../..', 'share', 'glib-2.0')" _contents "${_contents}")
string(REPLACE "path = os.path.join(filedir, '..')" "path = os.path.join(filedir, '../../share/glib-2.0')" _contents "${_contents}")
string(REPLACE "path = os.path.join('${CURRENT_PACKAGES_DIR}/share', 'glib-2.0')" "path = os.path.join('unuseable/share', 'glib-2.0')" _contents "${_contents}")
file(WRITE "${_file}" "${_contents}")

if(EXISTS "${CURRENT_PACKAGES_DIR}/tools/${PORT}/glib-gettextize")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/${PORT}/glib-gettextize" "${CURRENT_PACKAGES_DIR}" "`dirname $0`/../..")
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/gdb"
    "${CURRENT_PACKAGES_DIR}/debug/lib/gio"
    "${CURRENT_PACKAGES_DIR}/lib/gio"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSES/LGPL-2.1-or-later.txt")

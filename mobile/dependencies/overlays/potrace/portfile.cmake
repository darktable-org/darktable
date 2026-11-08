# SPDX-License-Identifier: GPL-3.0-or-later
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# Debian preserves the unmodified upstream release as its orig tarball. The
# archive's SHA-256 was checked against potrace_1.16-2.dsc; see README.md.
vcpkg_download_distfile(POTRACE_ARCHIVE
    URLS "https://deb.debian.org/debian/pool/main/p/potrace/potrace_1.16.orig.tar.gz"
    FILENAME "potrace-1.16.tar.gz"
    SHA512 10d85ed3fada80951223c65e923b5e6ee3e76e071575971d5b09b996b477ec5ff2403e0337c44963c6b441cc3ba6c4f7009541124a25c16b7721fdebd1f774b9
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${POTRACE_ARCHIVE}")
file(COPY
    "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_LIST_DIR}/config.h.in"
    "${CMAKE_CURRENT_LIST_DIR}/potrace.pc.in"
    "${CMAKE_CURRENT_LIST_DIR}/potraceConfig.cmake.in"
    DESTINATION "${SOURCE_PATH}"
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/potrace")
vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING" "${SOURCE_PATH}/AUTHORS")

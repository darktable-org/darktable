# SPDX-License-Identifier: MIT
# Based on the vcpkg 2025.06.13 port; retain its upstream source pin.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO lensfun/lensfun
    REF "v${VERSION}"
    SHA512 4db9a08d51ba50c7c2ff528d380bb28e34698b2bb5c40e5f3deeaa5544c888ac7e0f638bbc3f33a4f75dbb67e0425ca36ce6d8cd1d8c043a4173a2df47de08c6
    HEAD_REF master
    PATCHES fix_build.patch
)

# Keep the 0.3.4 library ABI, but package current calibrated profiles using the
# official database updater's schema-2-to-1 converter (see DATABASE.md).
file(READ "${CURRENT_PORT_DIR}/database-lock.json" LENSFUN_DATABASE_LOCK)
string(JSON LENSFUN_DATABASE_REVISION GET "${LENSFUN_DATABASE_LOCK}" revision)
string(JSON LENSFUN_DATABASE_SHA512 GET "${LENSFUN_DATABASE_LOCK}" archive_sha512)
string(JSON LENSFUN_DATABASE_LXML_VERSION GET "${LENSFUN_DATABASE_LOCK}" lxml_version)
vcpkg_from_github(
    OUT_SOURCE_PATH DATABASE_SOURCE_PATH
    REPO lensfun/lensfun
    REF "${LENSFUN_DATABASE_REVISION}"
    SHA512 "${LENSFUN_DATABASE_SHA512}"
)
vcpkg_find_acquire_program(PYTHON3)
x_vcpkg_get_python_packages(OUT_PYTHON_VAR LENSFUN_DATABASE_PYTHON
    PYTHON_EXECUTABLE "${PYTHON3}"
    PYTHON_VERSION "3"
    PACKAGES "lxml==${LENSFUN_DATABASE_LXML_VERSION}"
)
vcpkg_execute_required_process(
    COMMAND "${LENSFUN_DATABASE_PYTHON}" "${CURRENT_PORT_DIR}/convert_database.py"
        --source "${DATABASE_SOURCE_PATH}" --output "${SOURCE_PATH}/data/db"
        --lock "${CURRENT_PORT_DIR}/database-lock.json" --replace
    WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
    LOGNAME convert-database-${TARGET_TRIPLET}
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" LENSFUN_STATIC_LIB)
string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" LENSFUN_STATIC_CRT)
set(LENSFUN_EXTRA_OPTS "")
if("python" IN_LIST FEATURES)
    find_file(INITIAL_PYTHON3
        NAMES "python3${VCPKG_HOST_EXECUTABLE_SUFFIX}" "python${VCPKG_HOST_EXECUTABLE_SUFFIX}"
        PATHS "${CURRENT_HOST_INSTALLED_DIR}/tools/python3"
        NO_DEFAULT_PATH
        REQUIRED
    )
    x_vcpkg_get_python_packages(OUT_PYTHON_VAR PYTHON3
        PYTHON_EXECUTABLE "${INITIAL_PYTHON3}"
        PYTHON_VERSION "3"
        PACKAGES setuptools
    )
else()
    set(PYTHON3 "false")
endif()

if(VCPKG_TARGET_IS_WINDOWS)
    list(APPEND LENSFUN_EXTRA_OPTS -DPLATFORM_WINDOWS=ON)
endif()

if(NOT VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    # Upstream implements the complete correction algorithms in scalar C++.
    # Its optional x86 intrinsics and all CPU feature calls are macro-guarded.
    list(APPEND LENSFUN_EXTRA_OPTS -DBUILD_FOR_SSE=OFF -DBUILD_FOR_SSE2=OFF)
endif()
if(VCPKG_TARGET_IS_ANDROID)
    list(APPEND LENSFUN_EXTRA_OPTS -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${LENSFUN_EXTRA_OPTS}
        -DBUILD_STATIC=${LENSFUN_STATIC_LIB}
        -DBUILD_WITH_MSVC_STATIC_RUNTIME=${LENSFUN_STATIC_CRT}
        -DBUILD_TESTS=OFF
        -DBUILD_DOC=OFF
        -DBUILD_LENSTOOL=OFF
        -DINSTALL_HELPER_SCRIPTS=OFF
        "-DPYTHON=${PYTHON3}"
)
vcpkg_cmake_install()
file(INSTALL "${SOURCE_PATH}/data/db/database-provenance.json"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/lensfun/version_1")
file(INSTALL "${DATABASE_SOURCE_PATH}/data/COPYING.CC_BY-SA_3.0"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/lensfun/version_1")
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(READ "${SOURCE_PATH}/README.md" license_comment)
string(REGEX REPLACE "^.*\n(LICENSE\n)---+\n(.*)" "\\1\\2" license_comment "${license_comment}")
string(REGEX REPLACE "[^\n]+\n---+.*\$" "" license_comment "${license_comment}")
vcpkg_install_copyright(
    COMMENT "${license_comment}"
    FILE_LIST "${SOURCE_PATH}/docs/gpl-3.0.txt" "${SOURCE_PATH}/docs/lgpl-3.0.txt"
              "${SOURCE_PATH}/docs/cc-by-sa-3.0.txt" "${DATABASE_SOURCE_PATH}/data/COPYING.CC_BY-SA_3.0"
)

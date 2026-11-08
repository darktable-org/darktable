# Optional GTK probe source mirror for the vcpkg 2025.06.13 Pixman port.
# Meson's official WrapDB mirrors the upstream release tarball for the same
# version. It differs from the catalog's GitLab tag archive and has its own hash.
if(NOT VERSION STREQUAL "0.44.2")
    message(FATAL_ERROR "The Pixman source-mirror overlay is pinned to 0.44.2")
endif()
vcpkg_download_distfile(PIXMAN_ARCHIVE
    URLS
        "https://www.cairographics.org/releases/pixman-0.44.2.tar.gz"
        "https://github.com/mesonbuild/wrapdb/releases/download/pixman_0.44.2-1/pixman-0.44.2.tar.gz"
    FILENAME "pixman-0.44.2.tar.gz"
    SHA512 5d599568025403c0f5a8499057bbe22ff6bf769459d37c77e2978efd91fd46f9d64cf51d150371a55c6b2f323cc598fdf59649fe050a4a6cf5196a547af3faf1
)
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${PIXMAN_ARCHIVE}"
    PATCHES
        no-host-cpu-checks.patch
        missing_intrin_include.patch
)

set(x86_architectures x86 x64)
if(VCPKG_TARGET_ARCHITECTURE IN_LIST x86_architectures AND NOT VCPKG_TARGET_IS_UWP)
    list(APPEND OPTIONS
        -Dmmx=enabled
        -Dsse2=enabled
        -Dssse3=enabled
    )
else()
    list(APPEND OPTIONS
        -Dmmx=disabled
        -Dsse2=disabled
        -Dssse3=disabled
    )
    if(VCPKG_TARGET_IS_ANDROID)
        vcpkg_cmake_get_vars(cmake_vars_file)
        include("${cmake_vars_file}")
        find_path(cpu_features_dir
            NAMES cpu-features.c
            PATHS "${VCPKG_DETECTED_CMAKE_ANDROID_NDK}"
            PATH_SUFFIXES
                "sources/android/cpufeatures" # NDK r27c
            NO_DEFAULT_PATH
        )
        if(VCPKG_DETECTED_CMAKE_ANDROID_ARM_NEON AND cpu_features_dir)
            list(APPEND OPTIONS
                "-Dcpu-features-path=${cpu_features_dir}"
            )
        endif()
    endif()
    if(VCPKG_TARGET_IS_WINDOWS)
        # -Darm-simd=enabled does not work with arm64-windows
        list(APPEND OPTIONS
            -Da64-neon=disabled
            -Darm-simd=disabled
            -Dneon=disabled
        )
    endif()
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${OPTIONS}
        -Ddemos=disabled
        -Dgtk=disabled
        -Dlibpng=enabled
        -Dtests=disabled
)
vcpkg_install_meson()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

set(licenses "${SOURCE_PATH}/COPYING")
if(VCPKG_DETECTED_CMAKE_ANDROID_ARM_NEON AND cpu_features_dir)
    file(READ "${cpu_features_dir}/cpu-features.c" cpu_features_c)
    string(REGEX REPLACE "[*]/.*" "*/\n" cpu_features_license "${cpu_features_c}")
    file(WRITE "${CURRENT_PACKAGES_DIR}/${TARGET_TRIPLET}-rel/cpu-features (BSD-2-Clause)" "${cpu_features_license}")
    list(APPEND licenses "${CURRENT_PACKAGES_DIR}/${TARGET_TRIPLET}-rel/cpu-features (BSD-2-Clause)")
endif()
vcpkg_install_copyright(FILE_LIST ${licenses})

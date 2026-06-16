cmake_minimum_required(VERSION 3.21)
project(DarktableMobile VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Quick QuickControls2 Network Xml Multimedia)
# CorePrivate for qcoreapplication_platform.h (QAndroidApplication / JNI context)
# ImageFormats for AVIF/WebP/TIFF plugin support
find_package(Qt6 QUIET COMPONENTS CorePrivate ImageFormats)
qt_standard_project_setup(REQUIRES 6.5)

# ── ZXingCpp (QR code decoding) ────────────────────────────────────────────────
# Prefer a system-installed build; fall back to FetchContent for Android where
# the system package is not available at cross-compile time.
find_package(ZXing QUIET)
if(NOT ZXing_FOUND)
    include(FetchContent)
    FetchContent_Declare(zxing-cpp
        GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
        GIT_TAG        v2.3.0
        GIT_SHALLOW    TRUE
    )
    set(ZXING_BUILD_READERS         ON  CACHE BOOL "" FORCE)
    set(ZXING_BUILD_WRITERS         OFF CACHE BOOL "" FORCE)
    set(ZXING_BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
    set(ZXING_BUILD_BLACKBOX_TESTS  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zxing-cpp)
    # Older zxing-cpp sources put headers directly in core/src (no ZXing/ prefix).
    # Expose that path so __has_include(<ZXing/ReadBarcode.h>) resolves correctly
    # and qrscanner.cpp falls back to the bare include variant.
    set(_ZXING_EXTRA_INCLUDE "${zxing-cpp_SOURCE_DIR}/core/src")
endif()

# ── bundled daemon binary ──────────────────────────────────────────────────────
# Place the arm64-v8a cross-compiled dt-p2p-daemon at:
#   src/mobile/assets/dt-p2p-daemon
# before configuring.  build-android.sh does this automatically.
set(DAEMON_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/dt-p2p-daemon")

# ── sources ────────────────────────────────────────────────────────────────────
set(CPP_SOURCES
    src/main.cpp
    src/avifimageprovider.cpp  src/avifimageprovider.h
    src/daemonmanager.cpp      src/daemonmanager.h
    src/p2pclient.cpp          src/p2pclient.h
    src/imagemodel.cpp         src/imagemodel.h
    src/xmpio.cpp              src/xmpio.h
    src/pairingmanager.cpp     src/pairingmanager.h
    src/qrscanner.cpp          src/qrscanner.h
)

qt_add_executable(darktable-mobile ${CPP_SOURCES})

qt_add_qml_module(darktable-mobile
    URI DarktableMobile
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/ThumbnailGrid.qml
        qml/DarkroomView.qml
        qml/SettingsView.qml
        qml/PairingView.qml
)

# Bundle the daemon binary as a Qt resource for non-Android builds (desktop preview).
# On Android the daemon is shipped as libdt-p2p-daemon.so in android/libs/arm64-v8a/
# so the OS extracts it to the executable native-library directory at install time.
if(NOT ANDROID)
    if(EXISTS "${DAEMON_ASSET}")
        qt_add_resources(darktable-mobile "assets"
            FILES "${DAEMON_ASSET}"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}"
        )
    else()
        message(WARNING "dt-p2p-daemon not found at ${DAEMON_ASSET}. "
                        "Run build-android.sh or place the binary manually.")
    endif()
endif()

# Resolve the ZXingCpp target name (differs between system and FetchContent).
if(TARGET ZXing::ZXing)
    set(_ZXING_TARGET ZXing::ZXing)
elseif(TARGET ZXing)
    set(_ZXING_TARGET ZXing)
else()
    message(FATAL_ERROR "ZXingCpp target not found after find_package / FetchContent")
endif()

target_link_libraries(darktable-mobile PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Xml
    Qt6::Multimedia
    ${_ZXING_TARGET}
)

if(ANDROID AND TARGET Qt6::CorePrivate)
    target_link_libraries(darktable-mobile PRIVATE Qt6::CorePrivate)
endif()

if(TARGET Qt6::ImageFormats)
    target_link_libraries(darktable-mobile PRIVATE Qt6::ImageFormats)
endif()

if(DEFINED _ZXING_EXTRA_INCLUDE)
    target_include_directories(darktable-mobile PRIVATE "${_ZXING_EXTRA_INCLUDE}")
endif()

# ── Android packaging ──────────────────────────────────────────────────────────
if(ANDROID)
    set_target_properties(darktable-mobile PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_MIN_SDK_VERSION      26
        QT_ANDROID_TARGET_SDK_VERSION   34
        QT_ANDROID_PACKAGE_NAME         "net.darktable.mobile"
        QT_ANDROID_APP_NAME             "Darktable Mobile"
        QT_ANDROID_VERSION_CODE         1
        QT_ANDROID_VERSION_NAME         "0.1.0"
    )
endif()

install(TARGETS darktable-mobile
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_policy(SET QTP0002 NEW)

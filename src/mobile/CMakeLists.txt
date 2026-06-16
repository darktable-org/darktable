cmake_minimum_required(VERSION 3.21)
project(DarktableMobile VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Quick QuickControls2 Network Xml)
qt_standard_project_setup(REQUIRES 6.5)

# ── bundled daemon binary ──────────────────────────────────────────────────────
# Place the arm64-v8a cross-compiled dt-p2p-daemon at:
#   src/mobile/assets/dt-p2p-daemon
# before configuring.  build-android.sh does this automatically.
set(DAEMON_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/dt-p2p-daemon")

# ── sources ────────────────────────────────────────────────────────────────────
set(CPP_SOURCES
    src/main.cpp
    src/daemonmanager.cpp  src/daemonmanager.h
    src/p2pclient.cpp      src/p2pclient.h
    src/imagemodel.cpp     src/imagemodel.h
    src/xmpio.cpp          src/xmpio.h
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
)

# Bundle the daemon binary as a Qt resource so we can extract it at runtime.
if(EXISTS "${DAEMON_ASSET}")
    qt_add_resources(darktable-mobile "assets"
        FILES "${DAEMON_ASSET}"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
else()
    message(WARNING "dt-p2p-daemon not found at ${DAEMON_ASSET}. "
                    "Run build-android.sh or place the binary manually.")
endif()

target_link_libraries(darktable-mobile PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Xml
)

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

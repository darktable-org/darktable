cmake_minimum_required(VERSION 3.18)

add_library(darktable_ai STATIC
  backend.h
  backend_common.c
  backend_onnx.c
  segmentation.h
  segmentation.c
)

# Find ONNX Runtime (auto-downloads if not present)
find_package(ONNXRuntime REQUIRED)

# Find Dependencies (inherited from main build if available, but explicit here for clarity)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
# GTK3/RSVG needed for headers only (common/darktable.h -> utility.h -> gtk/gtk.h, librsvg/rsvg.h)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(RSVG2 REQUIRED librsvg-2.0)

# Include current directory for header
target_include_directories(darktable_ai PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../>
  $<INSTALL_INTERFACE:include>
  ${GLIB_INCLUDE_DIRS}
  ${JSON_GLIB_INCLUDE_DIRS}
  ${GTK3_INCLUDE_DIRS}
  ${RSVG2_INCLUDE_DIRS}
)

target_link_directories(darktable_ai PRIVATE
  ${GLIB_LIBRARY_DIRS}
  ${JSON_GLIB_LIBRARY_DIRS}
)

if(ONNXRuntime_SYSTEM_PACKAGE AND UNIX AND NOT APPLE)
  # Ubuntu/Debian's libonnxruntime links against the system libonnx, so both
  # register the same ONNX schemas at library load time, producing harmless but
  # noisy "already registered" warnings on stderr.  Suppress them by lazy-loading
  # ORT at runtime via g_module_open, with stderr temporarily redirected, instead
  # of having libonnxruntime.so as a direct ELF dependency of libdarktable.so.
  # ONNXRuntime_LIBRARIES is set by FindONNXRuntime.cmake to the .so path.
  target_compile_definitions(darktable_ai PRIVATE
    ORT_LAZY_LOAD=1
    ORT_LIBRARY_PATH="${ONNXRuntime_LIBRARIES}")
  target_include_directories(darktable_ai PRIVATE ${ONNXRuntime_INCLUDE_DIRS})
else()
  target_link_libraries(darktable_ai PUBLIC onnxruntime::onnxruntime)
endif()

target_link_libraries(darktable_ai PUBLIC
  ${GLIB_LIBRARIES}
  ${JSON_GLIB_LIBRARIES}
)

# Install the bundled ONNX Runtime shared library alongside darktable_ai
# (skip when using system package — it's already in the system library path)
if(ONNXRuntime_SYSTEM_PACKAGE)
  # Nothing to install
elseif(APPLE)
  file(GLOB _ORT_DYLIB "${ONNXRuntime_LIB_DIR}/libonnxruntime.*.dylib")
  if(_ORT_DYLIB)
    install(FILES ${_ORT_DYLIB}
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable
    )
  endif()
elseif(WIN32)
  # Install all ORT DLLs (onnxruntime.dll, onnxruntime_providers_shared.dll, etc.)
  # DirectML.dll is a system component on Windows 10 1903+ and does not need bundling.
  file(GLOB _ORT_DLL "${ONNXRuntime_LIB_DIR}/onnxruntime*.dll")
  if(_ORT_DLL)
    install(FILES ${_ORT_DLL}
      DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
  endif()
elseif(UNIX)
  file(GLOB _ORT_SO "${ONNXRuntime_LIB_DIR}/libonnxruntime.so*")
  if(_ORT_SO)
    install(FILES ${_ORT_SO}
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable
    )
  endif()
endif()

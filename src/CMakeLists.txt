include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CheckCSourceCompiles)

#
# Add files for libdarktable
#
FILE(GLOB SOURCE_FILES
  "bauhaus/bauhaus.c"
  "common/bilateral.c"
  "common/bilateralcl.c"
  "common/cache.c"
  "common/calculator.c"
  "common/collection.c"
  "common/color_picker.c"
  "common/colorlabels.c"
  "common/colorspaces.c"
  "common/curve_tools.c"
  "common/cpuid.c"
  "common/darktable.c"
  "common/database.c"
  "common/dbus.c"
  "common/dtpthread.c"
  "common/exif.cc"
  "common/film.c"
  "common/file_location.c"
  "common/fswatch.c"
  "common/gaussian.c"
  "common/grouping.c"
  "common/history.c"
  "common/gpx.c"
  "common/image.c"
  "common/image_cache.c"
  "common/image_compression.c"
  "common/imageio.c"
  "common/imageio_jpeg.c"
  "common/imageio_png.c"
  "common/imageio_module.c"
  "common/imageio_pfm.c"
  "common/imageio_rgbe.c"
  "common/imageio_tiff.c"
  "common/imageio_gm.c"
  "common/imageio_rawspeed.cc"
  "common/import_session.c"
  "common/interpolation.c"
  "common/metadata.c"
  "common/mipmap_cache.c"
  "common/noiseprofiles.c"
  "common/pdf.c"
  "common/styles.c"
  "common/selection.c"
  "common/system_signal_handling.c"
  "common/tags.c"
  "common/utility.c"
  "common/variables.c"
  "common/pwstorage/backend_kwallet.c"
  "common/pwstorage/pwstorage.c"
  "common/opencl.c"
  "common/dynload.c"
  "common/dlopencl.c"
  "common/ratings.c"
  "common/resource_limits.c"
  "common/histogram.c"
  "control/control.c"
  "control/crawler.c"
  "control/jobs.c"
  "control/jobs/control_jobs.c"
  "control/jobs/develop_jobs.c"
  "control/jobs/film_jobs.c"
  "control/jobs/image_jobs.c"
  "control/progress.c"
  "control/signal.c"
  "develop/develop.c"
  "develop/imageop.c"
  "develop/imageop_math.c"
  "develop/lightroom.c"
  "develop/pixelpipe.c"
  "develop/blend.c"
  "develop/blend_gui.c"
  "develop/tiling.c"
  "develop/masks/masks.c"
  "develop/format.c"
  "dtgtk/button.c"
  "dtgtk/drawingarea.c"
  "dtgtk/expander.c"
  "dtgtk/gradientslider.c"
  "dtgtk/icon.c"
  "dtgtk/paint.c"
  "dtgtk/resetlabel.c"
  "dtgtk/sidepanel.c"
  "dtgtk/togglebutton.c"
  "gui/accelerators.c"
  "gui/gtkentry.c"
  "gui/guides.c"
  "gui/gtk.c"
  "gui/hist_dialog.c"
  "gui/preferences.c"
  "gui/presets.c"
  "gui/styles_dialog.c"
  "libs/lib.c"
  "views/view.c"
  "views/undo.c"
  )
FILE(GLOB HEADER_FILES "*.h" "common/*.h" "external/CL/*.h" "control/*.h" "iop/*.h" "libs/*.h" "views/*.h")

if(APPLE)
  list(APPEND SOURCE_FILES "osx/osx.mm")
  list(APPEND HEADER_FILES "osx/osx.h")
  set_source_files_properties(osx/osx.mm PROPERTIES LANGUAGE CXX)
endif(APPLE)

set(SOURCES ${SOURCE_FILES} ${HEADER_FILES})


include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external)

# initial compiler flags
add_definitions("-DHAVE_CONFIG_H")

CHECK_C_COMPILER_FLAG("-std=c99" COMPILER_SUPPORTS_C99)
if(NOT COMPILER_SUPPORTS_C99)
        message(FATAL_ERROR "The compiler ${CMAKE_C_COMPILER} has no C99 support. Please use a different C compiler.")
endif()

CHECK_C_COMPILER_FLAG("-std=c11" COMPILER_SUPPORTS_C11)
if(NOT COMPILER_SUPPORTS_C11)
  message(FATAL_ERROR "The compiler ${CMAKE_C_COMPILER} has no C11 support. Please use a different C compiler.")
endif()

CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
if(NOT COMPILER_SUPPORTS_CXX11)
        message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

if(APPLE AND (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
        list(REMOVE_ITEM CMAKE_CXX_IMPLICIT_LINK_LIBRARIES stdc++)
        list(APPEND CMAKE_CXX_IMPLICIT_LINK_LIBRARIES c++)
        list(REMOVE_DUPLICATES CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
endif()

# Check for openmp
find_package(OpenMP)
if(OpenMP_C_FLAGS AND USE_OPENMP)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
  if(WIN32)
    list(APPEND LIBS gomp)
  endif(WIN32)
endif(OpenMP_C_FLAGS AND USE_OPENMP)

if(USE_DARKTABLE_PROFILING)
	add_definitions(-DUSE_DARKTABLE_PROFILING)
	set(SOURCES ${SOURCES} "common/profiling.c")
endif()

#
# Find all other required libraries for building
#
# GTK3 does pull glib, but this allows us to check for the version
find_package(Glib 2.40 REQUIRED)
add_definitions("-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_40")
add_definitions("-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_MIN_REQUIRED")
include_directories(SYSTEM ${Glib_INCLUDE_DIRS})
list(APPEND LIBS ${Glib_LIBRARIES})

# GTK3 pulls in ATK, GDK, GDK-PIXBUF, CAIRO, GLIB, PANGO
find_package(GTK3 3.14 REQUIRED)
add_definitions("-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_14")
#add_definitions("-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_MIN_REQUIRED")
include_directories(SYSTEM ${GTK3_INCLUDE_DIRS})
list(APPEND LIBS ${GTK3_LIBRARIES})

# Check for libxml2 / broken cmake module can't be included in the foreach() below
find_package(LibXml2 2.6 REQUIRED)
include_directories(SYSTEM ${LIBXML2_INCLUDE_DIR})
list(APPEND LIBS ${LIBXML2_LIBRARIES})
add_definitions(${LIBXML2_DEFINITIONS})

# used to launch a tiny http server, used by facebook exporter
find_package(LibSoup2)
if(LIBSOUP2_FOUND)
  include_directories(SYSTEM ${LIBSOUP2_INCLUDE_DIRS})
  list(APPEND LIBS ${LIBSOUP2_LIBRARIES})
  add_definitions(${LIBSOUP2_DEFINITIONS})
  add_definitions("-DHAVE_HTTP_SERVER")
  list(APPEND SOURCES "common/http_server.c")
endif(LIBSOUP2_FOUND)

if(USE_CAMERA_SUPPORT)
  find_package(Gphoto2)
  include_directories(SYSTEM ${Gphoto2_INCLUDE_DIRS})
  list(APPEND LIBS ${Gphoto2_LIBRARIES})
  add_definitions(${Gphoto2_DEFINITIONS})
endif(USE_CAMERA_SUPPORT)

# # INotify isn't used anymore as it seems
# find_package(INotify)
# if(INOTIFY_FOUND)
#   include_directories(SYSTEM ${INOTIFY_INCLUDE_DIRS})
#   list(APPEND LIBS ${INOTIFY_LIBRARIES})
#   add_definitions(${INOTIFY_DEFINITIONS})
# endif(INOTIFY_FOUND)

if(USE_OPENEXR)
  find_package(OpenEXR)
  if(OPENEXR_FOUND)
    include_directories(SYSTEM ${OpenEXR_INCLUDE_DIRS})
    list(APPEND LIBS ${OpenEXR_LIBRARIES})
    add_definitions(${OpenEXR_DEFINITIONS})
    list(APPEND SOURCES "common/imageio_exr.cc")
  endif(OPENEXR_FOUND)
endif(USE_OPENEXR)

if(USE_WEBP)
   find_package(WebP 0.3.0)
  if(WEBP_FOUND)
    include_directories(SYSTEM ${WebP_INCLUDE_DIRS})
    list(APPEND LIBS ${WebP_LIBRARIES})
    add_definitions(${WebP_DEFINITIONS})
  endif(WEBP_FOUND)
  else(USE_WEBP)
endif(USE_WEBP)

foreach(lib ${OUR_LIBS} LensFun GIO GThread GModule PangoCairo PThread Rsvg2 LibXml2 Sqlite3 CURL PNG JPEG TIFF LCMS2 JsonGlib)
  find_package(${lib} REQUIRED)
  include_directories(SYSTEM ${${lib}_INCLUDE_DIRS})
  list(APPEND LIBS ${${lib}_LIBRARIES})
  add_definitions(${${lib}_DEFINITIONS})
endforeach(lib)

# Require exiv2 >= 0.24 to make sure everything we need is available
find_package(Exiv2 0.24 REQUIRED)
include_directories(SYSTEM ${Exiv2_INCLUDE_DIRS})
list(APPEND LIBS ${Exiv2_LIBRARIES})
add_definitions(${Exiv2_DEFINITIONS})

if(USE_FLICKR)
  find_package(FLICKCURL)
  if(FLICKCURL_FOUND)
    include_directories(SYSTEM ${FLICKCURL_INCLUDE_DIRS})
    list(APPEND LIBS ${FLICKCURL_LIBRARIES})
    add_definitions(${FLICKCURL_DEFINITIONS})
  endif(FLICKCURL_FOUND)
endif(USE_FLICKR)

if(USE_OPENJPEG)
  find_package(OpenJPEG)
  if(OpenJPEG_FOUND)
    add_definitions("-DHAVE_OPENJPEG")
    list(APPEND SOURCES "common/imageio_j2k.c")

    include_directories(SYSTEM ${OpenJPEG_INCLUDE_DIRS})
    list(APPEND LIBS ${OpenJPEG_LIBRARIES})
  endif(OpenJPEG_FOUND)
endif(USE_OPENJPEG)

#
# Detect compile of optional pwstorage backends
#
if(USE_LIBSECRET AND JsonGlib_FOUND)
  find_package(Libsecret)
  if(LIBSECRET_FOUND)
    if(LIBSECRET_VERSION VERSION_LESS "0.16")
      message(STATUS "Found libsecret version ${LIBSECRET_VERSION}. You need at least libsecret 0.16 or newer.")
      set(LIBSECRET_FOUND FALSE)
    else()
      if(LIBSECRET_VERSION VERSION_LESS "0.18")
        message(STATUS "Found libsecret version ${LIBSECRET_VERSION}. Enable unstable API. Please upgrade to version 0.18 or newer.")
        add_definitions("-DSECRET_API_SUBJECT_TO_CHANGE")
      endif()
      set(SOURCES ${SOURCES} "common/pwstorage/backend_libsecret.c")
      include_directories(SYSTEM ${LIBSECRET_INCLUDE_DIRS})
      list(APPEND LIBS ${LIBSECRET_LIBRARIES})
      add_definitions("-DHAVE_LIBSECRET")
    endif()
  endif(LIBSECRET_FOUND)
endif(USE_LIBSECRET AND JsonGlib_FOUND)

if(USE_KWALLET)
  add_definitions("-DHAVE_KWALLET")
endif(USE_KWALLET)

if(USE_MAC_INTEGRATION)
  find_package(MacIntegration)
  if(MACINTEGRATION_FOUND)
    include_directories(SYSTEM ${MacIntegration_INCLUDE_DIRS})
    list(APPEND LIBS ${MacIntegration_LIBRARIES})
    add_definitions("-DMAC_INTEGRATION")
  endif(MACINTEGRATION_FOUND)
endif(USE_MAC_INTEGRATION)

if(USE_UNITY)
  find_package(Unity)
  if(UNITY_FOUND)
    add_definitions(-DHAVE_UNITY)
    include_directories(SYSTEM ${UNITY_INCLUDE_DIRS})
    list(APPEND LIBS ${UNITY_LIBRARIES})
  endif(UNITY_FOUND)
endif(USE_UNITY)

if(USE_GRAPHICSMAGICK)
  find_package(GraphicsMagick)
  if(GraphicsMagick_FOUND)
    add_definitions(-DHAVE_GRAPHICSMAGICK)
    include_directories(SYSTEM ${GraphicsMagick_INCLUDE_DIRS})
    list(APPEND LIBS ${GraphicsMagick_LIBRARIES})
  endif(GraphicsMagick_FOUND)
endif(USE_GRAPHICSMAGICK)

#
# Add HAVE_xxx defines used by darktable
#
# if(INOTIFY_FOUND)
#   add_definitions("-DHAVE_INOTIFY")
# endif(INOTIFY_FOUND)

if(LENSFUN_FOUND)
  add_definitions("-DHAVE_LENSFUN")
endif(LENSFUN_FOUND)

if(OPENEXR_FOUND)
  add_definitions("-DHAVE_OPENEXR")
endif(OPENEXR_FOUND)

if(USE_OPENCL)
  add_definitions("-DHAVE_OPENCL")
endif(USE_OPENCL)


if(GPHOTO2_FOUND)
  set(SOURCES ${SOURCES} "common/camera_control.c" "control/jobs/camera_jobs.c" "gui/camera_import_dialog.c" )
  add_definitions("-DHAVE_GPHOTO2")

  # Gphoto 2.5 changed the prototype of a set of callback functions.
  # The interface change makes it necessary to detect the version here,
  # as it's not possible to do in code. Argh.
  # Diff: http://sourceforge.net/p/gphoto/code/12735/
  #
  # This workaround may be removed when darktable no longer needs
  # to support gphoto2 < 2.5.0.
  #
  if(${GPHOTO2_VERSION_STRING} VERSION_LESS "2.5")
  else()
    add_definitions("-DHAVE_GPHOTO_25_OR_NEWER")
  endif()
endif(GPHOTO2_FOUND)

# Check for __builtin_cpu_supports here due to a bug in clang/llvm. LLVM uses
# libgcc to provide the symbols, so if clang wasn't compiled with libgcc, the
# compiler will say it's available; however the linker cannot find the symbols.
# So do a quick test compile to see if it's actually available.
# See https://llvm.org/bugs/show_bug.cgi?id=25510
check_c_source_compiles("#include <stdio.h>
int main() {
  __builtin_cpu_init();
  printf(\"%d\", __builtin_cpu_supports(\"sse3\"));
}" HAVE_BUILTIN_CPU_SUPPORTS)
if(HAVE_BUILTIN_CPU_SUPPORTS)
  add_definitions("-DHAVE_BUILTIN_CPU_SUPPORTS")
endif(HAVE_BUILTIN_CPU_SUPPORTS)
MESSAGE(STATUS "Does the compiler support __builtin_cpu_supports(): ${HAVE_BUILTIN_CPU_SUPPORTS}")

check_c_source_compiles("
static __thread int tls;
int main(void)
{
  return 0;
}" HAVE_TLS)
if(NOT HAVE_TLS)
  MESSAGE(FATAL_ERROR "The compiler does not support Thread-local storage.")
endif()

if(NOT WIN32)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif(NOT WIN32)

if(WIN32)
  # get rid of the console window. do we need both?
#   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mwindows -Wl,-subsystem,windows")
#   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mwindows -Wl,-subsystem,windows")

  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--enable-runtime-pseudo-reloc")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--enable-runtime-pseudo-reloc")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--enable-runtime-pseudo-reloc")

  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mms-bitfields")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mms-bitfields")

  SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "-Wl,--no-undefined -static-libgcc -Wl,-O1 -Wl,--as-needed -Wl,--sort-common -s")
  SET(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "-Wl,--no-undefined -static-libgcc -Wl,-O1 -Wl,--as-needed -Wl,--sort-common -s")

  list(APPEND LIBS ws2_32)
  list(APPEND LIBS msvcrt) # Needed for _aligned_alloc()
  list(APPEND SOURCES "win/getrusage.c")
endif(WIN32)

if(NOT CUSTOM_CFLAGS)
  if(BUILD_SSE2_CODEPATHS)
    #we MUST always specify our requred instruction set, native might not detect it
    set(DT_REQ_INSTRUCTIONS "-msse2")
  endif()

  include(march-mtune)

  #if(NOT BUILD_SSE2_CODEPATHS)
  #  set(MARCH "${MARCH} -mno-sse2 -D__DISABLE_SSE2__ -U__SSE2__ -D__DISABLE_SSE__ -U__SSE__")
  #endif()

  #-g MUST be set for ALL builds, or there will be no support for them when bugs happen
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${MARCH} ${DT_REQ_INSTRUCTIONS} -g")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O2")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -ffast-math -fno-finite-math-only")
  if(CMAKE_COMPILER_IS_GNUCC)
    if(BUILD_SSE2_CODEPATHS)
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mfpmath=sse")
    endif()

    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g3 -ggdb3")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -fexpensive-optimizations")
  else()
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0")
  endif()

  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MARCH} ${DT_REQ_INSTRUCTIONS} -g")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O2")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -ffast-math -fno-finite-math-only")
  if(CMAKE_COMPILER_IS_GNUCXX)
    if(BUILD_SSE2_CODEPATHS)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpmath=sse")
    endif()

    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g3 -ggdb3")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fexpensive-optimizations")
  else()
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0")
  endif()
endif(NOT CUSTOM_CFLAGS)

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_DEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")

#
# Generate config.h
#
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/config.cmake.h" "${CMAKE_CURRENT_BINARY_DIR}/config.h" @ONLY)

include_directories("${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")

#
# Build external deps
#
if(USE_LUA)
  find_package(Lua52 5.2)
  if(LUA52_FOUND)
    message(STATUS "Lua support: Enabled")
    include_directories(SYSTEM ${LUA52_INCLUDE_DIRS})
    list(APPEND LIBS ${LUA52_LDFLAGS})
  else()
    if(DONT_USE_INTERNAL_LUA)
      message(STATUS "Lua support: System library for lua 5.2 not found (to use darktable's version use -DDONT_USE_INTERNAL_LUA=Off)")
      set(USE_LUA OFF)
    else(DONT_USE_INTERNAL_LUA)
      message(STATUS "Lua support: System library not found (using darktable's version)")
      include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/lua/src)
      add_subdirectory(external/lua)
      list(APPEND LIBS lua)
    endif(DONT_USE_INTERNAL_LUA)
  endif(LUA52_FOUND)

  if(USE_LUA)
    add_definitions("-DUSE_LUA")
    FILE(GLOB SOURCE_FILES_LUA
      "lua/cairo.c"
      "lua/call.c"
      "lua/configuration.c"
      "lua/database.c"
      "lua/events.c"
      "lua/film.c"
      "lua/format.c"
      "lua/gettext.c"
      "lua/glist.c"
      "lua/gui.c"
      "lua/guides.c"
      "lua/image.c"
      "lua/init.c"
      "lua/lib.c"
      "lua/lua.c"
      "lua/lualib.c"
      "lua/luastorage.c"
      "lua/modules.c"
      "lua/preferences.c"
      "lua/print.c"
      "lua/storage.c"
      "lua/styles.c"
      "lua/tags.c"
      "lua/types.c"
      "lua/view.c"
      "lua/widget/widget.c"
      "lua/widget/box.c"
      "lua/widget/button.c"
      "lua/widget/check_button.c"
      "lua/widget/combobox.c"
      "lua/widget/container.c"
      "lua/widget/entry.c"
      "lua/widget/file_chooser.c"
      "lua/widget/label.c"
      "lua/widget/separator.c"
      "lua/widget/slider.c"
      "lua/widget/stack.c"
      "lua/widget/text_view.c"
      )
    set(SOURCES ${SOURCES} ${SOURCE_FILES_LUA})
  endif(USE_LUA)
else(USE_LUA)
  message(STATUS "Lua support: Disabled")
endif(USE_LUA)

if(USE_LUA)
  if(NOT LUA52_FOUND)
    list(APPEND LIB_DEPS lua) # 2nd half of the uglyness :/
  endif(NOT LUA52_FOUND)
  # liblautoc for lua automated interface generation
  add_library(lautoc_static STATIC IMPORTED)
  set_target_properties(lautoc_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/LuaAutoC/liblautoc.a POSITION_INDEPENDENT_CODE True)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/LuaAutoC/)
  add_subdirectory(external/LuaAutoC)
  list(APPEND LIB_DEPS lautoc)
  list(APPEND STATIC_LIBS lautoc_static)
endif(USE_LUA)

find_package(Pugixml 1.2 REQUIRED)
include_directories(SYSTEM ${Pugixml_INCLUDE_DIRS})
list(APPEND LIBS ${Pugixml_LIBRARIES})

if(NOT SOURCE_PACKAGE AND NOT APPLE)
  # apple build fails on deprecated warnings..
  add_definitions(-Werror)
endif()

# compile rawspeed
add_subdirectory(external/rawspeed)
list(APPEND LIB_DEPS rawspeed)
list(APPEND STATIC_LIBS rawspeed_static)

# osm gps map library
if(USE_MAP)
  find_package(OSMGpsMap)
  if(OSMGPSMAP_FOUND)
    message(STATUS "Map mode: enabled")
    add_definitions("-DHAVE_MAP")
    if(NOT OSMGPSMAP_VERSION VERSION_LESS 1.1.0)
      add_definitions("-DHAVE_OSMGPSMAP_110_OR_NEWER")
    endif()
    include_directories(SYSTEM ${OSMGPSMAP_INCLUDE_DIRS})
    list(APPEND LIBS ${OSMGPSMAP_LIBRARIES})
    add_definitions(${OSMGPSMAP_DEFINITIONS})
  else(OSMGPSMAP_FOUND)
    set(USE_MAP OFF)
    message(STATUS "Map mode: disabled, please install libosmgpsmap")
  endif(OSMGPSMAP_FOUND)

else(USE_MAP)
  message(STATUS "Map mode: disabled")
endif(USE_MAP)

# libcolord-gtk:
if(USE_COLORD)
  find_package(Colord)
  if(COLORD_FOUND)
    find_package(ColordGTK)
    if(COLORDGTK_FOUND)
      # We might make use of colord directly so link against it explicitly. just in case. move up if it is used without colord-gtk, too.
      include_directories(SYSTEM ${COLORD_INCLUDE_DIRS})
      list(APPEND LIBS ${COLORD_LIBRARIES})
      add_definitions(${COLORD_DEFINITIONS})

      # ... and also against libcolord-gtk to get the display profile
      include_directories(SYSTEM ${COLORDGTK_INCLUDE_DIRS})
      list(APPEND LIBS ${COLORDGTK_LIBRARIES})
      add_definitions(${COLORDGTK_DEFINITIONS})
      add_definitions("-DUSE_COLORDGTK")
    else(COLORDGTK_FOUND)
      message(STATUS "No libcolord-gtk found, this build will lack the ability to get the screen profile from colord")
      set(USE_COLORD OFF)
    endif(COLORDGTK_FOUND)
  else(COLORD_FOUND)
    message(STATUS "No libcolord found, this build will lack the ability to get the screen profile from colord")
    set(USE_COLORD OFF)
  endif(COLORD_FOUND)
endif(USE_COLORD)

if(BUILD_PRINT)
  find_package(Cups)
  if(CUPS_FOUND)
    include_directories(SYSTEM ${CUPS_INCLUDE_DIR})
    FILE(GLOB SOURCE_FILES_PRINT
      "common/cups_print.h"
      "common/cups_print.c"
      "common/printprof.c"
      )
    set(SOURCES ${SOURCES} ${SOURCE_FILES_PRINT})
    list(APPEND LIBS ${CUPS_LIBRARIES})
    add_definitions("-DHAVE_PRINT")
    message(STATUS "Print mode: enabled")
  else(CUPS_FOUND)
    set(BUILD_PRINT OFF)
    message(STATUS "Print mode: disabled, please install CUPS dev package")
  endif(CUPS_FOUND)
else(BUILD_PRINT)
  message(STATUS "Print mode: disabled")
endif(BUILD_PRINT)

#
# set development flags for our code
#
if(DT_CMAKE_INCLUDE_WHAT_YOU_USE)
  set(CMAKE_C_INCLUDE_WHAT_YOU_USE ${DT_CMAKE_INCLUDE_WHAT_YOU_USE})
  set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${DT_CMAKE_INCLUDE_WHAT_YOU_USE})
endif()

#
# Setup generation of preference dialog files..
#
if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
    COMMAND ${Xsltproc_BIN} --nonet --stringparam HAVE_OPENCL ${HAVE_OPENCL} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
    COMMENT "Generating preferences"
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
      COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl HAVE_OPENCL=${HAVE_OPENCL} > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
      COMMENT "Generating preferences"
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(FATAL_ERROR "Don't know how to generate preferences")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(
  generate_preferences ALL
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
if(USE_XMLLINT)
  add_dependencies(generate_preferences validate_darktableconfig_xml)
endif(USE_XMLLINT)

#
# Generate mapping of key strings to internal metadata id
#
add_custom_command(
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/create_metadata.sh ${CMAKE_CURRENT_SOURCE_DIR}/../tools/metadata.txt
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.h ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.c
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tools/create_metadata.sh ${CMAKE_CURRENT_SOURCE_DIR}/../tools/metadata.txt ${CMAKE_CURRENT_BINARY_DIR}
  COMMENT "Generating metadata .c/.h"
)
add_custom_target(
  generate_metadata ALL
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/create_metadata.sh ${CMAKE_CURRENT_SOURCE_DIR}/../tools/metadata.txt ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.h ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.c
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

#
# build libdarktable
#
add_library(lib_darktable SHARED ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.h ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.c ${CMAKE_BINARY_DIR}/src/version_gen.c ${SOURCES})

# cmake implicitly sets GENERATED on the source file in the directory of the custom command
# since this isn't the same directory we do have to manually set it
set_source_files_properties(${CMAKE_BINARY_DIR}/src/version_gen.c PROPERTIES GENERATED TRUE)

add_dependencies(lib_darktable generate_version)
add_dependencies(lib_darktable generate_preferences)
add_dependencies(lib_darktable generate_metadata)

if(APPLE)
  set_target_properties(lib_darktable PROPERTIES MACOSX_RPATH TRUE)
endif(APPLE)
set_target_properties(lib_darktable PROPERTIES OUTPUT_NAME darktable)
set_target_properties(lib_darktable PROPERTIES LINKER_LANGUAGE C)

#
# Link external libs
#

add_dependencies(lib_darktable ${LIB_DEPS})
target_link_libraries(lib_darktable ${STATIC_LIBS})
target_link_libraries(lib_darktable ${LIBS})

unset(LIBS)
unset(STATIC_LIBS)
unset(LIB_DEPS)

#
# Install lib_darktable
#
if(WIN32)
  # Windows needs its DLLs in the same directory as the executable to find them.
  install(TARGETS lib_darktable DESTINATION bin)
else(WIN32)
  install(TARGETS lib_darktable DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable)
endif(WIN32)

# Compile views modules
add_subdirectory(views)

# Compile lighttable modules
add_subdirectory(libs)

# Compile darkroom modules
add_subdirectory(iop)

# Compile export modules
add_subdirectory(imageio)

if(HAVE_OPENCL)
  # have a small test program, to be run as root on some systems to enable the driver to run cl stuff
  add_subdirectory(cltest)
endif(HAVE_OPENCL)

# have a command line interface
add_subdirectory(cli)

# have a command line utility to generate all the thumbnails
add_subdirectory(generate-cache)

# have a small test program that verifies your color management setup
if(BUILD_CMSTEST)
  add_subdirectory(cmstest)
endif(BUILD_CMSTEST)

# have a gui tool to create CLUTs from colour chart targets
add_subdirectory(chart)

#
# build darktable executable
#
add_executable(darktable ${SUBSYSTEM_MACOSX} ${SUBSYSTEM_WIN32} main.c ${RESOURCE_OBJECT})
set_target_properties(darktable PROPERTIES LINKER_LANGUAGE C)
if(APPLE)
  set_target_properties(lib_darktable PROPERTIES LINK_FLAGS "-framework Carbon -framework AppKit")
endif(APPLE)
target_link_libraries(darktable lib_darktable)
install(TARGETS darktable DESTINATION ${CMAKE_INSTALL_BINDIR})

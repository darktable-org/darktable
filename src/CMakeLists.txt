cmake_minimum_required(VERSION 2.6)

cmake_policy(SET CMP0011 NEW)

include(CheckCCompilerFlag)

#
# Add files for libdarktable
#
FILE(GLOB SOURCE_FILES
  "bauhaus/bauhaus.c"
  "common/cache.c"
  "common/calculator.c"
  "common/collection.c"
  "common/colorlabels.c"
  "common/colorspaces.c"
  "common/curve_tools.c"
  "common/darktable.c"
  "common/database.c"
  "common/dbus.c"
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
  "common/interpolation.c"
  "common/metadata.c"
  "common/mipmap_cache.c"
  "common/styles.c"
  "common/selection.c"
  "common/tags.c"
  "common/utility.c"
  "common/variables.c"
  "common/pwstorage/backend_kwallet.c"
  "common/pwstorage/pwstorage.c"
  "common/opencl.c"
  "common/dynload.c"
  "common/dlopencl.c"
  "common/ratings.c"
  "control/control.c"
  "control/jobs/control_jobs.c"
  "control/jobs/develop_jobs.c"
  "control/jobs/film_jobs.c"
  "control/jobs/image_jobs.c"
  "control/signal.c"
  "develop/develop.c"
  "develop/imageop.c"
  "develop/lightroom.c"
  "develop/pixelpipe.c"
  "develop/blend.c"
  "develop/blend_gui.c"
  "develop/tiling.c"
  "develop/masks/masks.c"
  "dtgtk/button.c"
  "dtgtk/icon.c"
  "dtgtk/label.c"
  "dtgtk/slider.c"
  "dtgtk/gradientslider.c"
  "dtgtk/tristatebutton.c"
  "dtgtk/togglebutton.c"
  "dtgtk/paint.c"
  "dtgtk/resetlabel.c"
  "gui/accelerators.c"
  "gui/contrast.c"
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

set(SOURCES ${SOURCE_FILES} ${HEADER_FILES})


include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/LibRaw)

# initial compiler flags
add_definitions("-DHAVE_CONFIG_H")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")

# Check for openmp
find_package(OpenMP)
if(OPENMP_FOUND AND USE_OPENMP)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
  if(WIN32)
    list(APPEND LIBS gomp)
  endif(WIN32)
  add_definitions("-DHAVE_OPENMP")
endif(OPENMP_FOUND AND USE_OPENMP)

if(USE_DARKTABLE_PROFILING)
	add_definitions(-DUSE_DARKTABLE_PROFILING)
	set(SOURCES ${SOURCES} "common/profiling.c")
endif()

#
# Find all other required libraries for building
#
find_package(Glib 2.30 REQUIRED)
include_directories(${Glib_INCLUDE_DIRS})
list(APPEND LIBS ${Glib_LIBRARIES})
add_definitions(${Glib_DEFINITIONS})

find_package(GTK2 2.24 REQUIRED gtk)
include_directories(${GTK2_INCLUDE_DIRS})
list(APPEND LIBS ${GTK2_LIBRARIES})
add_definitions(${GTK2_DEFINITIONS})

# Check for libxml2 / broken cmake module can't be included in the foreach() below
find_package(LibXml2 2.6 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})
list(APPEND LIBS ${LIBXML2_LIBRARIES})
add_definitions(${LIBXML2_DEFINITIONS})


if(USE_CAMERA_SUPPORT)
  find_package(Gphoto2)
  include_directories(${Gphoto2_INCLUDE_DIRS})
  list(APPEND LIBS ${Gphoto2_LIBRARIES})
  add_definitions(${Gphoto2_DEFINITIONS})
endif(USE_CAMERA_SUPPORT)

# INotify isn't used anymore as it seems
find_package(INotify)
if(INOTIFY_FOUND)
  include_directories(${INOTIFY_INCLUDE_DIRS})
  list(APPEND LIBS ${INOTIFY_LIBRARIES})
  add_definitions(${INOTIFY_DEFINITIONS})
endif(INOTIFY_FOUND)

find_package(OpenEXR)
if(OPENEXR_FOUND)
  include_directories(${OpenEXR_INCLUDE_DIRS})
  list(APPEND LIBS ${OpenEXR_LIBRARIES})
  add_definitions(${OpenEXR_DEFINITIONS})
  list(APPEND SOURCES "common/imageio_exr.cc")
endif(OPENEXR_FOUND)

find_package(WebP 0.3.0)
if(WEBP_FOUND)
  include_directories(${WebP_INCLUDE_DIRS})
  list(APPEND LIBS ${WebP_LIBRARIES})
  add_definitions(${WebP_DEFINITIONS})
endif(WEBP_FOUND)

# Find all the libs that don't require extra parameters
foreach(lib ${OUR_LIBS} LensFun GIO GThread GModule Cairo PangoCairo PThread Rsvg2 GDK-PixBuf LibXml2 Sqlite3 Exiv2 CURL PNG JPEG TIFF LCMS2)
  find_package(${lib} REQUIRED)
  include_directories(${${lib}_INCLUDE_DIRS})
  list(APPEND LIBS ${${lib}_LIBRARIES})
  add_definitions(${${lib}_DEFINITIONS})
endforeach(lib)

if(USE_LUA)
  find_package(Lua52 5.2) # add EXACT if 5.3 doesn't work for us
  if(LUA52_FOUND)
    message(STATUS "Lua support: Enabled")
    include_directories(${LUA52_INCLUDE_DIRS})
    list(APPEND LIBS ${LUA52_LIBRARIES})
  else()
    if(DONT_USE_INTERNAL_LUA)
	    message(STATUS "Lua support: System library not found (to use darktable's version use -DDONT_USE_INTERNAL_LUA=Off)")
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
      "lua/call.c"
      "lua/configuration.c"
      "lua/database.c"
      "lua/events.c"
      "lua/film.c"
      "lua/glist.c"
      "lua/gui.c"
      "lua/image.c"
      "lua/init.c"
      "lua/lua.c"
      "lua/modules.c"
      "lua/preferences.c"
      "lua/print.c"
      "lua/storage.c"
      "lua/styles.c"
      "lua/tags.c"
      "lua/types.c"
      )
    set(SOURCES ${SOURCES} ${SOURCE_FILES_LUA})
  endif(USE_LUA)
else(USE_LUA)
  message(STATUS "Lua support: Disabled")
endif(USE_LUA)

if(USE_NLS)
  find_package(Gettext)
  if(Gettext_FOUND)
    include_directories(${Gettext_INCLUDE_DIRS})
    list(APPEND LIBS ${Gettext_LIBRARIES})
    add_definitions("-DUSE_GETTEXT")
    message(STATUS "Internationalization: Enabled")
  else()
    message(STATUS "Internationalization: Disabled (libintl not found)")
  endif()
endif(USE_NLS)

if(USE_FLICKR)
  find_package(FLICKCURL)
  if(FLICKCURL_FOUND)
    include_directories(${FLICKCURL_INCLUDE_DIRS})
    list(APPEND LIBS ${FLICKCURL_LIBRARIES})
    add_definitions(${FLICKCURL_DEFINITIONS})
  endif(FLICKCURL_FOUND)
endif(USE_FLICKR)

if(USE_GLIBJSON)
  find_package(JsonGlib)
  if(JsonGlib_FOUND)
    include_directories(${JsonGlib_INCLUDE_DIRS})
    list(APPEND LIBS ${JsonGlib_LIBRARIES})
  endif(JsonGlib_FOUND)
endif(USE_GLIBJSON)

if(USE_OPENJPEG)
  find_package(OpenJPEG)
  if(OPENJPEG_FOUND)
    list(APPEND SOURCES "common/imageio_j2k.c")
    include_directories(${OPENJPEG_INCLUDE_DIRS})
    list(APPEND LIBS ${OPENJPEG_LIBRARIES})
    add_definitions(${OPENJPEG_DEFINITIONS})
    add_definitions("-DHAVE_OPENJPEG")
    if(OPENJPEG_VERSION VERSION_LESS "1.5.0")
      message(STATUS "OpenJPEG version ${OPENJPEG_VERSION} found. Only 1.5 and newer support reading of icc profiles.")
    else()
      add_definitions("-DHAVE_OPENJPEG_ICC")
    endif()
  endif(OPENJPEG_FOUND)
endif(USE_OPENJPEG)

#
# Detect compile of optional pwstorage backends
#
if(USE_GNOME_KEYRING)
  find_package(GnomeKeyring)
  if(GNOMEKEYRING_FOUND)
    set(SOURCES ${SOURCES} "common/pwstorage/backend_gkeyring.c")
    include_directories(${GnomeKeyring_INCLUDE_DIRS})
    list(APPEND LIBS ${GnomeKeyring_LIBRARIES})
    add_definitions("-DHAVE_GKEYRING")
  endif(GNOMEKEYRING_FOUND)
endif(USE_GNOME_KEYRING)

if(USE_MAC_INTEGRATION)
  find_package(MacIntegration)
  if(MACINTEGRATION_FOUND)
    include_directories(${MacIntegration_INCLUDE_DIRS})
    list(APPEND LIBS ${MacIntegration_LIBRARIES})
    add_definitions("-DMAC_INTEGRATION")
  endif(MACINTEGRATION_FOUND)
endif(USE_MAC_INTEGRATION)

if(USE_UNITY)
  find_package(Unity)
  if(UNITY_FOUND)
    add_definitions(-DHAVE_UNITY)
    include_directories(${UNITY_INCLUDE_DIRS})
    list(APPEND LIBS ${UNITY_LIBRARIES})
  endif(UNITY_FOUND)
endif(USE_UNITY)

if(USE_GRAPHICSMAGICK)
  find_package(GraphicsMagick)
  if(GraphicsMagick_FOUND)
    add_definitions(-DHAVE_GRAPHICSMAGICK)
    include_directories(${GraphicsMagick_INCLUDE_DIRS})
    list(APPEND LIBS ${GraphicsMagick_LIBRARIES})
  endif(GraphicsMagick_FOUND)
endif(USE_GRAPHICSMAGICK)


# build opengl slideshow viewer?
if(BUILD_SLIDESHOW)
  find_package(SDL)
  find_package(OpenGL)
  if(SDL_FOUND AND OPENGL_FOUND)
    add_subdirectory(dtview)
  endif(SDL_FOUND AND OPENGL_FOUND)
endif(BUILD_SLIDESHOW)

# needed for the file manager
if(APRIL_FOOLS)
  find_package(VTE)
  if(VTE_FOUND)
    include_directories(${VTE_INCLUDE_DIRS})
    list(APPEND LIBS ${VTE_LIBRARIES})
  endif(VTE_FOUND)
endif(APRIL_FOOLS)

#
# Add HAVE_xxx defines used by darktable
#
if(INOTIFY_FOUND)
  add_definitions("-DHAVE_INOTIFY")
endif(INOTIFY_FOUND)

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


#
# Set default compile flags for GCC
#
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fno-strict-aliasing")
# c++ code doesn't like -Werror (libraw)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fno-strict-aliasing")

if(NOT WIN32)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
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

  add_definitions("-DLIBRAW_NODLL")
  list(APPEND LIBS ws2_32)
  list(APPEND LIBS msvcrt) # Needed for _aligned_alloc()
  list(APPEND SOURCES "win/getrusage.c")
endif(WIN32)

if(NOT CUSTOM_CFLAGS)

if(NOT APPLE AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  # apple build fails on deprecated warnings..
  # and too many warnings reported by Clang for now
  if(NOT SOURCE_PACKAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
  endif(NOT SOURCE_PACKAGE)

  # gcc-4.8 build fails on unused local typedefs in gmacros.h:162
  # to be removed when glib will get fixed
  if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
    if (GCC_VERSION VERSION_GREATER 4.8 OR GCC_VERSION VERSION_EQUAL 4.8)
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-local-typedefs")
    endif()
  endif()
endif(NOT APPLE AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")

if(NOT BINARY_PACKAGE_BUILD)
	MESSAGE("-- Checking for -march=native support")
	CHECK_C_COMPILER_FLAG("-march=native" MARCHNATIVE)
	if (MARCHNATIVE)
		set(MARCH "-march=native")
	else()
		MESSAGE("-- Checking for -mtune=native support")
		CHECK_C_COMPILER_FLAG("-mtune=native" MTUNENATIVE)
		if (MTUNENATIVE)
			set(MARCH "-mtune=native")
		else()
			set(MARCH "-mtune=generic")
		endif()
	endif()
else()
 	set(MARCH "-mtune=generic")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse2 -g")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -ffast-math ${MARCH}")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse2 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -ffast-math ${MARCH}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")
if(CMAKE_COMPILER_IS_GNUCC)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mfpmath=sse")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -fexpensive-optimizations")
endif()
if(CMAKE_COMPILER_IS_GNUCXX)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpmath=sse")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fexpensive-optimizations")
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
# Setup generation of preference dialog files..
#
if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
	add_custom_command(
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
		COMMAND ${Xsltproc_BIN} --stringparam HAVE_OPENCL ${HAVE_OPENCL} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
	)
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
	if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
		add_custom_command(
			DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml
			OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
			COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_BINARY_DIR}/../data/darktableconfig.xml ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl HAVE_OPENCL=${HAVE_OPENCL} > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
		)
	else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
		message(FATAL_ERROR "Don't know how to generate preferences")
	endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

#
# Generate mapping of key strings to internal metadata id
#
add_custom_command(
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/create_metadata.sh ${CMAKE_CURRENT_SOURCE_DIR}/../tools/metadata.txt
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.h ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.c
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tools/create_metadata.sh ${CMAKE_CURRENT_SOURCE_DIR}/../tools/metadata.txt ${CMAKE_CURRENT_BINARY_DIR}
)

# this is a dummy library whose sole purpose is to serve as a dependency for subdirs, such as imageio/storage/gallery.
# cmake can't depend on custom targets in other directories than the current one.
add_library(metadata_dummy ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.c ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.h)

#
# build libdarktable
#
add_library(lib_darktable STATIC SHARED ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.h ${CMAKE_CURRENT_BINARY_DIR}/metadata_gen.c ${SOURCES})
if(APPLE)
  set_target_properties(lib_darktable PROPERTIES INSTALL_NAME_DIR "@executable_path/../lib/darktable")
endif(APPLE)
set_target_properties(lib_darktable PROPERTIES OUTPUT_NAME darktable)
set_target_properties(lib_darktable PROPERTIES LINKER_LANGUAGE C)

#
# the libraw part is a bit of a hack:
# the static linking didn't work since it was pulling -lstdc++ and -lm into linker flags.
# so we do a custom dependency and pretend an imported liblibraw_r.a so no other -l are
# appended.
#
add_dependencies(lib_darktable libraw_r)
add_library(libraw_static STATIC IMPORTED)
set_target_properties(libraw_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/LibRaw/liblibraw_r.a)
set(LIBRAW_STATIC_LIBS libraw_static)

target_link_libraries(lib_darktable ${LIBRAW_STATIC_LIBS})
list(APPEND STATIC_LIBS ${LIBRAW_STATIC_LIBS})

# same for librawspeed (can switch it off with -DDONT_USE_RAWSPEED, for macs):
if(NOT DONT_USE_RAWSPEED)
  add_definitions("-DHAVE_RAWSPEED")
  add_dependencies(lib_darktable rawspeed)
  add_library(rawspeed_static STATIC IMPORTED)
  set_target_properties(rawspeed_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/rawspeed/librawspeed.a)
  target_link_libraries(lib_darktable rawspeed_static)
  # compile rawspeed
  add_subdirectory(external/rawspeed)
  list(APPEND STATIC_LIBS rawspeed_static)
endif(NOT DONT_USE_RAWSPEED)

# osm gps map library
if(USE_GEO)
  find_package(LibSoup2)
  if(LIBSOUP2_FOUND)
    include_directories(${LIBSOUP2_INCLUDE_DIRS})
    list(APPEND LIBS ${LIBSOUP2_LIBRARIES})
    add_definitions(${LIBSOUP2_DEFINITIONS})
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/osm-gps-map/src)
    add_dependencies(lib_darktable osmgpsmap)
    add_library(osmgpsmap_static STATIC IMPORTED)
    set_target_properties(osmgpsmap_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/osm-gps-map/libosmgpsmap.a)
    target_link_libraries(lib_darktable osmgpsmap_static)
    add_subdirectory(external/osm-gps-map)
    list(APPEND STATIC_LIBS osmgpsmap_static)
    add_definitions("-DHAVE_MAP")
    message(STATUS "Map mode: enabled")
  else(LIBSOUP2_FOUND)
    set(USE_GEO OFF)
    message(STATUS "Map mode: disabled, please install libsoup2")
  endif(LIBSOUP2_FOUND)
else(USE_GEO)
  message(STATUS "Map mode: disabled")
endif(USE_GEO)

# libcolord-gtk:
if(USE_COLORD)
  find_package(Colord)
  if(COLORD_FOUND)
    include_directories(${COLORD_INCLUDE_DIRS})
    list(APPEND LIBS ${COLORD_LIBRARIES})
    # we ship our own copy of libcolord-gtk - the one packaged in distros is linked against gtk3 :(
    add_dependencies(lib_darktable colord-gtk)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/colord-gtk/src)
    add_library(colord-gtk_static STATIC IMPORTED)
    set_target_properties(colord-gtk_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/colord-gtk/libcolord-gtk.a)
    target_link_libraries(lib_darktable colord-gtk_static)
    add_subdirectory(external/colord-gtk)
    list(APPEND STATIC_LIBS colord-gtk_static)
    add_dependencies(colord-gtk colord)
    add_definitions("-DUSE_COLORDGTK")
  endif(COLORD_FOUND)
endif(USE_COLORD)

if(USE_SQUISH)
  # libsquish for compressed thumbnails:
  add_dependencies(lib_darktable squish)
  add_library(squish_static STATIC IMPORTED)
  set_target_properties(squish_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/squish/libsquish.a)
  target_link_libraries(lib_darktable squish_static)
  add_subdirectory(external/squish)
  add_definitions("-DHAVE_SQUISH")
endif(USE_SQUISH)

if(USE_LUA)
  if(NOT LUA52_FOUND)
    add_dependencies(lib_darktable liblua) # 2nd half of the uglyness :/
  endif(NOT LUA52_FOUND)
	# liblautoc for lua automated interface generation
	add_dependencies(lib_darktable lautoc)
	add_library(lautoc_static STATIC IMPORTED)
	set_target_properties(lautoc_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/LuaAutoC/liblautoc.a)
	target_link_libraries(lib_darktable lautoc_static)
	include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/LuaAutoC/include)
	add_subdirectory(external/LuaAutoC)
endif(USE_LUA)

###### GTK+3 port ######

#
#  Do not include individual headers
#
add_definitions("-DGTK_DISABLE_SINGLE_INCLUDES")

#
# Dirty hack to enforce GTK3 behaviour in GTK2: "Replace GDK_<keyname> with GDK_KEY_<keyname>"
#
add_definitions("-D__GDK_KEYSYMS_COMPAT_H__")

#
# Use accessor functions instead of direct access
#
add_definitions("-DGSEAL_ENABLE")

#
#  Do not use deprecated symbols
#
add_definitions("-DGDK_DISABLE_DEPRECATED")
add_definitions("-DGTK_DISABLE_DEPRECATED")
add_definitions("-Werror=implicit-function-declaration")

###### GTK+3 port ######

target_link_libraries(lib_darktable ${LIBS})

if(WIN32)
  # Windows needs its DLLs in the same directory as the executable to find them.
  install(TARGETS lib_darktable DESTINATION bin)
else(WIN32)
  install(TARGETS lib_darktable DESTINATION ${LIB_INSTALL}/darktable)
endif(WIN32)

# make sure static libs is first
list(INSERT LIBS 0 lib_darktable ${STATIC_LIBS})

# Compile libraw modules
add_subdirectory(external/LibRaw)

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

# have a small test program that verifies your color management setup
if(BUILD_CMSTEST)
  add_subdirectory(cmstest)
endif(BUILD_CMSTEST)

#
# build darktable executable
#
add_executable(darktable ${SUBSYSTEM_MACOSX} ${SUBSYSTEM_WIN32} main.c ${RESOURCE_OBJECT})
if(NOT WIN32)
  # Windows doesn't know the concept of RPATHs :(
  set_target_properties(darktable PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  set_target_properties(darktable PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
  set_target_properties(darktable PROPERTIES INSTALL_RPATH $ORIGIN/../${LIB_INSTALL}/darktable)
endif(NOT WIN32)
set_target_properties(darktable PROPERTIES LINKER_LANGUAGE C)
if(APPLE)
  set_target_properties(lib_darktable PROPERTIES LINK_FLAGS "-framework Carbon")
endif(APPLE)
target_link_libraries(darktable ${LIBS})
install(TARGETS darktable DESTINATION bin)





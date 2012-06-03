cmake_minimum_required(VERSION 2.6)

include(CheckCCompilerFlag)

#
# Add files for libdarktable
#
FILE(GLOB SOURCE_FILES
  "bauhaus/bauhaus.c"
  "common/cache.c"
  "common/collection.c" 
  "common/colorlabels.c" 
  "common/colorspaces.c" 
  "common/curve_tools.c"
  "common/darktable.c" 
  "common/database.c"
  "common/exif.cc" 
  "common/film.c" 
  "common/file_location.c" 
  "common/fswatch.c" 
  "common/history.c" 
  "common/image.c" 
  "common/image_cache.c" 
  "common/image_compression.c" 
  "common/imageio.c" 
  "common/imageio_exr.cc" 
  "common/imageio_jpeg.c" 
  "common/imageio_module.c" 
  "common/imageio_pfm.c" 
  "common/imageio_rgbe.c" 
  "common/imageio_tiff.c" 
  "common/imageio_rawspeed.cc" 
  "common/interpolation.c" 
  "common/metadata.c" 
  "common/mipmap_cache.c"
  "common/styles.c" 
  "common/similarity.c" 
  "common/selection.c"
  "common/tags.c" 
  "common/utility.c" 
  "common/variables.c" 
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
  "develop/pixelpipe.c"
  "develop/blend.c"
  "develop/blend_gui.c"
  "develop/tiling.c"
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
  "gui/gtk.c"  
  "gui/preferences.c"  
  "gui/presets.c"  
  "gui/styles_dialog.c"  
  "libs/lib.c"
  "views/view.c"
  )
FILE(GLOB HEADER_FILES "*.h" "common/*.h" "external/CL/*.h" "control/*.h" "iop/*.h" "libs/*.h", "views/*.h")

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
  add_definitions("-DHAVE_OPENMP")
endif(OPENMP_FOUND AND USE_OPENMP)

if(USE_DARKTABLE_PROFILING)
	add_definitions(-DUSE_DARKTABLE_PROFILING)
	set(SOURCES ${SOURCES} "common/profiling.c")
endif()

#
# Find all other required libraries for building
#
find_package(GTK2 2.6 REQUIRED gtk)
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

# Find all the libs that don't require extra parameters
foreach(lib ${OUR_LIBS} LensFun GIO GThread GModule Cairo PangoCairo PThread Rsvg2 GDK-PixBuf LibXml2 INotify Sqlite3 Exiv2  CURL PNG JPEG TIFF OpenEXR LCMS2)
  find_package(${lib} REQUIRED)
  include_directories(${${lib}_INCLUDE_DIRS})
  list(APPEND LIBS ${${lib}_LIBRARIES})
  add_definitions(${${lib}_DEFINITIONS})
endforeach(lib)

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

#
# Detect compile of optional pwstorage backends
#
if(USE_KWALLET) 
find_package(DBUSGLIB)
  if(DBUSGLIB_FOUND)
    list(APPEND SOURCES "common/pwstorage/backend_kwallet.c")
    include_directories(${DBUSGLIB_INCLUDE_DIRS})
    list(APPEND LIBS ${DBUSGLIB_LIBRARIES})
    add_definitions("-DHAVE_KWALLET")
  endif(DBUSGLIB_FOUND)
endif(USE_KWALLET)

if(USE_GNOME_KEYRING)
  find_package(GnomeKeyring)
  if(GNOMEKEYRING_FOUND)
    set(SOURCES ${SOURCES} "common/pwstorage/backend_gkeyring.c")
    include_directories(${GnomeKeyring_INCLUDE_DIRS})
    list(APPEND LIBS ${GnomeKeyring_LIBRARIES})
    add_definitions("-DHAVE_GKEYRING")
  endif(GNOMEKEYRING_FOUND)
endif(USE_GNOME_KEYRING)

# have a small test program, to be run as root on some systems to enable the driver to run cl stuff
add_subdirectory(cltest)

if(USE_UNITY)
  find_package(Unity)
  if(UNITY_FOUND)
    add_definitions(-DHAVE_UNITY)
    include_directories(${UNITY_INCLUDE_DIRS})
    list(APPEND LIBS ${UNITY_LIBRARIES})
  endif(UNITY_FOUND)
endif(USE_UNITY)

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
endif(GPHOTO2_FOUND)


#
# Set default compile flags for GCC
#
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fno-strict-aliasing")
# c++ code doesn't like -Werror (libraw)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fno-strict-aliasing")

if(NOT APPLE AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  # apple build fails on deprecated warnings..
  # and too many warnings reported by Clang for now
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
endif(NOT APPLE AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")

if(NOT WIN32)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
endif(NOT WIN32)

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
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -ffast-math -fexpensive-optimizations ${MARCH}")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g -D_DEBUG")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse2 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -ffast-math -fexpensive-optimizations ${MARCH}")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g -D_DEBUG")
if(CMAKE_COMPILER_IS_GNUCC)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mfpmath=sse")
endif()
if(CMAKE_COMPILER_IS_GNUCXX)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpmath=sse")
endif()

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
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
		COMMAND ${Xsltproc_BIN} --stringparam HAVE_OPENCL ${HAVE_OPENCL} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
	)
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
	if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
		add_custom_command(
			DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml
			OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
			COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_prefs.xsl HAVE_OPENCL=${HAVE_OPENCL} > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
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
if(APPLE)
  set(LIBRAW_STATIC_LIBS libraw_static stdc++)
else(APPLE)
  set(LIBRAW_STATIC_LIBS libraw_static)
endif(APPLE)

target_link_libraries(lib_darktable ${LIBS} ${LIBRAW_STATIC_LIBS})

# same for librawspeed (can switch it off with -DDONT_USE_RAWSPEED, for macs):
if(NOT DONT_USE_RAWSPEED)
  add_definitions("-DHAVE_RAWSPEED")
  add_dependencies(lib_darktable rawspeed)
  add_library(rawspeed_static STATIC IMPORTED)
  set_target_properties(rawspeed_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/rawspeed/librawspeed.a)
  target_link_libraries(lib_darktable ${LIBS} rawspeed_static)
  # compile rawspeed
  add_subdirectory(external/rawspeed)
endif(NOT DONT_USE_RAWSPEED)

# libsquish for compressed thumbnails:
add_dependencies(lib_darktable squish)
add_library(squish_static STATIC IMPORTED)
set_target_properties(squish_static PROPERTIES IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/external/squish/libsquish.a)
target_link_libraries(lib_darktable ${LIBS} squish_static)
add_subdirectory(external/squish)

# libcolord-gtk:
find_package(ColordGtk)
if(COLORDGTK_FOUND)
  message(STATUS "Using colord to fetch color profiles")
  include_directories(${COLORDGTK_INCLUDE_DIRS})
  list(APPEND LIBS ${COLORDGTK_LIBRARIES})
  add_definitions("-DUSE_COLORDGTK")
#   add_dependencies(lib_darktable colord-gtk)
else(COLORDGTK_FOUND)
  message(STATUS "Either colord is missing or too old (<0.1.20)")
endif(COLORDGTK_FOUND)


install(TARGETS lib_darktable DESTINATION ${LIB_INSTALL}/darktable)
list(APPEND LIBS lib_darktable)

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




#
# build darktable executable
#
add_executable(darktable ${SUBSYSTEM_MACOSX} ${SUBSYSTEM_WIN32} main.c ${RESOURCE_OBJECT})
set_target_properties(darktable PROPERTIES CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set_target_properties(darktable PROPERTIES CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set_target_properties(darktable PROPERTIES INSTALL_RPATH $ORIGIN/../${LIB_INSTALL}/darktable)
set_target_properties(darktable PROPERTIES LINKER_LANGUAGE C)
if(APPLE AND USE_OPENCL)
set_target_properties(darktable PROPERTIES LINK_FLAGS -Wl,-syslibroot,/Developer-old/SDKs/MacOSX10.6.sdk)
endif(APPLE AND USE_OPENCL)
target_link_libraries(darktable ${LIBS})
install(TARGETS darktable DESTINATION bin)





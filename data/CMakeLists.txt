cmake_minimum_required(VERSION 2.6)

add_subdirectory(pixmaps)

if(USE_OPENCL)
	add_subdirectory(kernels)
endif(USE_OPENCL)

#
# Generate and instal darktable.css
#
if ("${PC_GTK3_VERSION}" VERSION_GREATER "3.13.6")
  set(GTK_CSS_CLASS "checked")
else ()
  set(GTK_CSS_CLASS "active")
endif ()
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/darktable.css.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.css )
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.css DESTINATION ${SHARE_INSTALL}/darktable)

#
# web gallery export support files:
#
FILE(GLOB WEB_FILES "style/*")
install(FILES ${WEB_FILES} DESTINATION ${SHARE_INSTALL}/darktable/style)

FILE(GLOB WEB_FILES "js/*")
install(FILES ${WEB_FILES} DESTINATION ${SHARE_INSTALL}/darktable/js)

#
# latex book template support files:
#
FILE(GLOB WEB_FILES "latex/*")
install(FILES ${WEB_FILES} DESTINATION ${SHARE_INSTALL}/darktable/latex)

if(USE_LUA)
#
# lua system scripts
#
install(DIRECTORY "lua" DESTINATION ${SHARE_INSTALL}/darktable/)
install(FILES luarc DESTINATION ${SHARE_INSTALL}/darktable/)
endif(USE_LUA)
#
# Install (and generate when necessary) other system shares
#
file(GLOB PO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../po/*.po")
add_custom_command(
		OUTPUT darktable.desktop
		SOURCE darktable.desktop.in
		COMMAND ${intltool_merge_BIN} -d ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_SOURCE_DIR}/darktable.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop
		MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/darktable.desktop.in
		DEPENDS ${PO_FILES}
)
add_custom_target(darktable.desktop ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop DESTINATION ./share/applications)
install(FILES darktable.appdata.xml DESTINATION ./share/appdata)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
install(FILES ${WATERMARKS} DESTINATION ${SHARE_INSTALL}/darktable/watermarks)

#
# Install gdb command file for backtrace generation
#
install(FILES gdb_commands DESTINATION ${SHARE_INSTALL}/darktable)

#
# Transform darktableconfig.xml into darktablerc
#

set(DEFCONFIG_OPENCL "true")
if(APPLE)
  # OpenCL support on OS X isn't stable enough to enable it by default
  set(DEFCONFIG_OPENCL "false")
endif(APPLE)
set(DEFCONFIG_AUDIOPLAYER "aplay")
if(APPLE)
  # OS X doesn't have aplay, but afplay should do
  set(DEFCONFIG_AUDIOPLAYER "afplay")
endif(APPLE)
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/darktableconfig.xml.in ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml )
execute_process( COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.dtd)


if(USE_XMLLINT)
	execute_process(
		COMMAND ${Xmllint_BIN} --valid ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/
		OUTPUT_QUIET ERROR_QUIET
		RESULT_VARIABLE Xmllint_RES
	)
	if(NOT ${Xmllint_RES} EQUAL 0 )
		message(FATAL_ERROR "data/darktableconfig.xml is not valid")
	endif(NOT ${Xmllint_RES} EQUAL 0 )
endif(USE_XMLLINT)

if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
    COMMAND ${Xsltproc_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
      COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(FATAL_ERROR "Don't know how to generate darktablerc")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(darktablerc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc)

# first of all install darktablerc file into share as template
# postinst script should copy this into users homedirectory
# $HOME/.config/darktable/darktable.rc
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktablerc DESTINATION ${SHARE_INSTALL}/darktable/)


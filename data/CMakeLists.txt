cmake_minimum_required(VERSION 2.8.5)

add_subdirectory(pixmaps)

if(USE_OPENCL)
	add_subdirectory(kernels)
endif(USE_OPENCL)

#
# Generate and instal darktable.css
#
if ("${GTK3_VERSION}" VERSION_GREATER "3.19.0")
  configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/darktable.css.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.css )
else ()
  if ("${GTK3_VERSION}" VERSION_GREATER "3.13.6")
    set(GTK_CSS_CLASS "checked")
  else ()
    set(GTK_CSS_CLASS "active")
  endif ()
  configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/darktable-3.18.css.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.css )
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.css DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable)

#
# web gallery export support files:
#
FILE(GLOB WEB_FILES "style/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/style)

FILE(GLOB WEB_FILES "js/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/js)

#
# latex book template support files:
#
FILE(GLOB WEB_FILES "latex/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/latex)

if(USE_LUA)
#
# lua system scripts
#
install(DIRECTORY "lua" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable)
install(FILES luarc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable)
endif(USE_LUA)
#
# Install (and generate when necessary) other system shares
#
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/darktable.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop.in)
file(GLOB PO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../po/*.po")
add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop
		SOURCE ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop.in
		COMMAND ${intltool_merge_BIN} --desktop-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop
		MAIN_DEPENDENCY ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop.in
		DEPENDS ${PO_FILES}
)
add_custom_target(darktable.desktop_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop)
if(${VALIDATE_DESKTOP_FILE})
  add_custom_target(
    validate_darktable_desktop ALL
    COMMAND ${desktop_file_validate_BIN} --warn-kde ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/
    COMMENT "Checking validity of darktable.desktop"
    )
  add_dependencies(darktable.desktop_file validate_darktable_desktop)
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable.appdata.xml
		SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in
		COMMAND ${intltool_merge_BIN} --xml-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.appdata.xml
		MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in
		DEPENDS ${PO_FILES}
)
add_custom_target(darktable.appdata_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktable.appdata.xml)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/appdata)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
install(FILES ${WATERMARKS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/watermarks)

#
# Install gdb command file for backtrace generation
#
install(FILES gdb_commands DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable)

#
# Install noiseprofiles
#
install(FILES noiseprofiles.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable)

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
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.dtd COPYONLY )


if(USE_XMLLINT)
  add_custom_target(
    validate_darktableconfig_xml ALL
    COMMAND ${Xmllint_BIN} --valid --noout ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of data/darktableconfig.xml"
  )
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

add_custom_target(
  darktablerc_file ALL
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
)
if(USE_XMLLINT)
  add_dependencies(darktablerc_file validate_darktableconfig_xml)
endif(USE_XMLLINT)

# first of all install darktablerc file into share as template
# postinst script should copy this into users homedirectory
# $HOME/.config/darktable/darktable.rc
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktablerc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable)


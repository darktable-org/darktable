add_subdirectory(pixmaps ${DARKTABLE_DATADIR}/pixmaps)

if(USE_OPENCL)
	add_subdirectory(kernels ${DARKTABLE_DATADIR}/kernels)
endif(USE_OPENCL)

FILE(GLOB THEME_FILES "themes/*.css")
FILE(COPY ${THEME_FILES} DESTINATION "${DARKTABLE_DATADIR}/themes")
install(FILES ${THEME_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/themes COMPONENT DTApplication)

#
# web gallery export support files:
#
FILE(GLOB WEB_FILES "style/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/style COMPONENT DTApplication)

install(DIRECTORY "pswp" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

#
# latex book template support files:
#
FILE(GLOB WEB_FILES "latex/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/latex COMPONENT DTApplication)

if(USE_LUA)
#
# lua system scripts
#
FILE(COPY lua luarc DESTINATION "${DARKTABLE_DATADIR}")
install(DIRECTORY "lua" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)
install(FILES luarc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)
endif(USE_LUA)

#
# Install (and generate when necessary) other system shares
#
if(NOT WIN32)
  if("avif" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/avif)
  endif()
  if("cr3" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/x-canon-cr3)
  endif()
  if("exr" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/x-exr image/aces)
  endif()
  if("heif" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/heif image/heic)
  endif()
  if("jp2" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/jp2)
  endif()
  if("jxl" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/jxl)
  endif()
  if("webp" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/webp)
  endif()
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/darktable.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop.in)
  file(GLOB PO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../po/*.po")
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop
    SOURCE ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop.in
    COMMAND sh -c "${intltool_merge_BIN} --desktop-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop"
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
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications COMPONENT DTApplication)
endif(NOT WIN32)

if(${VALIDATE_APPDATA_FILE})
  add_custom_command(
    OUTPUT ${DARKTABLE_SHAREDIR}/metainfo/darktable.appdata.xml
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DARKTABLE_SHAREDIR}/metainfo
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in ${DARKTABLE_SHAREDIR}/metainfo/darktable.appdata.xml"
    COMMAND ${appstream_util_BIN} validate --nonet ${DARKTABLE_SHAREDIR}/metainfo/darktable.appdata.xml
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
else()
  add_custom_command(
    OUTPUT ${DARKTABLE_SHAREDIR}/metainfo/darktable.appdata.xml
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DARKTABLE_SHAREDIR}/metainfo
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in ${DARKTABLE_SHAREDIR}/metainfo/darktable.appdata.xml"
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/darktable.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
endif()

add_custom_target(darktable.appdata_file ALL DEPENDS ${DARKTABLE_SHAREDIR}/metainfo/darktable.appdata.xml)

install(FILES ${DARKTABLE_SHAREDIR}/metainfo/darktable.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo COMPONENT DTApplication)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
FILE(COPY ${WATERMARKS} DESTINATION "${DARKTABLE_DATADIR}/watermarks")
install(FILES ${WATERMARKS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/watermarks COMPONENT DTApplication)

#
# Install gdb command file for backtrace generation
#
FILE(COPY gdb_commands DESTINATION "${DARKTABLE_DATADIR}")
install(FILES gdb_commands DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

#
# Install noiseprofiles
#
if(${VALIDATE_JSON})
  add_custom_target(
    validate_noiseprofiles_json ALL
    COMMAND ${jsonschema_BIN} -i ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of noiseprofiles.json"
  )
endif()
FILE(COPY noiseprofiles.json DESTINATION "${DARKTABLE_DATADIR}")
install(FILES noiseprofiles.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

#
# Install wb presets
#
if(${VALIDATE_JSON})
  add_custom_target(
    validate_wbpresets_json ALL
    COMMAND ${jsonschema_BIN} -i ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.json ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.schema
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.json ${CMAKE_CURRENT_SOURCE_DIR}/wb_presets.schema
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of wb_presets.json"
  )
endif()
FILE(COPY wb_presets.json DESTINATION "${DARKTABLE_DATADIR}")
install(FILES wb_presets.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

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
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/darktableconfig.dtd ${DARKTABLE_DATADIR}/darktableconfig.dtd COPYONLY)

# get_property(DT_PLUGIN_IOPS GLOBAL PROPERTY DT_PLUGIN_IOPS)
get_property(DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT GLOBAL PROPERTY DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT)

set(DARKTABLECONFIG_IOP_ENTRIES "")
foreach(DT_PLUGIN_IOP IN LISTS DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT)
  set(DARKTABLECONFIG_IOP_ENTRIES "${DARKTABLECONFIG_IOP_ENTRIES}
  <dtconfig>
    <name>plugins/darkroom/${DT_PLUGIN_IOP}/visible</name>
    <type>bool</type>
    <default>true</default>
    <shortdescription/>
    <longdescription/>
  </dtconfig>")
endforeach()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/darktableconfig.xml.in ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml)

if(USE_XMLLINT)
  add_custom_target(
    validate_darktableconfig_xml ALL
    COMMAND ${Xmllint_BIN} --nonet --valid --noout ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml"
  )
endif(USE_XMLLINT)

if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
    COMMAND ${Xsltproc_BIN} --nonet ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
      COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(FATAL_ERROR "Don't know how to generate darktablerc")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(
  darktablerc_file ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_darktablerc.xsl ${DARKTABLE_DATADIR}/darktableconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/darktableconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
)
if(USE_XMLLINT)
  add_dependencies(darktablerc_file validate_darktableconfig_xml)
endif(USE_XMLLINT)

# first of all install darktablerc file into share as template
# postinst script should copy this into users homedirectory
# $HOME/.config/darktable/darktable.rc
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktablerc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

#
# Install bash completion file. users/packagers can symlink that to /etc/bash_completion.d/
#
install(FILES darktable.bash DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable COMPONENT DTApplication)

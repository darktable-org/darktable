cmake_minimum_required(VERSION 2.6)

add_subdirectory(pixmaps)

add_subdirectory(kernels)

#
# Generate and instal gtkrc
#
set(GTKRC_FONT_SIZE 8)
if(APPLE)
  # 8 is very tiny on mac
  set(GTKRC_FONT_SIZE 11)
endif(APPLE)
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/darktable.gtkrc.in ${CMAKE_CURRENT_BINARY_DIR}/darktable.gtkrc )


#
# Add files that should go into shared
#
set(SHARE_FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.gtkrc)
install(FILES ${SHARE_FILES} DESTINATION ${SHARE_INSTALL}/darktable)

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
install(FILES ${WEB_FILES} DESTINATION ${SHARE_INSTALL}/latex)

#
# Install other system shares
#
install(FILES darktable.desktop DESTINATION ./share/applications)

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

if(USE_XMLLINT)
	execute_process(
		COMMAND ${Xmllint_BIN} --valid ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../data/
		OUTPUT_QUIET ERROR_QUIET
		RESULT_VARIABLE Xmllint_RES
	)
	if(NOT ${Xmllint_RES} EQUAL 0 )
		message(FATAL_ERROR "data/darktableconfig.xml is not valid")
	endif(NOT ${Xmllint_RES} EQUAL 0 )
endif(USE_XMLLINT)

if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
    COMMAND ${Xsltproc_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.dtd ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
      COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/../data/darktableconfig.xml ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_darktablerc.xsl > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
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


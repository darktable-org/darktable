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
install(FILES ${SHARE_FILES} DESTINATION ${SHARE_INSTALL})

#
# web gallery export support files:
#
FILE(GLOB WEB_FILES "style/*")
install(FILES ${WEB_FILES} DESTINATION ${SHARE_INSTALL}/style)

FILE(GLOB WEB_FILES "js/*")
install(FILES ${WEB_FILES} DESTINATION ${SHARE_INSTALL}/js)

#
# Install other system shares
#
install(FILES darktable.desktop DESTINATION ./share/applications)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
install(FILES ${WATERMARKS} DESTINATION ${SHARE_INSTALL}/watermarks)

#
# Install gdb command file for backtrace generation
#
install(FILES gdb_commands DESTINATION ${SHARE_INSTALL})

#
# Setup some custom commands to generate a darktablerc template
# out of the darktable.schemas file.
 add_custom_command (
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_keys
    COMMAND 
        grep '<key>' ${CMAKE_CURRENT_SOURCE_DIR}/darktable.schemas | 
        sed -e 's/<key>//g' -e 's/<\\/key>//g' -e 's/\\/schemas\\/apps\\/darktable\\///g' | 
        nl -s: | 
        sed -e 's/^[ \\t]*//\;s/[ \\t]*$$//' > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_keys
  )
 
  add_custom_command (
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values
    COMMAND 
        grep '<default>' ${CMAKE_CURRENT_SOURCE_DIR}/darktable.schemas | 
        sed -e 's/<default>//g' -e 's/<\\/default>//g' | 
        nl -s: | 
        sed -e 's/^[ \\t]*//\;s/[ \\t]*$$//' > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values
        
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_keys
  )
 
  if(NOT APPLE)
    add_custom_command (
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
        COMMAND 
          join -1 1 -2 1 -o 1.2 2.2 ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_keys ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values | 
          sed -e 's/ /=/g' > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_in
          || (echo "*** error: please check if you have coreutils installed! we need grep, sed, nl, and join ***" &1>&2 | false)
          && grep plugins ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_in > /dev/null && mv ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_in ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
          # only move over if something is in there at all.
        
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values
    )
  else()
    add_custom_command (
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
        COMMAND 
          join -1 1 -2 1 -o '1.2 2.2' ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_keys ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values | 
          sed -e 's/ /=/g' > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_in
          || (echo "*** error: please check if you have coreutils installed! we need grep, sed, nl, and join ***" &1>&2 | false)
          && grep plugins ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_in > /dev/null && mv ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_in ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
          # only move over if something is in there at all.
        
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values
    )
  endif(NOT APPLE)
 
  add_custom_target(darktablerc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc)
 
  # first of all install darktablerc file into share as template
  # postinst script should copy this into users homedirectory 
  # $HOME/.config/darktable/darktable.rc
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktablerc DESTINATION ${SHARE_INSTALL}/)


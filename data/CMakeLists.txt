cmake_minimum_required(VERSION 2.6)

add_subdirectory(pixmaps)

add_subdirectory(kernels)

#
# install darktable faster wrapper
#
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/darktable-faster DESTINATION bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

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
set(WEB_FILES style/style.css style/favicon.ico)
install(FILES ${WEB_FILES} DESTINATION ${SHARE_INSTALL}/style/)

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
# Install gconf schema.
#

if(USE_GCONF_BACKEND)
  if(NOT DONT_INSTALL_GCONF_SCHEMAS)
    install (CODE "set(ENV{GCONF_CONFIG_SOURCE} ${DT_GCONF_SCHEMA_DIR})
        execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_QUIET COMMAND gconftool-2 --makefile-install-rule darktable.schemas)")
    install (CODE "message(\"-- Installing: gconf schemas to '${DT_GCONF_SCHEMA_DIR}'\")")
    install (CODE "execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_QUIET COMMAND killall -HUP gconfd-2)")
    install (CODE "message(\"-- Installing: restarting gconfd-2\")")
  else()
    install(FILES darktable.schemas DESTINATION /etc/gconf/schemas)
  endif(NOT DONT_INSTALL_GCONF_SCHEMAS)
else()
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
 
  add_custom_command (
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
    COMMAND 
      join -1 1 -2 1 -o 1.2 2.2 ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_keys ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values | 
      sed -e 's/ /=/g' > ${CMAKE_CURRENT_BINARY_DIR}/darktablerc
      
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc_values
  )
 
  add_custom_target(darktablerc ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/darktablerc)
 
  # first of all install darktablerc file into share as template
  # postinst script should copy this into users homedirectory 
  # $HOME/.config/darktable/darktable.rc
  install(FILES darktablerc DESTINATION ${SHARE_INSTALL}/)
  
endif()

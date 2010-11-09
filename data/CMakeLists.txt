cmake_minimum_required(VERSION 2.6)

add_subdirectory(pixmaps)

add_subdirectory(kernels)

#
# Generate and instal gtkrc
#
set(DTGTKRC_FONT_SIZE 8)
if(APPLE)
	# 8 is very tiny on mac
	set(GTKRC_FONT_SIZE 11)
endif(APPLE)
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/darktable.gtkrc.in ${SHARE_INSTALL}/darktable.gtkrc )

#
# Add files that should go into shared
#
set(SHARE_FILES darktable.glade)
install(FILES ${SHARE_FILES} DESTINATION ${SHARE_INSTALL})

#
# Install other system shares
#
install(FILES darktable.desktop DESTINATION ./share/applications)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
install(FILES ${WATERMARKS} DESTINATION ${SHARE_INSTALL}/watermarks)
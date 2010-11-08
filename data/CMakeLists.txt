cmake_minimum_required(VERSION 2.6)

add_subdirectory(pixmaps)

add_subdirectory(kernels)

#
# Add files that should go into shared
#
set(SHARE_FILES darktable.gtkrc darktable.glade)
install(FILES ${SHARE_FILES} DESTINATION ${SHARE_INSTALL})


#
# Install other system shares
#
install(FILES darktable.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
install(FILES ${WATERMARKS} DESTINATION ${SHARE_INSTALL}/watermarks)
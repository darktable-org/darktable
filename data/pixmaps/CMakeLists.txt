cmake_minimum_required(VERSION 2.6)

#
# Install plugin pixmaps
#
FILE(GLOB DARKROOM_PLUGIN_PIXMAPS "plugins/darkroom/*.png")
install(FILES ${DARKROOM_PLUGIN_PIXMAPS} DESTINATION ${SHARE_INSTALL}/pixmaps/plugins/darkroom )


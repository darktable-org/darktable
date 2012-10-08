cmake_minimum_required(VERSION 2.6)

set(COLORD_GTK_SOURCES
  "src/colord-gtk/cd-window.c"
  "src/colord-gtk/cd-window-sync.c"
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
add_definitions("-DCD_COMPILATION")

add_library(colord-gtk STATIC ${COLORD_GTK_SOURCES})
target_link_libraries(colord-gtk)


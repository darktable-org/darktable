cmake_minimum_required(VERSION 2.6)

# Build manual page
add_custom_command(
	OUTPUT darktable.1
	SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/darktable.pod
	COMMAND pod2man ARGS -s 1 
	${CMAKE_CURRENT_SOURCE_DIR}/darktable.pod 
	${CMAKE_CURRENT_BINARY_DIR}/darktable.1 
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/darktable.pod
	
)

add_custom_target(manpages ALL DEPENDS darktable.1)


install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.1 DESTINATION ${CMAKE_INSTALL_PREFIX}/share/man/man1)
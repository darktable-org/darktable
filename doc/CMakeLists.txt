cmake_minimum_required(VERSION 2.6)

#
# Install Doc files
#
set(DOC_FILES README AUTHORS TRANSLATORS LICENSE)
install(FILES ${DOC_FILES} DESTINATION ./share/doc/darktable)

# Build manual page
add_custom_command(
	OUTPUT darktable.1
	SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/darktable.pod
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tools/makeman.sh ${CMAKE_CURRENT_SOURCE_DIR}/darktable.pod ${CMAKE_CURRENT_BINARY_DIR}/../src/config.h ${CMAKE_CURRENT_SOURCE_DIR}/AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/darktable.1
	${CMAKE_CURRENT_SOURCE_DIR}/darktable.pod 
	${CMAKE_CURRENT_BINARY_DIR}/darktable.1 
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/darktable.pod
	
)

add_custom_target(manpages ALL DEPENDS darktable.1)


install(FILES ${CMAKE_CURRENT_BINARY_DIR}/darktable.1 DESTINATION ./share/man/man1)

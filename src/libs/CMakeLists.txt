cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")
set(MODULES export copy_history styles tagging image select collect recentcollect metadata metadata_view)

add_library(export MODULE "export.c")
add_library(copy_history MODULE "copy_history.c")
add_library(styles MODULE "styles.c")
add_library(image MODULE "image.c")
add_library(tagging MODULE "tagging.c")
add_library(select MODULE "select.c")
add_library(collect MODULE "collect.c")
add_library(recentcollect MODULE "recentcollect.c")
add_library(metadata MODULE "metadata.c")
add_library(metadata_view MODULE "metadata_view.c")

if(APRIL_FOOLS)
	if(VTE_FOUND)
		add_library(file_manager MODULE "file_manager.c")
		set(MODULES ${MODULES} file_manager)
		if(VTE_VERSION LESS "0.26.0")
			add_definitions(-DVTE_DEPRECATED=1)
			message("--- vte < 0.26 - using deprecated function")
		else(VTE_VERSION LESS "0.26.0")
			message("--- vte >= 0.26 - using recent function")
		endif(VTE_VERSION LESS "0.26.0")
	endif(VTE_FOUND)
endif(APRIL_FOOLS)

# check if gphoto2 found and add build of camera modules...
if(GPHOTO2_FOUND)
	add_library(capture MODULE "capture.c")
	add_library(camera MODULE "camera.c")
	set(MODULES ${MODULES} capture camera)
endif(GPHOTO2_FOUND)

# Add libs references
foreach(module ${MODULES})
	target_link_libraries(${module} ${LIBS})
endforeach(module)

# Add installation target for each module
foreach(module ${MODULES})
	install(TARGETS  ${module} DESTINATION ${LIB_INSTALL}/darktable/plugins/lighttable)
endforeach(module)


# - Try to find sys/queue.h include file

include(CheckIncludeFile)
include(CheckSymbolExists)

check_include_file(sys/queue.h SYSQUEUE_FOUND)
if (SYSQUEUE_FOUND)
	foreach (type "TAILQ")
		foreach (elem "ENTRY" "FIRST" "HEAD" "INIT" "INSERT_TAIL" "NEXT" "REMOVE")
			# Check for each symbol.
			check_symbol_exists(
					${type}_${elem}
					sys/queue.h
					SYSQUEUE_${type}_${elem}
			)

			if(NOT SYSQUEUE_${type}_${elem})
				message(FATAL_ERROR "ERROR: ${type}_${elem} is not defined in sys/queue.h !")
			endif()
		endforeach()
	endforeach()
else()
	message(FATAL_ERROR "ERROR: sys/queue.h include header is not found")
endif()

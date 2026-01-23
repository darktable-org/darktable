find_path(POTRACE_INCLUDE_DIR
    NAMES potracelib.h
    PATH_SUFFIXES include
)

find_library(POTRACE_LIBRARY
    NAMES potrace libpotrace
    PATH_SUFFIXES lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Potrace
    REQUIRED_VARS POTRACE_LIBRARY POTRACE_INCLUDE_DIR
)

if(Potrace_FOUND)
    add_library(Potrace::Potrace UNKNOWN IMPORTED)
    set_target_properties(Potrace::Potrace PROPERTIES
        IMPORTED_LOCATION ${POTRACE_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${POTRACE_INCLUDE_DIR}
    )
endif()

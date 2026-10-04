# Is the synthetic probe image actually adequate to replay harvested masks
# against? Measured per channel, not assumed -- see the file header.
add_cmocka_test(test_probe_image
                SOURCES test_probe_image.c
                LINK_LIBRARIES lib_darktable cmocka)
foreach(_t test_probe_image)
  target_include_directories(${_t} PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${CMAKE_SOURCE_DIR}/src)
  if(WIN32)
      _copy_required_library(${_t} lib_darktable)
  endif(WIN32)
endforeach()

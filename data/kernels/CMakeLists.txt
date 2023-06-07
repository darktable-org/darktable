#
# install opencl kernel source files
#
FILE(GLOB DT_OPENCL_KERNELS "*.cl")
FILE(GLOB DT_OPENCL_EXTRA "programs.conf" "common.h" "rgb_norms.h" "noise_generator.h" "color_conversion.h" "colorspace.h")

add_custom_target(testcompile_opencl_kernels ALL)

macro (testcompile_opencl_kernel IN)
  get_filename_component(KERNAME ${IN} NAME)

  set(TOUCH "${CMAKE_CURRENT_BINARY_DIR}/${KERNAME}.touch")

  set(KERNAME_OUT "opencl-program-${KERNAME}")

  add_custom_command(
    OUTPUT  ${TOUCH}
    COMMAND ${CLANG_OPENCL_COMPILER} -cc1 -cl-std=CL1.2 -isystem ${CLANG_OPENCL_INCLUDE_DIR} -finclude-default-header -I${CMAKE_CURRENT_SOURCE_DIR} ${IN}
    COMMAND ${CMAKE_COMMAND} -E touch ${TOUCH} # will be empty!
    DEPENDS ${IN}
    COMMENT "Test-compiling OpenCL program ${KERNAME}"
  )

  add_custom_target(
    ${KERNAME_OUT}
    DEPENDS ${TOUCH} # will be empty!
    DEPENDS ${IN}
  )

  add_dependencies(testcompile_opencl_kernels ${KERNAME_OUT})
endmacro (testcompile_opencl_kernel)

if (TESTBUILD_OPENCL_PROGRAMS)
  foreach(KERNEL IN ITEMS ${DT_OPENCL_KERNELS})
    testcompile_opencl_kernel(${KERNEL})
  endforeach()
endif()

install(FILES ${DT_OPENCL_KERNELS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/kernels COMPONENT DTApplication)
# Need to copy to have kernels all in one path like after installing
file(COPY ${DT_OPENCL_KERNELS} DESTINATION ${DARKTABLE_DATADIR}/kernels)

install(FILES ${DT_OPENCL_EXTRA} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/kernels COMPONENT DTApplication)
# Need to copy to have kernels all in one path like after installing
file(COPY ${DT_OPENCL_EXTRA} DESTINATION ${DARKTABLE_DATADIR}/kernels)

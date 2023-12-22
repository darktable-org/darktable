#
# compile macOS metal kernel source files
#

FILE(GLOB DT_METAL_KERNEL_SOURCES "*.metal")

set(DT_METALLIB "darktable.metallib")
set(DT_METALLIB_TOUCH "${DT_METALLIB}.touch")

set(KERNELS "")

add_custom_target(metal_kernels ALL)
add_custom_target(dt_metallib ALL)

macro (compile_metal_kernel IN)
  get_filename_component(KERNAME ${IN} NAME)
  get_filename_component(KERNAME_WLE ${IN} NAME_WLE)

  set(KERNAME_OUT "${KERNAME_WLE}.air")

  set(TOUCH "${CMAKE_CURRENT_BINARY_DIR}/${KERNAME}.touch")

  add_custom_command(
    OUTPUT  ${TOUCH}
    COMMAND ${CMAKE_COMMAND} -E touch ${TOUCH} # will be empty!
    COMMAND xcrun -sdk macosx metal -c ${IN} -o ${KERNAME_OUT}
    DEPENDS ${IN} 
    COMMENT "Compiling metal kernel ${IN}"
    VERBATIM
  )

  add_custom_target(
    ${KERNAME_OUT}
    DEPENDS ${TOUCH} # will be empty!
    DEPENDS ${IN}
  )

  add_dependencies(metal_kernels ${KERNAME_OUT})
  add_dependencies(dt_metallib ${KERNAME_OUT})

  list(APPEND KERNELS ${KERNAME_OUT})
endmacro (compile_metal_kernel)

foreach(KERNEL IN ITEMS ${DT_METAL_KERNEL_SOURCES})
  compile_metal_kernel(${KERNEL})
endforeach()

add_custom_command(
  OUTPUT ${DT_METALLIB_TOUCH}
  DEPENDS metal_kernels
  COMMAND xcrun -sdk macosx metallib -o ${DT_METALLIB} ${KERNELS}
  COMMENT "Building ${DT_METALLIB}"
  COMMAND ${CMAKE_COMMAND} -E touch ${DT_METALLIB_TOUCH}
)

add_custom_target(
  dt_metallib_touch ALL
  DEPENDS ${DT_METALLIB_TOUCH}
  DEPENDS metal_kernels
)

add_dependencies(dt_metallib dt_metallib_touch)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable
        FILES_MATCHING PATTERN "*.air" PATTERN "*.metallib"
        PATTERN "CMakeFiles" EXCLUDE
)

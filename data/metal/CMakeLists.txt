#
# compile macOS metal kernel source files
#

FILE(GLOB DT_METAL_KERNEL_SOURCES "*.metal")

set(DT_METALLIB "darktable.metallib")

add_custom_target(metal_kernels ALL)

macro (compile_metal_kernel IN)
  get_filename_component(KERNAME ${IN} NAME)
  get_filename_component(KERNAME_WLE ${IN} NAME_WLE)

  set(KERNAME_OUT "${KERNAME_WLE}.air")

  set(TOUCH "${CMAKE_CURRENT_BINARY_DIR}/${KERNAME}.touch")

  add_custom_command(
    OUTPUT  ${TOUCH} ${DT_METALLIB}.touch
    COMMAND ${CMAKE_COMMAND} -E touch ${TOUCH} # will be empty!
    COMMAND ${CMAKE_COMMAND} -E touch ${DT_METALLIB}.touch # will be empty!
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
endmacro (compile_metal_kernel)

foreach(KERNEL IN ITEMS ${DT_METAL_KERNEL_SOURCES})
  compile_metal_kernel(${KERNEL})
endforeach()

add_custom_target(
  ${DT_METALLIB} ALL
  DEPENDS ${DT_METALLIB}.touch
  COMMAND xcrun -sdk macosx metallib *.air -o ${DT_METALLIB} 
  COMMENT "Building ${DT_METALLIB}"
)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable
        FILES_MATCHING PATTERN "*.air" PATTERN "*.metallib"
        PATTERN "CMakeFiles" EXCLUDE
)

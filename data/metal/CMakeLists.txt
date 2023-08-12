#
# compile macOS metal kernel source files
#

FILE(GLOB DT_METAL_KERNELS "*.metal")

set(DARKTABLE_METALDIR "${DARKTABLE_BINDIR}/metal")
set(DARKTABLE_METALLIB "darktable.metallib")

add_custom_target(compile_metal_kernels ALL)

macro (compile_metal_kernel IN)
  get_filename_component(KERNAME ${IN} NAME)
  get_filename_component(KERNNAME_WLE ${IN} NAME_WLE)

  set(KERNAME_OUT "${KERNNAME_WLE}.air")

  add_custom_command(
    OUTPUT  ${DARKTABLE_METALDIR}/${KERNAME_OUT}
    COMMAND xcrun -sdk macosx metal -c ${IN} -o ${DARKTABLE_METALDIR}/${KERNAME_OUT}
    DEPENDS ${IN}
    COMMENT "Compiling metal kernel ${KERNAME}"
  )

  add_custom_target(
    ${KERNAME_OUT}
    DEPENDS ${DARKTABLE_METALDIR}/${KERNAME_OUT}
    DEPENDS ${IN}
  )

  add_dependencies(compile_metal_kernels ${KERNAME_OUT})
endmacro (compile_metal_kernel)

file(MAKE_DIRECTORY ${DARKTABLE_METALDIR})

foreach(KERNEL IN ITEMS ${DT_METAL_KERNELS})
  compile_metal_kernel(${KERNEL})
endforeach()

add_custom_command(
  OUTPUT  ${DARKTABLE_METALDIR}/${DARKTABLE_METALLIB}
  COMMAND xcrun -sdk macosx metallib ${DARKTABLE_METALDIR}/*.air -o ${DARKTABLE_METALDIR}/${DARKTABLE_METALLIB} 
  COMMENT "Building ${DARKTABLE_METALLIB}"
)

add_custom_target(
  run ALL
  DEPENDS ${DARKTABLE_METALDIR}/${DARKTABLE_METALLIB}
)

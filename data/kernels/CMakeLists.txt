#
# install opencl kernel source files
#
FILE(GLOB DT_OPENCL_KERNELS "*.cl" "programs.conf" "common.h")
install(FILES ${DT_OPENCL_KERNELS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/darktable/kernels)

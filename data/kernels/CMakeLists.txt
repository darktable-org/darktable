cmake_minimum_required(VERSION 2.6)

#
# install opencl kernel source files
#
FILE(GLOB DT_OPENCL_KERNELS "*.cl" "*.h" "programs.conf")
install(FILES ${DT_OPENCL_KERNELS} DESTINATION ${SHARE_INSTALL}/darktable/kernels)

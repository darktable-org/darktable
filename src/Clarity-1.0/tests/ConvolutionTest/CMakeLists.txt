ADD_EXECUTABLE(ConvolutionTest ConvolutionTest.cxx)

TARGET_LINK_LIBRARIES(ConvolutionTest
  Clarity
  ${FFTW_EXECUTABLE_LIBRARIES}
  ${FOUND_CUFFT}
)

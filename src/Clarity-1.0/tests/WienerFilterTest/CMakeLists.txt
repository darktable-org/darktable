ADD_EXECUTABLE(WienerFilterTest WienerFilterTest.cxx)

TARGET_LINK_LIBRARIES(WienerFilterTest
  Clarity
  ${FFTW_EXECUTABLE_LIBRARIES}
  ${FOUND_CUFFT}
)

ADD_EXECUTABLE(MaximumLikelihoodTest MaximumLikelihoodTest.cxx)

TARGET_LINK_LIBRARIES(MaximumLikelihoodTest
  Clarity
  ${FFTW_EXECUTABLE_LIBRARIES}
  ${FOUND_CUFFT}
)

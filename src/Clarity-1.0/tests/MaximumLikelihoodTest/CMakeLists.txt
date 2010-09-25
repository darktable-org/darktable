if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

ADD_EXECUTABLE(MaximumLikelihoodTest MaximumLikelihoodTest.cxx)

TARGET_LINK_LIBRARIES(MaximumLikelihoodTest
  Clarity
  ${FFTW_EXECUTABLE_LIBRARIES}
  ${FOUND_CUFFT}
)

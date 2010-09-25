if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

ADD_EXECUTABLE(JansenVanCittertTest JansenVanCittertTest.cxx)

TARGET_LINK_LIBRARIES(JansenVanCittertTest
  Clarity
  ${FFTW_EXECUTABLE_LIBRARIES}
  ${FOUND_CUFFT}
)

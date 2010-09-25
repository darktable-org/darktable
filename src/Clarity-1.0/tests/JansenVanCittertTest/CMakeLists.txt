ADD_EXECUTABLE(JansenVanCittertTest JansenVanCittertTest.cxx)

TARGET_LINK_LIBRARIES(JansenVanCittertTest
  Clarity
  ${FFTW_EXECUTABLE_LIBRARIES}
  ${FOUND_CUFFT}
)

# the codec can also be built alone for sanitizer tests and fuzzing
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.18)
  project(darktable_nikon_he LANGUAGES CXX)
  include(CTest)
endif()

add_library(darktable_nikon_he_codec STATIC
  nikon_he_bayer.cpp
  nikon_he_bit_reader.cpp
  nikon_he_coefficient_decode.cpp
  nikon_he_decode.cpp
  nikon_he_dequantize.cpp
  nikon_he_gcli_decode.cpp
  nikon_he_idwt_horizontal.cpp
  nikon_he_idwt_vertical.cpp
  nikon_he_picture_header.cpp
  nikon_he_precinct_decode.cpp
  nikon_he_precinct_header.cpp
  nikon_he_predecessor.cpp
  nikon_he_predict_lut.cpp
  nikon_he_subband_config.cpp
  nikon_he_tile.cpp)

target_compile_features(darktable_nikon_he_codec PUBLIC cxx_std_17)
target_include_directories(darktable_nikon_he_codec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set_target_properties(darktable_nikon_he_codec PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(TARGET rawspeed)
  add_library(darktable_nikon_he STATIC ../imageio_nikon_he.cc)
  target_link_libraries(darktable_nikon_he PUBLIC darktable_nikon_he_codec rawspeed)
  set_target_properties(darktable_nikon_he PROPERTIES POSITION_INDEPENDENT_CODE ON)

  install(FILES README.md LICENSE.LGPL LICENSE.CDDL
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/nikon_he COMPONENT DTDocuments)
elseif(BUILD_TESTING)
  add_executable(darktable-test-nikon-he ../../tests/nikon_he.cc)
  target_link_libraries(darktable-test-nikon-he PRIVATE darktable_nikon_he_codec)
  add_test(NAME imageio_nikon_he COMMAND darktable-test-nikon-he)
endif()

option(NIKON_HE_FUZZER "Build the Nikon HE libFuzzer target (Clang only)" OFF)
if(NIKON_HE_FUZZER)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NIKON_HE_FUZZER requires Clang")
  endif()
  target_compile_options(darktable_nikon_he_codec PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all)
  target_link_options(darktable_nikon_he_codec INTERFACE -fsanitize=address,undefined)
  add_executable(darktable-fuzz-nikon-he ../../tests/nikon_he_fuzzer.cc)
  target_compile_options(darktable-fuzz-nikon-he PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(darktable-fuzz-nikon-he PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(darktable-fuzz-nikon-he PRIVATE darktable_nikon_he_codec)
endif()

# MIT License
#
# Copyright (c) 2018 Kamil Lorenc
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

## Add unit test with mocking support
#  \param name unit test name (excluding extension and 'test_' prefix)
#  \param SOURCES optional list of source files to include in test executable
#  (beside test_${name}.c)
#  \param MOCKS optional list of functions to be mocked in executable
#  \param COMPILE_OPTIONS optional list of options for the compiler
#  \param LINK_LIBRARIES optional list of libraries to link (used as
#  -l${LINK_LIBRARIES})
#  \param LINK_OPTIONS optional list of options to be passed to linker
function(add_cmocka_mock_test name)
  # parse arguments passed to the function
  set(options )
  set(oneValueArgs )
  set(multiValueArgs SOURCES MOCKS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS)
  cmake_parse_arguments(ADD_MOCKED_TEST "${options}" "${oneValueArgs}"
    "${multiValueArgs}" ${ARGN} )

  # create link flags for mocks
  set(link_flags "")
  foreach (mock ${ADD_MOCKED_TEST_MOCKS})
    set(link_flags "${link_flags} -Wl,--wrap=${mock}")
  endforeach(mock)

  # define test
  add_cmocka_test(${name}
                  SOURCES ${ADD_MOCKED_TEST_SOURCES}
                  COMPILE_OPTIONS ${DEFAULT_C_COMPILE_FLAGS}
                                  ${ADD_MOCKED_TEST_COMPILE_OPTIONS}
                  LINK_LIBRARIES ${ADD_MOCKED_TEST_LINK_LIBRARIES}
                  LINK_OPTIONS ${link_flags} ${ADD_MOCKED_TEST_LINK_OPTIONS})

  # allow using includes from src/ directory
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endfunction(add_cmocka_mock_test)

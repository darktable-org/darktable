#  For more information, please see: http://software.sci.utah.edu
#
#  The MIT License
#
#  Copyright (c) 2007
#  Scientific Computing and Imaging Institute, University of Utah
#
#  License for the specific language governing rights and limitations under
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  the rights to use, copy, modify, merge, publish, distribute, sublicense,
#  and/or sell copies of the Software, and to permit persons to whom the
#  Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included
#  in all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
#  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
#  DEALINGS IN THE SOFTWARE.

# .cubin Parsing CMake Script
# Abe Stephens
# (c) 2007 Scientific Computing and Imaging Institute, University of Utah

FILE(READ ${input_file} file_text)

IF (${file_text} MATCHES ".+")

  # Remember, four backslashes is escaped to one backslash in the string.
  STRING(REGEX REPLACE ";" "\\\\;" file_text ${file_text})
  STRING(REGEX REPLACE "\ncode" ";code" file_text ${file_text})
  
  LIST(LENGTH file_text len)

  FOREACH(line ${file_text})

    # Only look at "code { }" blocks.
    IF(line MATCHES "^code")
      
      # Break into individual lines.
      STRING(REGEX REPLACE "\n" ";" line ${line})

      FOREACH(entry ${line})

        # Extract kernel names.
        IF (${entry} MATCHES "[^g]name = ([^ ]+)")
          STRING(REGEX REPLACE ".* = ([^ ]+)" "\\1" entry ${entry})

          # Check to see if the kernel name starts with "_"
          SET(skip FALSE)
          # IF (${entry} MATCHES "^_")
            # Skip the rest of this block.
            # MESSAGE("Skipping ${entry}")
            # SET(skip TRUE)
          # ELSE (${entry} MATCHES "^_")
            MESSAGE("Kernel:    ${entry}")  
          # ENDIF (${entry} MATCHES "^_")

        ENDIF(${entry} MATCHES "[^g]name = ([^ ]+)")

        # Skip the rest of the block if necessary
        IF(NOT skip)

          # Registers
          IF (${entry} MATCHES "reg = ([^ ]+)")
            STRING(REGEX REPLACE ".* = ([^ ]+)" "\\1" entry ${entry})
            MESSAGE("Registers: ${entry}")
          ENDIF(${entry} MATCHES "reg = ([^ ]+)")
          
          # Local memory
          IF (${entry} MATCHES "lmem = ([^ ]+)")
            STRING(REGEX REPLACE ".* = ([^ ]+)" "\\1" entry ${entry})
            MESSAGE("Local:     ${entry}")
          ENDIF(${entry} MATCHES "lmem = ([^ ]+)")
          
          # Shared memory
          IF (${entry} MATCHES "smem = ([^ ]+)")
            STRING(REGEX REPLACE ".* = ([^ ]+)" "\\1" entry ${entry})
            MESSAGE("Shared:    ${entry}")
          ENDIF(${entry} MATCHES "smem = ([^ ]+)")
                  
          IF (${entry} MATCHES "^}")
            MESSAGE("")
          ENDIF(${entry} MATCHES "^}")

        ENDIF(NOT skip)


      ENDFOREACH(entry)

    ENDIF(line MATCHES "^code")

  ENDFOREACH(line) 

ELSE(${depend_text} MATCHES ".+") 
  # MESSAGE("FOUND NO DEPENDS")
ENDIF(${depend_text} MATCHES ".+")



#
#   This file is part of darktable,
#   Copyright (C) 2022 darktable developers.
#
#   darktable is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   darktable is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
#

# Read an IOP file and guess how many buffers it needs from reading allocs
# This can be used to predict the RAM footprint of an IOP and setup tiling properly
# Example use :
#  `python tiling.py filmicrgb.c OpenCL` for OpenCL buffers
#  `python tiling.py filmicrgb.c C` for C buffers

import os
import re
import sys
directory = "../src/iop/"

path = os.path.join(directory, sys.argv[1])

if(not os.path.isfile(path)):
  print("%s is not a file" % path)
  exit(1)

lang = ""
if(len(sys.argv) > 2):
  lang = sys.argv[2]

f = open(path, "r")
content = f.read()
print(path)

excluded_keywords = [ "float", "size_t", "int", "uint", "uint8_t", "uint16_t", "cl_mem", "sizeof" ]

def parse_allocs(regex, content):
  matches = re.finditer(regex, content, re.MULTILINE)

  for matchNum, match in enumerate(matches):
    # Get the name of the pointer to the allocation
    variable_name = match.group(1)

    # Get the type of alloc function
    alloc_type = match.group(2)

    # Get the arguments of the function
    args = match.group(3)
    print("\t", match.group(0))

    # Extract all arguments
    args = args.split(",")

    for arg in args:
      # Try to find variables in arguments
      variables_mask = r"([a-zA-Z]+\[{0,1}[0-9A-Z]*\]{0,1}[\_\-\>]*[a-zA-Z0-9]*\[{0,1}[0-9A-Z]*\]{0,1})"
      variables = set(re.findall(variables_mask, arg))

      for var in variables:
        # Try to find the variables assignations/declarations
        if var not in excluded_keywords:
          print("\t\t-", var, ":")

          # Remove arrays if any
          var = re.sub(r"\[.*\]", "", var)
          declaration_mask = r"%s\[{0,1}[0-9A-Z]*\]{0,1} = .+;" % var
          declarations = set(re.findall(declaration_mask, content))

          # Print the declaration line of the variables if found
          if(len(declarations) > 0):
            for declaration in declarations:
              print("\t\t\t", declaration)
          else:
            print("\t\t\t no assignation found")

if(lang == "C"):
  print("C buffers allocated:")
  alloc_regex = r"([a-zA-Z0-9_\-\>\.\[\]]+) = (dt_|c|m)alloc.*\((.+)\)"
  parse_allocs(alloc_regex, content)

elif(lang == "OpenCL"):
  print("\nOpenCL buffers allocated:")
  alloc_regex = r"([a-zA-Z0-9_\-\>\.\[\]]+) = dt_opencl(.*)alloc[a-zA-Z_-]*\((.+)\)"
  parse_allocs(alloc_regex, content)

else:
  print("Option 2 `%s` not recognized" % lang)
  print("valid options are `C` or `OpenCL`")
  print("Example:")
  print("\t`python tiling.py filmicrgb.c OpenCL` for OpenCL buffers")
  print("\t`python tiling.py filmicrgb.c C` for C buffers")

  f.close()
  exit(1)

f.close()

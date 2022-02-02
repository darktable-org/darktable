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

# Read each IOP file, parse it for buffer allocs, and check if we have a corresponding free

import os
import re
directory = "../src/iop/"

alloc_regex = r"([a-zA-Z0-9_\-\>\.\[\]]+) = (dt_|c|m|dt_opencl_)alloc.*\(.+\)"


def find_call_and_line(elem, file, directory):
  line_number = 0
  f = open(os.path.join(directory, file), "r")

  # find the line(s) in file
  for line in f:
    line_number += 1
    if elem in line:
      print("\t\t line", line_number, ":", elem.strip())

  f.close()

for file in sorted(os.listdir(directory)):
  if file.endswith(".c") or file.endswith(".h"):
    f = open(os.path.join(directory, file), "r")
    content = f.read()

    # Find all allocs and extract the name of the pointer to the allocation
    matches = re.finditer(alloc_regex, content, re.MULTILINE)

    safe_allocs = 0
    faulty_allocs = 0
    suspicious_allocs = 0

    print("%s" % file)

    for matchNum, match in enumerate(matches):
      # Get the name of the pointer to the allocation
      variable_name = match.group(1)
      alloc_type = match.group(2)

      # Look how many times the variable is allocated
      variable_alloc_regex = " %s = %salloc.*\(.+\)" % (variable_name, alloc_type)
      matches2 = re.findall(variable_alloc_regex, content, re.MULTILINE)
      allocs = len(matches2)
      matches2 = set(matches2)

      # Look how many times the variable is freed
      variable_free_regex = r""
      buffer_type = ""
      if(alloc_type == "dt_opencl_"):
        variable_free_regex  = ".*release_mem_object.*\(%s\)" % variable_name
        buffer_type = "OpenCL"
      else:
        variable_free_regex  = " \S*free.*\(%s\)" % variable_name
        buffer_type = "C"
      matches3 = re.findall(variable_free_regex, content, re.MULTILINE)
      frees = len(matches3)
      matches3 = set(matches3)

      # Note that for OpenCL, we may have more than one free for each alloc because of the error go-to
      if(frees < allocs and frees == 0):
        print("\tERROR: %s buffer `%s` is allocated %i time(s) but never freed" % (buffer_type, variable_name, allocs))
        for elem in matches2:
          find_call_and_line(elem, file, directory)
        for elem in matches3:
          find_call_and_line(elem, file, directory)

        faulty_allocs += 1

      elif(frees < allocs and frees > 0):
        print("\tWARNING: %s buffer `%s` is allocated %i time(s) but freed %i time(s)" % (buffer_type, variable_name, allocs, frees))
        for elem in matches2:
          find_call_and_line(elem, file, directory)
        for elem in matches3:
          find_call_and_line(elem, file, directory)

        suspicious_allocs += 1

      else:
        safe_allocs += 1

    msg_type = "INFO"
    if(suspicious_allocs > 0):
      msg_type = "WARNING"
    if(faulty_allocs > 0):
      msg_type = "ERROR"

    print("\t%s: %i safe alloc(s) detected over %i\n" % (msg_type, safe_allocs, safe_allocs + faulty_allocs + suspicious_allocs))

    f.close()

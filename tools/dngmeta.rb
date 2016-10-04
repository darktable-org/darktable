#!/usr/bin/env ruby

# MIT License
#
# Copyright (c) 2016 Roman Lebedev
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

if ARGV.size < 1
  $stderr.puts "Usage: dngmeta.rb <file1> [file2] ..."
  exit 2
end

def is_iso_ok(exifhash, tag)
  if not exifhash[tag] or exifhash[tag] == "0" or exifhash[tag] == "65535"
    return false
  else
    return exifhash[tag]
  end
end

def get_iso(exifhash)
  isokeys = [
    "Exif.Photo.ISOSpeedRatings", "Exif.Photo.RecommendedExposureIndex",
    "Exif.Photo.StandardOutputSensitivity", "Exif.Nikon3.ISOSpeed",
    "Exif.Nikon3.ISOSettings", "Exif.NikonIi.ISO"
  ]

  iso = nil

  isokeys.each do |key|
    value = is_iso_ok(exifhash, key)
    return value if value
  end

  return iso
end

def get_exif(filename)
  exifhash = {}

  IO.popen("exiv2 -q -Qm -Pkt \"#{filename}\" 2> /dev/null") do |io|
    while !io.eof?
      lineparts = io.readline.split(" ", 2).map(&:strip)

      next if not lineparts.size == 2

      lineparts.each_slice(2) { |k,v| exifhash[k] = v }
   end
  end

  return exifhash
end

class Array
  def handle_data_dups
    iso = self.first
    return [iso, self.last.first] if self.last.size == 1

    whitelevels = self.last.map { |black, white| white }.uniq

    if whitelevels.size != 1
      $stderr.puts "ISO #{iso} has multiple variants with different white levels: #{self.last}"
      return [iso, [-1, -1]]
    end

    whitelevel = whitelevels[0]

    blacklevels = self.last.map { |black, white| black }.uniq

    if (blacklevels.max - blacklevels.min) > 4
      $stderr.puts "ISO #{iso} has multiple variants with too different black levels: #{self.last}"
      return [iso, [-1, -1]]
    end

    blacklevel = blacklevels.max

    return [iso, [blacklevel, whitelevel]]
  end

  def handle_different_black_levels
    white = self.first
    black = self.last.keys.max
    isos = self.last.values.flatten

    return isos, [black, white]
  end
end

def print_sensor(black, white, iso = false)
  if iso
    isolist = " iso_list=\"#{iso.join(" ")}\""
  else
    isolist = ""
  end

  return "\t\t<Sensor black=\"#{black}\" white=\"#{white}\"#{isolist}/>"
end

make = model = uniquecameramodel = nil
sensors = {}
ARGV.each do |filename|
  exifhash = get_exif(filename)

  if (make and make != exifhash["Exif.Image.Make"]) or
      # (model and model != exifhash["Exif.Image.Model"]) or
      (uniquecameramodel and uniquecameramodel != exifhash["Exif.Image.UniqueCameraModel"])
    $stderr.puts "WARNING: #{filename} - " \
                 "all files must be from the same camera maker and model!"
    next
  end

  make = exifhash["Exif.Image.Make"]
  model = exifhash["Exif.Image.Model"]
  uniquecameramodel = exifhash["Exif.Image.UniqueCameraModel"]

  iso = get_iso(exifhash).to_i
  white = exifhash["Exif.SubImage1.WhiteLevel"].to_i

  blacks = exifhash["Exif.SubImage1.BlackLevel"].split().map(&:strip)

  blacks = blacks.map(&:to_r)

  # want average black level, but rounded to next int
  black = blacks.reduce(:+).to_f / blacks.size
  black = black.ceil.to_i

  dsc = [black, white]

  sensors[iso] = [] if not sensors[iso]
  sensors[iso] << dsc
end

# want sorted by increasing ISO level
sensors = sensors.sort

# and only unique values
sensors.map! { |iso, dscs| [iso, dscs.uniq.sort] }

# sometimes there may be duplicate data still.
# if white levels match, and black levels are not too different, just take the biggest black level
sensors.map!(&:handle_data_dups)

invsensors = {}

if make == "Canon"
  isohash = {} # white => black => iso

  sensors.each do |iso, dsc|
    black = dsc.first
    white = dsc.last

    isohash[white] = {} if not isohash[white]
    isohash[white][black] = [] if not isohash[white][black]
    isohash[white][black] << iso
  end

  # group by white level, take the max black level.
  invsensors = isohash.map(&:handle_different_black_levels)

  tmp = {}
  invsensors.each do |iso, dsc|
    tmp[dsc] = [] if not tmp[dsc]
    tmp[dsc] << iso.flatten.sort
  end

  invsensors = tmp
else
  invsensors = {}
  sensors.each do |iso, dsc|
    invsensors[dsc] = [] if not invsensors[dsc]
    invsensors[dsc] << iso
  end
end

if true
  puts "invsensors #{invsensors}"
  puts
end

# so, which [black, white] is the most common ?
mostfrequent = invsensors.sort{ |x,y| y[1].flatten.size <=> x[1].flatten.size }.first
invsensors.delete(mostfrequent.first)

if true
  puts "sensors #{sensors}"
  puts "mostfrequent #{mostfrequent}"
  puts "invsensors #{invsensors}"
  puts
  # exit
end

mostfrequent = mostfrequent.first

puts "\t<Camera make=\"#{make}\" model=\"#{model}\">"
puts "\t\t<ID make=\"\" model=\"\">#{uniquecameramodel}</ID>"

puts print_sensor(mostfrequent.first, mostfrequent.last)
invsensors.each do |dsc, isos|
  puts print_sensor(dsc.first, dsc.last, isos)
end

puts "\t</Camera>"

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;

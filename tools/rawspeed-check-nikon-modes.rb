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

IGNORE_ONLY_14BIT = [
  ["NIKON CORPORATION", "NIKON D5100"],
  ["NIKON CORPORATION", "NIKON D5200"],
  ["NIKON CORPORATION", "COOLPIX A"]
]

IGNORE_ONLY_MODE = {
  ["NIKON CORPORATION", "NIKON 1 AW1"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 J1"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 J2"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 J3"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 J4"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 S1"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 S2"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 V1"] => "compressed",
  ["NIKON CORPORATION", "NIKON 1 V2"] => "compressed",
  ["NIKON CORPORATION", "NIKON D1"] => "uncompressed",
  ["NIKON CORPORATION", "NIKON D50"] => "compressed",
  ["NIKON CORPORATION", "NIKON D70"] => "compressed",
  ["NIKON CORPORATION", "NIKON D70s"] => "compressed",
  ["NIKON CORPORATION", "NIKON D600"] => "compressed",
  ["NIKON CORPORATION", "NIKON D610"] => "compressed",
  ["NIKON CORPORATION", "NIKON D750"] => "compressed",
  ["NIKON CORPORATION", "NIKON D3000"] => "compressed",
  ["NIKON CORPORATION", "NIKON D3100"] => "compressed",
  ["NIKON CORPORATION", "NIKON D3200"] => "compressed",
  ["NIKON CORPORATION", "NIKON D3400"] => "compressed",
  ["NIKON CORPORATION", "NIKON D5100"] => "compressed",
  ["NIKON CORPORATION", "NIKON D5200"] => "compressed",
  ["NIKON CORPORATION", "NIKON D7000"] => "compressed",
  ["NIKON CORPORATION", "NIKON D7100"] => "compressed",
  ["NIKON CORPORATION", "COOLPIX A"] => "compressed",
  ["NIKON", "COOLPIX P330"] => "compressed",
  ["NIKON", "COOLPIX P6000"] => "uncompressed",
  ["NIKON", "COOLPIX P7000"] => "uncompressed",
  ["NIKON", "COOLPIX P7100"] => "uncompressed",
  ["NIKON", "COOLPIX P7700"] => "compressed",
  ["NIKON", "COOLPIX P7800"] => "compressed",
  ["NIKON", "E5400"] => "uncompressed",
  ["NIKON", "E5700"] => "uncompressed"
}

IGNORE_HIGH_WHITELEVEL = [
  ["NIKON CORPORATION", "NIKON 1 AW1"],
  ["NIKON CORPORATION", "NIKON 1 J2"],
  ["NIKON CORPORATION", "NIKON 1 J3"],
  ["NIKON CORPORATION", "NIKON 1 S2"],
  ["NIKON CORPORATION", "NIKON 1 V2"],
  ["NIKON CORPORATION", "NIKON D1"],
  ["NIKON CORPORATION", "NIKON D1H"],
  ["NIKON CORPORATION", "NIKON D1X"],
  ["NIKON CORPORATION", "NIKON D50"],
  ["NIKON CORPORATION", "NIKON D70"],
  ["NIKON CORPORATION", "NIKON D70s"],
  ["NIKON CORPORATION", "NIKON D100"],
  ["NIKON", "COOLPIX P330"],
  ["NIKON", "COOLPIX P6000"],
  ["NIKON", "COOLPIX P7000"],
  ["NIKON", "COOLPIX P7700"],
  ["NIKON", "COOLPIX P7800"],
  ["NIKON", "E5400"],
  ["NIKON", "E5700"]
]

require 'nokogiri'

CAMERAS=File.expand_path("../src/external/rawspeed/data/cameras.xml", File.dirname(__FILE__))

cameras_hash = {}
File.open(CAMERAS) do |f|
  xml_doc  = Nokogiri::XML(f)
  xml_doc.css("Camera").each do |c|
    exif_maker = c.attribute("make").value
    exif_model = c.attribute("model").value
    cameraname = [exif_maker, exif_model]

    if not exif_maker.match(/nikon/i)
      next
    end

    if not c.attribute("mode")
      puts "Camera has no mode tag: #{exif_maker} #{exif_model}"
      next
    end

    mode = c.attribute("mode").value

    display_cameraname = [exif_maker, exif_model, mode]

    # ???
    if mode == "sNEF-uncompressed"
      next
    end

    if mode.scan(/-/).size != 1
      next
    end

    splitmode = mode.split('-')
    bitness = splitmode[0].strip
    compression = splitmode[1].strip

    if not bitness == "12bit" and not bitness == "14bit" and
      puts "Camera #{display_cameraname} has strange bitness part of mode tag: #{bitness}"
      next
    end

    bitness_num = bitness.to_i

    if not compression == "compressed" and not compression == "uncompressed" and
      puts "Camera #{display_cameraname} has strange compression part of mode tag: #{bitness}"
      next
    end

    if c.css("Sensor").size == 0
      puts "Camera #{display_cameraname} has zero sensors"
      next
    end

    if c.css("Sensor")[0]
      white = c.css("Sensor")[0].attribute("white").value.to_i
    end

    if not white
      puts "Camera #{display_cameraname} has no white level?"
    end

    if bitness.to_i != Math.log2(white.to_f).ceil.to_i and not IGNORE_HIGH_WHITELEVEL.include?(cameraname)
      puts "Camera #{display_cameraname} has wrong bitness tag: #{bitness} (#{bitness.to_i}, white is #{white})"
    end

    if white >= ((2**bitness_num)-1) and not IGNORE_HIGH_WHITELEVEL.include?(cameraname)
      puts "Camera #{display_cameraname} has too high white level: #{white} (>= ((2^#{bitness_num})-1), which is #{((2**bitness_num)-1)})"
      #next
    end

    whitemax = 0.8 * 2**(bitness_num)
    if white <= whitemax
      puts "Camera #{display_cameraname} has too low white level: #{white} (smaller than #{whitemax})"
      #next
    end

    cameras_hash = Hash.new if not cameras_hash
    cameras_hash[cameraname] = Hash.new if not cameras_hash.key?(cameraname)
    cameras_hash[cameraname][bitness] = Hash.new if not cameras_hash[cameraname].key?(bitness)
    cameras_hash[cameraname][bitness][compression] = Hash.new if not cameras_hash[cameraname][bitness].key?(compression)
    cameras_hash[cameraname][bitness][compression] = white
  end
end

puts

cameras_hash.each do |cameraname, modes|
  if modes.key?("14bit") and not modes.key?("12bit") and not IGNORE_ONLY_14BIT.include?(cameraname)
    puts "Camera #{cameraname} apparently has 14-bit sensor, but 12-bit mode is not defined."
  end

  if modes.key?("12bit") and IGNORE_ONLY_14BIT.include?(cameraname)
    puts "Camera #{cameraname} has no 12-bit modes, but 12-bit mode is defined."
  end

  modes.each do |mode, compressions|
    thecompression = compressions.keys[0]
    if compressions.length != 2 and not (IGNORE_ONLY_MODE.key?(cameraname) and IGNORE_ONLY_MODE[cameraname].include?(thecompression))
      puts "Camera #{cameraname} #{mode} has only #{thecompression} mode defined.", modes
    else
      if compressions.length == 2 and (compressions.values[0] != compressions.values[1])
        puts "Camera #{cameraname} #{mode} has different white levels between compression modes."
      end
    end

    compressions.each do |compression, whitelevel|
      if IGNORE_ONLY_MODE.key?(cameraname) and IGNORE_ONLY_MODE[cameraname] != compression
        puts "Camera #{cameraname} has no compression \"#{compression}\", but the mode is defined."
      end
    end
  end
end

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;

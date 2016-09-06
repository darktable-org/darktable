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

IGNORE_ONLY_14BIT = []

IGNORE_ONLY_MODE = {
  ["NIKON CORPORATION", "NIKON D600"] => "compressed",
  ["NIKON CORPORATION", "NIKON D3200"] => "compressed"
}

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
      puts "Camera \"#{exif_maker} #{exif_model}\" has strange bitness part of mode tag: #{bitness}"
      next
    end

    bitness_num = bitness.to_i

    if not compression == "compressed" and not compression == "uncompressed" and
      puts "Camera \"#{exif_maker} #{exif_model}\" has strange compression part of mode tag: #{bitness}"
      next
    end

    if c.css("Sensor")[0]
      white = c.css("Sensor")[0].attribute("white").value.to_i
    end

    if not white
      puts "Camera \"#{exif_maker} #{exif_model}\" \"#{mode}\" has no white level?"
    end

    if white >= ((2**bitness_num)-1)
      puts "Camera \"#{exif_maker} #{exif_model}\" \"#{mode}\" has too high white level: #{white} (bigger than ((2^#{bitness_num})-1), which is #{((2**bitness_num)-1)})"
      #next
    end

    if white <= 0.9 * 2**(bitness_num)
      puts "Camera \"#{exif_maker} #{exif_model}\" \"#{mode}\" has too low white level: #{white} (smaller than 0.9 * 2^(bitness_num), which is #{0.9 * 2**(bitness_num)})"
      #next
    end

    cameras_hash = Hash.new if not cameras_hash
    cameras_hash[cameraname] = Hash.new if not cameras_hash.key?(cameraname)
    cameras_hash[cameraname][bitness] = Hash.new if not cameras_hash[cameraname].key?(bitness)
    cameras_hash[cameraname][bitness][compression] = Hash.new if not cameras_hash[cameraname][bitness].key?(compression)
    cameras_hash[cameraname][bitness][compression] = white
  end
end

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

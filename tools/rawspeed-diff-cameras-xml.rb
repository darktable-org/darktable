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

require 'nokogiri'

CAMERAS_GOOD=File.expand_path("../src/external/rawspeed/data/cameras-good.xml", File.dirname(__FILE__))
CAMERAS=File.expand_path("../src/external/rawspeed/data/cameras.xml", File.dirname(__FILE__))

def parse_rs(cameras, hash)
  File.open(cameras) do |f|
    xml_doc  = Nokogiri::XML(f)
    xml_doc.css("Camera").each do |c|
      exif_maker = c.attribute("make").value
      exif_model = c.attribute("model").value
      cameraname = [exif_maker, exif_model]

      if c.attribute("supported") and c.attribute("supported").value == "no"
        next
      end

      if not exif_maker.match(/nikon/i)
        next
      end

      if not c.attribute("mode")
        mode = ""
      else
        mode = c.attribute("mode").value
      end

      if mode != "" and mode != "sNEF-uncompressed" and mode.scan(/-/).size == 1
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

        mode = [bitness_num, compression]
      end

      crop = []
      if c.css("Crop")[0]
        crop << c.css("Crop")[0].attribute("x").value
        crop << c.css("Crop")[0].attribute("y").value
        crop << c.css("Crop")[0].attribute("width").value
        crop << c.css("Crop")[0].attribute("height").value
      end

      hash = Hash.new if not hash
      hash[cameraname] = Hash.new if not hash.key?(cameraname)
      hash[cameraname][mode] = crop
    end
  end
end

def print_crop(crop)
  puts "\t\t<Crop x=\"#{crop[0]}\" y=\"#{crop[1]}\" width=\"#{crop[2]}\" height=\"#{crop[3]}\"/>"
end


old_hash = {}
parse_rs(CAMERAS_GOOD, old_hash)
# puts old_hash

# puts

new_hash = {}
parse_rs(CAMERAS, new_hash)
# puts new_hash

# puts
# puts

new_hash.each do |cameraname, modes|
  if not old_hash.key?(cameraname)
    puts "Camera #{cameraname} is newly-added?"
    next
  end

  modes.each do |mode, crop|
    old_crop = []

    if not old_hash[cameraname].key?(mode) and not old_hash[cameraname].key?("")
      puts "Camera #{cameraname} #{mode} - can not find any entries in old xml?"
      next
    end

    if old_hash[cameraname].key?(mode)
      old_crop = old_hash[cameraname][mode]
    else
      old_crop = old_hash[cameraname][""]
    end

    if old_crop != crop
       puts "Camera #{cameraname} #{mode} - crop differs. Should be #{old_crop}. Current: #{crop}"
       print_crop(old_crop)
       puts
    end
  end
end

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;

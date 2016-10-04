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

CAMERAS=File.expand_path("../src/external/rawspeed/data/cameras.xml", File.dirname(__FILE__))

File.open(CAMERAS) do |f|
  xml_doc  = Nokogiri::XML(f)
  xml_doc.css("Camera").each do |c|
    exif_maker = c.attribute("make").value
    exif_model = c.attribute("model").value
    mode = c.attribute("mode").value if c.attribute("mode")
    cameraname = [exif_maker, exif_model, mode]

    next if not exif_maker.match(/canon/i)

    next if exif_model.match(/PowerShot/i)

    next if c.attribute("mode")


    if c.css("Sensor").size == 1
      puts "Camera #{cameraname} has only one <Sensor> entry"
    end

    c.css("Sensor").each do |x|
      white = x.attribute("white").value.to_i

      bitness = Math.log2(white.to_f).ceil.to_i

      maxwhite = ((2**bitness)-1)
      if white >= maxwhite
        puts "Camera #{cameraname} has too high white level: " \
             "#{white} (>= ((2^#{bitness})-1), which is #{maxwhite})"
      end
    end
  end
end

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;

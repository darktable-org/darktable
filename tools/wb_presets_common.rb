#!/usr/bin/env ruby

module WbPresetsCommon
  def self.map_to_hash(hash, map)
    cameraname = [map[0], map[1]]
    hash = Hash.new if not hash
    hash[cameraname] = Hash.new if not hash.key?(cameraname)
    hash[cameraname][map[2]] = Hash.new if not hash[cameraname].key?(map[2])

    hash[cameraname][map[2]][map[3].to_i] = [map[4], map[5], map[6]]
  end

  def self.parse_preset(hash, line, upcase)
    if line[0..2] == "  {"
      lineparts = line.split('"')
      cameraname = ""
      if(upcase)
        cameraname = [lineparts[1].upcase, lineparts[3].upcase]
      else
        cameraname = [lineparts[1], lineparts[3]]
      end
      if cameraname.join.strip != ""

        # thank you, jhass
        p = line.delete('{}"').chomp(",").split(",").map(&:strip)

        p[0] = cameraname[0]
        p[1] = cameraname[1]

        # really don't care about kelvin presets.
        if p[2][-1].upcase == "K"
          return
        end

        if p[3].to_i.abs > 9
          puts ["tuning > 9 !", p]
          exit
        end

        if p[-1].to_f.abs != 0.0
          puts ["g2 != 0.0 !", p]
          exit
        end

        g = p[5].to_f
        r = p[4].to_f/g
        b = p[6].to_f/g
        g = 1

        p[4] = r
        p[5] = g
        p[6] = b

        map_to_hash(hash, p)
      end
    end
  end

  def self.output_presets(map)
    map.each do |key0, value0|
      value0.each do |key1, value1|
        value1.each do |key2, value2|
          puts "  { \"#{key0[0]}\", \"#{key0[1]}\", #{key1}, #{key2}, { #{value2[0]}, #{value2[1]}, #{value2[2]}, 0 } },"
        end
      end
    end
  end

  DTPRESETS=File.expand_path("../src/external/wb_presets.c", File.dirname(__FILE__))

  def self.dt_presets()
    presets = {}
    File.open(DTPRESETS) do |f|
      f.each do |line|
        parse_preset(presets, line, false)
      end
    end

    return presets
  end
end

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;

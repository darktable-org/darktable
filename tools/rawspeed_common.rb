#!/usr/bin/env ruby

module RawSpeedCommon
  require 'nokogiri'

  CAMERAS=File.expand_path("../src/external/rawspeed/data/cameras.xml", File.dirname(__FILE__))

  MANUAL_MUNGERS = {
    ["EASTMAN KODAK COMPANY", "KODAK EASYSHARE Z1015 IS DIGITAL CAMERA"] => ["KODAK", "EASYSHARE Z1015 IS"],
    ["KONICA MINOLTA", "ALPHA 5D"] => ["MINOLTA", "DYNAX 5D"],
    ["KONICA MINOLTA", "MAXXUM 5D"] => ["MINOLTA", "DYNAX 5D"],
    ["KONICA MINOLTA", "ALPHA 7D"] => ["MINOLTA", "DYNAX 7D"],
    ["KONICA MINOLTA", "MAXXUM 7D"] => ["MINOLTA", "DYNAX 7D"],
    ["Kodak", "DCS Pro SLR/n"] => ["KODAK", "DCS Pro SLR/n"],
    ["Leica Camera AG", "M8 Digital Camera"] => ["LEICA", "M8"],
  }

  def self.munge_make_model(make, model)
    makemodel = if MANUAL_MUNGERS[[make,model]]
      MANUAL_MUNGERS[[make,model]]
    elsif make.split[0] == model.split[0]
      [make.split[0], model[make.split[0].size..-1].strip]
    elsif model[0..6] == "FinePix"
      [make.split[0], model[7..-1].strip]
    elsif make.split[0..1].join(" ").upcase == "KONICA MINOLTA"
      [make.split[1], model.strip]
    elsif make.split[0].upcase == "RICOH" && model.split[0].upcase == "PENTAX"
      [model.split[0], model.split[1..-1].join.strip]
    elsif make.upcase == "KODAK"
      [make.upcase, model]
    else
      [make.split[0], model.strip]
    end
    return [makemodel[0].upcase, makemodel[1].upcase]
  end

  def self.generate_hashes()
    forward_hash = {} # From EXIF to clean name (1:1)
    backward_hash = {} # From clean name to EXIF (1:N)
    File.open(CAMERAS) do |f|
      xml_doc  = Nokogiri::XML(f)
      xml_doc.css("Camera").each do |c|
        clean_maker = exif_maker = c.attribute("make").value
        clean_model = exif_model = c.attribute("model").value
        if c.css("ID")[0]
          clean_maker = c.css("ID")[0].attribute("make").value
          clean_model = c.css("ID")[0].attribute("model").value
        end
        clean_id = [clean_maker, clean_model]
        exif_id = RawSpeedCommon.munge_make_model(exif_maker, exif_model)
        forward_hash[exif_id] = clean_id
        backward_hash[clean_id] ||= {}
        backward_hash[clean_id][exif_id] = true
        c.css("Alias").each do |a|
          exif_model = a.content
          exif_id = RawSpeedCommon.munge_make_model(exif_maker, exif_model)
          forward_hash[exif_id] = clean_id
          backward_hash[clean_id][exif_id] = true
        end
      end
    end

    return [forward_hash, backward_hash]
  end
end

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;

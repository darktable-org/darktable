require 'nokogiri'

class Runner
  EXT_IGNORES = ["xmp", "zip", "txt", "7z"]
  XMPS = [nil] # ["minimal"] # nil, minimal, standard_matrix
  WARN_LEVEL = 3
  DIFF_LEVEL = 5
  SAMPLEDIR = File.expand_path("../../data/samples/", File.dirname(__FILE__))
  CAMERAS = File.expand_path("../../../../src/external/rawspeed/data/cameras.xml", File.dirname(__FILE__))

  def initialize(opts={})
    @dir           = opts[:dir] || SAMPLEDIR
    @camera        = opts[:camera]
    @brand         = opts[:brand]
    @camera_limit  = opts[:camera_limit]

    # Create a map between EXIF and proper camera names
    @exif_alias_map = {}
    xml_doc  = Nokogiri::XML(File.open(CAMERAS))
    xml_doc.css("Camera").each do |c|
      maker = exif_maker = c.attribute("make").value
      model = exif_model = c.attribute("model").value
      exif_id = [exif_maker, exif_model]
      if c.css("ID")[0]
        maker = c.css("ID")[0].attribute("make").value
        model = c.css("ID")[0].attribute("model").value
      end
      @exif_alias_map[exif_id] = [maker,model]
      c.css("Alias").each do |a|
        exif_model = model = a.content
        exif_id = [exif_maker, exif_model]
        model = a.attribute("id").value if a.attribute("id")
        @exif_alias_map[exif_id] = [maker,model]
      end
    end
  end

  def get_makermodel(file)
    def get_exif_key(key, file)
      IO.popen("exiv2 -g \"#{key}\" -Pt \"#{file}\" 2>/dev/null","r").read
    end

    maker = get_exif_key("Exif.Image.Make", file)
    maker = maker[0..6] == "SAMSUNG" ? "SAMSUNG" : maker.strip
    model = get_exif_key("Exif.Image.Model", file)
    model = model[0..5] == "NX2000" ? "NX2000" : model.strip

    if (maker == "" || model == "") # Try with rawspeed instead
      IO.popen("darktable-rs-identify \"#{file}\"","r").each do |line|
        parts = line.split(":")
        case parts[0].strip
        when "make"
          maker = parts[1..-1].join(":").strip
        when "model"
          model = parts[1..-1].join(":").strip
        end
      end
    end

    if makermodel = @exif_alias_map[[maker, model]]
      return makermodel
    else
      $stderr.puts "Warning: Couldn't find make and model for '#{maker}' '#{model}'"
      maker = "Unknown" if !maker || maker == ''
      model = File.basename(file) if !model || model == ''
      return [maker, model]
    end
  end

  def run_images
    counts={}
    Dir["#{@dir}/**/*"].each do |file|
      if File.file?(file) &&
         file.split(".").size > 1 &&
         !(EXT_IGNORES.include?(file.split(".")[-1].downcase))
        brandname, cameraname = get_makermodel(file)
        if ((!@brand || brandname.downcase == @brand.downcase) &&
            (!@camera || cameraname.downcase == @camera.downcase))
          counts[brandname+cameraname] ||= 0
          if !@camera_limit || (counts[brandname+cameraname] += 1) < @camera_limit+1
            XMPS.each do |xmp|
              xmpfile = xmp ? File.expand_path("../../data/xmps/#{xmp}.xmp", File.dirname(__FILE__)) : nil
              yield file, xmpfile, xmp ? xmp : "none" , brandname, cameraname
            end
          end
        end
      end
    end
  end

  def mean_pixel_error(image1, image2)
    imgcmp = File.expand_path "../../bin/imgcmp", File.dirname(__FILE__)
    values = {}
    IO.popen("#{imgcmp} \"#{image1}\" \"#{image2}\"") do |io|
      while !io.eof?
        lineparts = io.readline.split(" ")
        if lineparts.size == 2
          values[lineparts[0]] = lineparts[1].to_f
        end
      end
    end
    values["mean_pixel_error"]
  end

  def calc_image_diff(image1, image2, diff)
    run_cmd_test_file "composite \"#{image1}\" \"#{image2}\" -compose difference \"#{diff}\"", diff
  end

  def constrast_stretch_image(image, output)
    run_cmd_test_file "convert \"#{image}\" -contrast-stretch 0%0% \"#{output}\"", output
  end

  def dcraw_export(dcraw, file, outfile)
    runsh "#{dcraw} -w -c '#{file}' | convert - #{outfile}"
  end

  def export(dtbuild, outputdir)
    run_images do |file, xmpfile, xmpname, brandname, cameraname|
      FileUtils.mkdir_p outputdir
      basename = File.basename file
      outfile = "#{outputdir}/#{brandname}/#{basename}.#{xmpname}.jpg"
      FileUtils.rm_f outfile
      ret, out = dtbuild.export(file, xmpfile, outfile)
      puts out
      #$stderr.puts ret ? "exported #{file}" : "failed #{file}"
      # generate JPG with dcraw for comparison
      #outfile = "#{outputdir}/#{brandname}/#{basename}.dcraw.jpg"
      #dcraw_export("dcraw", file, outfile)
    end
  end

  def compare(dtbuildfrom, dtbuildto, outputdir)
    run_images do |file, xmpfile, xmpname, brandname, cameraname|
      FileUtils.mkdir_p outputdir
      basename = File.basename file
      values = {}

      # Make sure to remove any "/" from names to avoid broken paths
      values["name"] = "#{brandname}-#{cameraname}-#{basename}-#{xmpname}".tr("/","-")
      values["brand"] = brandname
      values["camera"] = cameraname
      values["file"] = file
      values["xmp"] = xmpname

      outfile1 = "#{outputdir}/#{values["name"]}-1.jpg"
      values["file1"] = outfile1
      ret1, out1 = dtbuildfrom.export(file, xmpfile, outfile1)
      values["output1"] = out1
      values["version1"] = dtbuildfrom.version

      outfile2 = "#{outputdir}/#{values["name"]}-2.jpg"
      values["file2"] = outfile2
      ret2, out2 = dtbuildto.export(file, xmpfile, outfile2)
      values["output2"] = out2
      values["version2"] = dtbuildto.version

      if !ret1 && ret2
        # Awesome we now at least output something with the file
        puts "VERY GOOD new file working: #{values["name"]}"
        yield [:new_pass, values] if block_given?
      elsif ret1 && !ret2
        # Boo we now fail this file
        puts "VERY BAD  file is now failing: #{values["name"]}"
        yield [:new_fail, values] if block_given?
      elsif !ret1 && !ret2
        # Bummer we still don't know what to do with the file
        yield [:still_fail, values] if block_given?
        puts "BAD       file is still failing: #{values["name"]}"
      else
        pdiff = mean_pixel_error(outfile1, outfile2)
        if pdiff && pdiff < WARN_LEVEL
          puts "GOOD      file is still working: #{values["name"]}"
          yield [:still_pass, values] if block_given?
        else
          # We have caught a difference
          difffile = "#{outputdir}/#{values["name"]}-diff.jpg"
          values["difffile"] = difffile
          calc_image_diff outfile1, outfile2, difffile

          stretch_difffile = "#{outputdir}/#{values["name"]}-diff-stretch.jpg"
          values["stretch_difffile"] = stretch_difffile
          constrast_stretch_image difffile, stretch_difffile
          if pdiff && pdiff < DIFF_LEVEL
            puts "BAD       file is somewhat different: #{values["name"]}"
            yield [:warn, values] if block_given?
          else
            puts "VERY BAD  file is quite different: #{values["name"]}"
            yield [:diff, values] if block_given?
          end
        end
      end
    end
  end

  def compare_web(dtbuildfrom, dtbuildto, outputdir)
    FileUtils.rm_rf outputdir
    FileUtils.mkdir_p outputdir
    copy_web_basics(outputdir)
    brand_stats = {}
    brand_results = {}

    comparedir = outputdir+"/originals/"

    compare(dtbuildfrom, dtbuildto, comparedir) do |type, values|
      brand_stats[values["brand"]] ||= {}
      brand_stats[values["brand"]][type] ||= 0
      brand_stats[values["brand"]][type] += 1

      brand_results[values["brand"]] ||= {}
      brand_results[values["brand"]][type] ||= []
      brand_results[values["brand"]][type] << values

      values["type"] = type
      if type == :diff || type == :warn
        build_diffpage(outputdir, values)
      end

      FileUtils.rm_rf comparedir

      # Rebuild the index after each image so we get the partial output as it happens
      build_indexpage(outputdir, brand_stats, brand_results)
    end
  end
end

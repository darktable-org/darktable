def run_export(dir, outputdir, brand, camera, limit)
  run_images(dir, brand, camera, limit) do |file, xmpfile, xmpname, brandname, cameraname|
    FileUtils.mkdir_p outputdir
    basename = File.basename file
    outfile = "#{outputdir}/#{brandname}/#{basename}.#{xmpname}.jpg"
    FileUtils.rm_f outfile
    ret, out = dt_export("/opt/darktable/bin/darktable-cli", file, xmpfile, outfile)
    puts out
    #$stderr.puts ret ? "exported #{file}" : "failed #{file}"
    # generate JPG with dcraw for comparison
    #outfile = "#{outputdir}/#{brandname}/#{basename}.dcraw.jpg"
    #dcraw_export("dcraw", file, outfile)
  end
end

def get_dt(build)
  file = if build == "working_copy"
    "/opt/darktable/bin/darktable-cli"
  else
    File.expand_path "../../bin/builds/#{build}/inst/bin/darktable-cli", File.dirname(__FILE__)
  end
  $stderr.puts "No file for build #{build}" if !File.exists?(file)
  file
end

def get_dtversion(build)
  dir = if build == "working_copy"
    "/home/pedrocr/Projects/darktable"
  else
    File.expand_path "../../bin/builds/#{build}/src/", File.dirname(__FILE__)
  end
  $stderr.puts "No version for build #{build}" if !File.exists?(dir)
  build+" (at "+IO.popen("git -C #{dir} rev-parse HEAD").read+")"
end

WARN_LEVEL = 3
DIFF_LEVEL = 5

def run_compare(dir, outputdir, brand, camera, limit, from, to)
  FileUtils.mkdir_p outputdir
  run_images(dir, brand, camera, limit) do |file, xmpfile, xmpname, brandname, cameraname|
    basename = File.basename file
    values = {}

    values["name"] = "#{brandname}-#{cameraname}-#{basename}-#{xmpname}"
    values["brand"] = brandname
    values["camera"] = cameraname
    values["file"] = file
    values["xmp"] = xmpname

    outfile1 = "#{outputdir}/#{brandname}-#{cameraname}-#{basename}-#{xmpname}-1.jpg"
    values["file1"] = outfile1
    ret1, out1 = dt_export(get_dt(from), file, xmpfile, outfile1)
    values["output1"] = out1
    values["version1"] = get_dtversion(from)

    outfile2 = "#{outputdir}/#{brandname}-#{cameraname}-#{basename}-#{xmpname}-2.jpg"
    values["file2"] = outfile2
    ret2, out2 = dt_export(get_dt(to), file, xmpfile, outfile2)
    values["output2"] = out2
    values["version2"] = get_dtversion(to)

    if !ret1 && ret2
      # Awesome we now at least output something with the file
      puts "VERY GOOD new file working: #{values["name"]}"
      yield [:new_pass, values] if block_given?
    elsif ret1 && !ret2
      # Boo we now fail this file
      puts "VERY BAD  file is now failing: #{values["name"]}"
      yield [:new_fail, values] if block_given?
    elsif !ret1 && !ret2
      # Bummer we stil don't know what to do with the file
      yield [:still_fail, values] if block_given?
      puts "BAD       file is still failing: #{values["name"]}"
    else
      pdiff = mean_pixel_error(outfile1, outfile2)
      if pdiff && pdiff < WARN_LEVEL
        puts "GOOD      file is still working: #{values["name"]}"
        yield [:still_pass, values] if block_given?
      else
        # We have caught a difference
        difffile = "#{outputdir}/#{brandname}-#{cameraname}-#{basename}-#{xmpname}-diff.jpg"
        values["difffile"] = difffile
        calc_image_diff outfile1, outfile2, difffile

        stretch_difffile = "#{outputdir}/#{brandname}-#{cameraname}-#{basename}-#{xmpname}-diff-stretch.jpg"
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

def run_compare_web(dir, outputdir, brand, camera, limit, from, to)
  FileUtils.mkdir_p outputdir
  copy_web_basics(outputdir)
  brand_stats = {}
  brand_results = {}

  run_compare(dir, "compare_output", brand, camera, limit, from, to) do |type, values|
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

    # Rebuild the index after each image so we get the partial output as it happens
    build_indexpage(outputdir, brand_stats, brand_results)
  end
end

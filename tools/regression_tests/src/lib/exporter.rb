def dt_export(dt, file, xmpfile, outfile)
  xmpfile = xmpfile ? "'#{xmpfile}'" : "" # Escape file if it exists
  command =  "#{dt} '#{file}' #{xmpfile} '#{outfile}' --core"
#  command += " -d perf"
#  command += " -d camsupport"
  command += " --conf plugins/imageio/format/tiff/bpp=16"
#  command += " --conf plugins/imageio/format/jpeg/quality=100"
  command += " --conf write_sidecar_files=false"
  run_cmd_test_file command, outfile
end

def dcraw_export(dcraw, file, outfile)
  system "#{dcraw} -w -c '#{file}' | convert - #{outfile}"
end

def mean_pixel_error(image1, image2)
  imgcmp = File.expand_path "../../bin/imgcmp", File.dirname(__FILE__)
  values = {}
  IO.popen("#{imgcmp} #{image1} #{image2}") do |io|
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
  run_cmd_test_file "composite #{image1} #{image2} -compose difference #{diff}", diff
end

def constrast_stretch_image(image, output)
  run_cmd_test_file "convert #{image} -contrast-stretch 0%0% #{output}", output
end

XMPS = [nil] # ["minimal"] # nil, minimal, standard_matrix
IGNORES = ["xmp", "zip", "txt", "7z"]

def run_cmd_test_file(cmd, file)
  return [true, ""] if File.exists?(file)
  output = ""
  IO.popen(cmd+" 2>&1", "r") do |io|
    while !io.eof?
      output += io.readline
    end
  end
  [File.exists?(file), output]
end

def run_images(dir, brand, camera, camera_limit)
  camera ||= "*"
  brand ||= "*"
  counts={}
  Dir["#{dir}/#{brand}/#{camera}/*"].each do |file|
    if File.file?(file) &&
       file.split(".").size > 1 &&
       !(IGNORES.include?(file.split(".")[-1].downcase))
      brandname = file.split("/")[-3]
      cameraname = file.split("/")[-2]
      counts[brandname+cameraname] ||= 0
      if camera_limit && (counts[brandname+cameraname] += 1) < camera_limit+1  
        XMPS.each do |xmp|
          xmpfile = xmp ? File.expand_path("../../data/xmps/#{xmp}.xmp", File.dirname(__FILE__)) : nil
          yield file, xmpfile, xmp ? xmp : "none" , brandname, cameraname
        end
      end
    end
  end
end


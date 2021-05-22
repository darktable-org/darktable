require 'erb'
require 'fileutils'
require File.expand_path "./helpers.rb", File.dirname(__FILE__)

class ERBContext
  def initialize(hash)
    hash.each_pair do |key, value|
      instance_variable_set('@' + key.to_s, value)
    end
  end

  def get_binding
    binding
  end
end

def process_template(template, values)
  template = File.read (File.expand_path "./templates/#{template}", File.dirname(__FILE__))
  ERB.new(template).result(ERBContext.new(values).get_binding)
end

def reduce_image(outputdir, image)
  outfile = "#{outputdir}/#{File.basename(image)}"
  run_cmd_test_file "convert '#{image}' -geometry 1000x500 '#{outfile}'", outfile
end

def build_diffpage(outputdir, values)
  # First build the content
  values["content"] = process_template("testdiff.erb", values)
  # Now we build the page structure and put the content in the middle
  content = process_template("main.erb", values)
  # Now we finally output the file
  outfile = File.expand_path "diff-#{values["name"]}.html", outputdir
  File.open(outfile, 'w') {|f| f.write content}

  # Now we need to create the reduced size images
  ["file1","file2","difffile","stretch_difffile"].each do |f|
    reduce_image(outputdir, values[f])
  end
end

def build_indexpage(outputdir, brand_stats, brand_results)
  # First build the content
  values = {"brand_stats" => brand_stats, "brand_results" => brand_results}
  # FIXME: this can probably be done much cleaner by only calculating 
  #        version1 and version2 once and passing those along
  brand_results.each do |brand, results|
    results.each do |type, res|
      values["version1"] = res[0]["version1"]
      values["version2"] = res[0]["version2"]
      break
    end
    break
  end
  values["content"] = process_template("index.erb", values)
  # Now we build the page structure and put the content in the middle
  content = process_template("main.erb", values)
  # Now we finally output the file
  outfile = File.expand_path "index.html", outputdir
  File.open(outfile, 'w') {|f| f.write content}
end

def copy_web_basics(outputdir)
  ["theme.css", "bootstrap", "jquery.js"].each do |asset|
    file = File.expand_path "./assets/#{asset}", File.dirname(__FILE__)
    FileUtils.cp_r file, outputdir
  end
end

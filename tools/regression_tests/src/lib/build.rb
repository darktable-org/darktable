require 'fileutils'

def build_c_util(name)
  src = File.expand_path "../progs/#{name}.c", File.dirname(__FILE__)
  bin = File.expand_path "../../bin/#{name}", File.dirname(__FILE__)
  if !File.exists?(bin) || File.mtime(src) > File.mtime(bin)
    $stderr.puts "== Building '#{bin}'"
    runsh "libtool --quiet --mode=link gcc -lm $(pkg-config --libs --cflags GraphicsMagick) #{src} -o #{bin}"
  end
end

class DTBuild
  def initialize(build)
    @build = build
    @already_built = false
  end

  def run_build
    return if @already_built

    @repodir = File.expand_path "../../bin/repos/#{@build}/", File.dirname(__FILE__)

    FileUtils.rm_rf @repodir
    FileUtils.mkdir_p @repodir
    runsh "git clone ../../.git/ -b '#{@build}' #{@repodir} 2>/dev/null 1>/dev/null"
    
    @ref = IO.popen("git -C #{@repodir} rev-parse HEAD").read.strip

    @instdir = File.expand_path "../../bin/builds/#{@ref}/inst/", File.dirname(__FILE__)
    @bin = File.expand_path "./bin/darktable-cli", @instdir

    if !File.exists? @bin
      # We need to actually build this thing
      $stderr.puts "== Building '#{@build}'"
      FileUtils.rm_rf @instdir
      FileUtils.mkdir_p @instdir
      runsh "cd #{@repodir} && ./build.sh --prefix '#{@instdir}' --buildtype Release"
      runsh "cd #{@repodir}/build && make install"
    end
    @already_built = true
  end

  def export(file, xmpfile, outfile)
    run_build

    xmpfile = xmpfile ? "'#{xmpfile}'" : "" # Escape file if it exists
    command =  "#{@bin} '#{file}' #{xmpfile} '#{outfile}' --core"
  #  command += " -d perf"
  #  command += " -d camsupport"
    command += " --conf plugins/imageio/format/tiff/bpp=16"
  #  command += " --conf plugins/imageio/format/jpeg/quality=100"
    command += " --conf write_sidecar_files=false"
    run_cmd_test_file command, outfile
  end

  def version
    @build+" (at "+@ref+")"
  end
end

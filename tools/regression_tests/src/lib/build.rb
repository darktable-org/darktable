require 'fileutils'

def runsh(cmd)
  if system(cmd) == nil
    $stderr.puts "== Error running command '#{cmd}'"
    exit 1
  end
end

def build_c_util(name)
  src = File.expand_path "../progs/#{name}.c", File.dirname(__FILE__)
  bin = File.expand_path "../../bin/#{name}", File.dirname(__FILE__)
  if !File.exists?(bin) || File.mtime(src) > File.mtime(bin)
    $stderr.puts "== Building '#{bin}'"
    runsh "libtool --quiet --mode=link gcc -lm $(pkg-config --libs --cflags GraphicsMagick) #{src} -o #{bin}"
  end
end

class DTBuild
  DT_REPO="https://github.com/darktable-org/darktable.git"
  
  def initialize(build, opts={})
    @repo = opts[:repo] || DT_REPO
    @build = build
    @repodir = File.expand_path "../../bin/repos/#{@build}/", File.dirname(__FILE__)

    $stderr.puts "== Building '#{@build}' from '#{@repo}'"
    FileUtils.rm_rf @repodir
    FileUtils.mkdir_p @repodir
    runsh "git clone '#{DT_REPO}' --reference ../../ -b '#{@build}' #{@repodir}"
    
    @ref = @build

    @instdir = File.expand_path "../../bin/builds/#{@ref}/inst/", File.dirname(__FILE__)
    @bin = File.expand_path "./bin/darktable-cli", @instdir

    if !File.exists? @bin
      # We need to actually build this thing
      FileUtils.rm_rf @instdir
      FileUtils.mkdir_p @instdir
      runsh "cd #{@repodir} && ./build.sh --prefix '#{@instdir}' --buildtype Release"
      runsh "cd #{@repodir}/build && make install"
    end
  end
end

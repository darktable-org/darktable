def runsh(cmd)
  if system(cmd) == nil
    $stderr.puts "== Error running command '#{cmd}'"
    exit 1
  end
end

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

require File.expand_path "lib/common.rb", File.dirname(__FILE__)
require File.expand_path "lib/exporter.rb", File.dirname(__FILE__)
require File.expand_path "lib/runner.rb", File.dirname(__FILE__)
require File.expand_path "lib/build.rb", File.dirname(__FILE__)
require File.expand_path "lib/samples.rb", File.dirname(__FILE__)
require File.expand_path "web/builder.rb", File.dirname(__FILE__)

def prepare_builds(from_version, to_version)
  build_c_util("imgcmp")
  fetch_samples
end

require 'fileutils'

src = ARGV[0]
dst = ARGV[1]

$stderr.puts "Generating #{dst} from #{src}"

var_name = File.basename(dst, ".*").upcase
FileUtils.mkdir_p File.dirname dst
d = File.read src
File.write dst, <<-C
// Generated from #{src} by config.rb -- do not edit
static const unsigned char #{var_name}[] = {#{(d.unpack 'C*').join ','}, 0};
C

#!/usr/bin/env ruby
require "open-uri"
require "fileutils"
require "tmpdir"

VERSION  = "rel-4.0.4"
URL      = "https://github.com/arminbiere/kissat/archive/refs/tags/#{VERSION}.tar.gz"
DEST     = "build/kissat"
LIB      = "#{DEST}/build/libkissat.a"

if File.exist?(LIB)
  puts "kissat already built: #{LIB}"
  exit
end

FileUtils.mkdir_p("build")
FileUtils.rm_rf(DEST)

puts "downloading kissat #{VERSION} ..."
tarball = "build/kissat.tar.gz"
URI.open(URL) { |src| File.binwrite(tarball, src.read) }

system("tar", "xzf", tarball, "-C", "build", "--exclude=*/test/cnf/hard.cnf") or abort("tar failed")
File.delete(tarball)
File.rename("build/kissat-#{VERSION}", DEST)

puts "configuring kissat ..."
cc = ENV["CC"]
args = cc ? ["./configure", "CC=#{cc}"] : ["./configure"]
if RUBY_PLATFORM =~ /mingw|mswin|cygwin/
  args << "--quiet"
end
Dir.chdir(DEST) { system(*args) or abort("configure failed") }

puts "building kissat ..."
make_cmd = "make"
unless system("which", make_cmd, out: File::NULL, err: File::NULL)
  make_cmd = "mingw32-make"
  unless system("which", make_cmd, out: File::NULL, err: File::NULL)
    abort("make not found in PATH")
  end
end
Dir.chdir("#{DEST}/build") do
  unless system(make_cmd, "libkissat.a", out: $stdout, err: $stdout)
    abort("make failed")
  end
end
puts "done: #{LIB}"

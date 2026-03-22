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

system("tar", "xzf", tarball, "-C", "build") or abort("tar failed")
File.delete(tarball)
File.rename("build/kissat-#{VERSION}", DEST)

puts "configuring kissat ..."
cc = ENV["CC"]
args = cc ? ["./configure", "CC=#{cc}"] : ["./configure"]
Dir.chdir(DEST) { system(*args) or abort("configure failed") }

puts "building kissat ..."
system("make", "-C", "#{DEST}/build", "libkissat.a") or abort("make failed")
puts "done: #{LIB}"

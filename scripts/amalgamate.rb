#!/usr/bin/env ruby

require 'fileutils'

# scripts/amalgamate.rb — amalgamate C source files
# Usage: ruby scripts/amalgamate.rb [-i include_dir]... input1.c [input2.c ...] output.c
#
# Reads input files line by line, removes all #pragma once,
# and recursively inlines #include "xxx.h" from include dirs.

include_dirs = []
inputs = []
output = nil

i = 0
while i < ARGV.size
  if ARGV[i] == "-i" && i + 1 < ARGV.size
    include_dirs << ARGV[i + 1]
    i += 2
  else
    inputs << ARGV[i]
    i += 1
  end
end

output = inputs.pop

if inputs.empty? || output.nil?
  abort "usage: amalgamate.rb [-i dir]... input1 [input2 ...] output"
end

$included = {}
$include_dirs = include_dirs

def find_header(name)
  $include_dirs.each do |dir|
    path = File.join(dir, name)
    return path if File.exist?(path)
  end
  nil
end

def process_file(path)
  lines = []
  File.readlines(path).each do |line|
    case line
    when /^#pragma once/
      # don't emit, common_header_gen already emits one, and amalgamated impl doesn't need any
    when /^#include\s*</
      # system include, pass through
      lines << line
    when /^#include\s+"([^"]+)"/
      header_name = $1
      unless $included[header_name]
        header_path = find_header(header_name)
        if header_path
          $included[header_name] = true
          lines.concat(process_file(header_path))
        else
          # header not found in include dirs, keep as-is
          lines << line
        end
      end
      # if already included, skip
    else
      lines << line
    end
  end
  lines
end

result = []
inputs.each do |input|
  result.concat(process_file(input))
end

FileUtils.mkdir_p(File.dirname(output))
File.write(output, result.join)
puts "Amalgamation successful: #{output}"

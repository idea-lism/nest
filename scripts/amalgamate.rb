#!/usr/bin/env ruby

require 'open3'

TOOL = "build/tools/amalgamate"

stdout, stderr, status = Open3.capture3(TOOL, *ARGV)

stderr.each_line do |line|
  $stderr.write(line) unless line.include?("JUCE Assertion failure") || line.start_with?("JUCE v")
end
$stdout.write(stdout) unless stdout.empty?

exit(status.exitstatus) unless status.success?

output_file = ARGV.last
if File.exist? output_file
  puts "Amalgamation successful: #{output_file}"
else
  abort "Amalgamation failed: output file not found"
end

met = false
res = []
(File.readlines output_file).each do |line|
  case line
  when /^#pragma once/
    if not met
      res << line
      met = true
    end
  else
    res << line
  end
end
File.write output_file, res.join

# frozen_string_literal: true

# benchmark/summary.rb — print markdown summary tables from benchmark CSVs.
# Usage: ruby benchmark/summary.rb

require "csv"

RESULTS_DIR = File.join(__dir__, "results")

def read_csv(name)
  path = File.join(RESULTS_DIR, name)
  return [] unless File.exist?(path)

  CSV.read(path, headers: true).map(&:to_h)
end

def fmt_us(v)
  v = v.to_f
  v >= 1000 ? "#{v.round(0).to_s.gsub(/(\d)(?=(\d{3})+$)/, '\1,')}" : v.round(1).to_s
end

def fmt_mbs(v)
  v.to_f.round(1).to_s
end

def print_internal_tables
  %w[calc json kotlin].each do |grammar|
    rows = read_csv("internal_#{grammar}.csv")
    next if rows.empty?

    # Filter to -O2 and 1MB inputs only
    rows = rows.select { |r| r["opt"] == "-O2" && r["input"].start_with?("1m_") }
    next if rows.empty?

    puts "### #{grammar} — 1MB, -O2\n\n"
    puts "| Mode | Input | Parse µs | MB/s | Tokens |"
    puts "|------|-------|----------|------|--------|"
    rows.sort_by { |r| [r["memoize"], r["input"]] }.each do |r|
      puts "| #{r['memoize']} | #{r['input']} | #{fmt_us(r['parse_us'])} | #{fmt_mbs(r['throughput_mbs'])} | #{r['token_count']} |"
    end
    puts
  end
end

def print_compare_table
  rows = read_csv("compare.csv")
  return if rows.empty?

  by_grammar = rows.group_by { |r| r["grammar"] }
  by_grammar.each do |grammar, grammar_rows|
    puts "### #{grammar} — cross-framework\n\n"
    puts "| Framework | Parse µs | MB/s | Compile ms | Build ms | Binary KB |"
    puts "|-----------|----------|------|------------|----------|-----------|"
    grammar_rows.each do |r|
      bin_kb = r["binary_bytes"].to_i / 1024
      puts "| #{r['framework']} | #{fmt_us(r['parse_us'])} | #{fmt_mbs(r['throughput_mbs'])} | #{r['compile_ms']} | #{r['build_ms']} | #{bin_kb} |"
    end
    puts
  end
end

puts "# Benchmark Summary\n\n"
puts "## Internal (nest-only, memoize comparison)\n\n"
print_internal_tables
puts "## Comparison (cross-framework)\n\n"
print_compare_table

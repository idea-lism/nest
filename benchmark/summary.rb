# frozen_string_literal: true

# benchmark/summary.rb — print markdown summary tables from benchmark CSVs.
# Usage: ruby benchmark/summary.rb

require "csv"

RESULTS_DIR = File.join(__dir__, "results")
SIZE_LABELS = %w[1k 10k 1m].freeze
MEMOIZE_MODES = %w[none naive shared].freeze
GRAMMARS = %w[calc json kotlin].freeze
METRICS = ["MB/s", "RSS MB", "Tokens", "Chunks"].freeze

def read_csv(name)
  path = File.join(RESULTS_DIR, name)
  return [] unless File.exist?(path)

  CSV.read(path, headers: true).map(&:to_h)
end

def fmt_us(v)
  v = v.to_f
  v >= 1000 ? "#{v.round(0).to_s.gsub(/(\d)(?=(\d{3})+$)/, '\1')}" : v.round(1).to_s
end

def fmt_mbs(v)
  v.to_f.round(1).to_s
end

def fmt_rss_mb(v)
  format("%.1f", v.to_f / 1024.0)
end

def preferred_input_name(rows, size_label)
  %w[mixed base flat deep wide].each do |variant|
    name = "#{size_label}_#{variant}"
    row = rows.find { |r| r["input"] == name }
    return row["input"] if row
  end

  rows.find { |r| r["input"].start_with?("#{size_label}_") }&.fetch("input", nil)
end

def find_row(rows, input_name, memoize, opt)
  return nil unless input_name

  rows.find { |r| r["input"] == input_name && r["memoize"] == memoize && r["opt"] == opt }
end

def metric_cells(row)
  return ["-", "-", "-", "-"] unless row

  [
    fmt_mbs(row["throughput_mbs"]),
    fmt_rss_mb(row["rss_kb"]),
    row["token_count"],
    row["chunk_count"]
  ]
end

def print_md_table(headers, rows)
  puts "| #{headers.join(' | ')} |"
  puts "| #{headers.map { |h| '-' * [h.length, 3].max }.join(' | ')} |"
  rows.each do |row|
    puts "| #{row.join(' | ')} |"
  end
  puts
end

def print_calc_tables(grammar_rows)
  calc_rows = grammar_rows.fetch("calc")

  SIZE_LABELS.each do |size_label|
    input_name = preferred_input_name(calc_rows, size_label)
    next unless input_name

    puts "### calc — #{input_name}\n\n"
    headers = ["Mode"] + ["-O0", "-O2"].flat_map { |opt| METRICS.map { |metric| "#{opt} #{metric}" } }
    rows = MEMOIZE_MODES.map do |memoize|
      [memoize] +
        metric_cells(find_row(calc_rows, input_name, memoize, "-O0")) +
        metric_cells(find_row(calc_rows, input_name, memoize, "-O2"))
    end
    print_md_table(headers, rows)
  end
end

def print_all_grammar_tables(grammar_rows)
  SIZE_LABELS.each do |size_label|
    inputs = GRAMMARS.each_with_object({}) do |grammar, acc|
      input_name = preferred_input_name(grammar_rows.fetch(grammar), size_label)
      acc[grammar] = input_name if input_name
    end
    next if inputs.empty?

    puts "### all grammars — #{size_label}, -O2\n\n"
    puts GRAMMARS.map { |grammar| "#{grammar}=`#{inputs[grammar] || '-'}`" }.join(", ")
    puts
    puts

    headers = ["Mode"] + GRAMMARS.flat_map { |grammar| METRICS.map { |metric| "#{grammar} #{metric}" } }
    rows = MEMOIZE_MODES.map do |memoize|
      [memoize] + GRAMMARS.flat_map do |grammar|
        metric_cells(find_row(grammar_rows.fetch(grammar), inputs[grammar], memoize, "-O2"))
      end
    end
    print_md_table(headers, rows)
  end
end

def print_internal_tables
  grammar_rows = GRAMMARS.each_with_object({}) do |grammar, acc|
    acc[grammar] = read_csv("internal_#{grammar}.csv")
  end

  print_calc_tables(grammar_rows)
  print_all_grammar_tables(grammar_rows)
end

def print_compare_table
  rows = read_csv("compare.csv")
  return if rows.empty?

  by_grammar = rows.group_by { |r| r["grammar"] }
  by_grammar.each do |grammar, grammar_rows|
    puts "### #{grammar} — cross-framework\n\n"
    headers = ["Framework", "Parse µs", "MB/s", "Compile ms", "Build ms", "Binary KB"]
    table_rows = grammar_rows.map do |r|
      [
        r["framework"],
        fmt_us(r["parse_us"]),
        fmt_mbs(r["throughput_mbs"]),
        r["compile_ms"],
        r["build_ms"],
        (r["binary_bytes"].to_i / 1024).to_s
      ]
    end
    print_md_table(headers, table_rows)
  end
end

puts "# Benchmark Summary\n\n"
puts "## Internal (nest-only, memoize comparison)\n\n"
print_internal_tables
puts "## Comparison (cross-framework)\n\n"
print_compare_table

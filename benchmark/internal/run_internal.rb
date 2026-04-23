# frozen_string_literal: true

# benchmark/internal/run_internal.rb
# Nest-only internal benchmarks: memoize × clang-opt matrix.
# calc runs -O0/-O2. json/kotlin run -O2 only.
# Output: benchmark/results/internal_<grammar>.csv + markdown report.

require_relative "../common"
include BenchmarkCommon

CC = "xcrun clang"
MEMOIZE_MODES = %w[none naive shared].freeze
SIZE_LABELS = %w[1k 10k 1m].freeze
REPORT_METRICS = ["MB/s", "RSS MB", "Memo MB", "Tokens", "Chunks"].freeze
OPT_LEVELS = {
  "calc" => %w[-O0 -O2],
  "json" => %w[-O2],
  "kotlin" => %w[-O2]
}.freeze

def build_nest_runner(grammar, prefix, memoize, opt, build_dir)
  grammar_info = GRAMMARS.fetch(grammar)
  nest_grammar = grammar_info[:nest_grammar]
  runner_erb = File.join(RUNNERS_DIR, "nest_runner.c.erb")

  ensure_dirs(build_dir)

  # nest c → prefix.ll + prefix.h
  sh(NEST_BIN, "c", nest_grammar, "-p", prefix, "-m", memoize, chdir: build_dir)

  # Render runner C from ERB
  runner_c = File.join(build_dir, "runner.c")
  rendered = ERB.new(File.read(runner_erb), trim_mode: "-").result_with_hash(prefix: prefix)
  File.write(runner_c, rendered)

  # Compile
  runner_bin = File.join(build_dir, "runner")
  sh("#{CC} #{opt} -std=c23 runner.c #{prefix}.ll -o runner", chdir: build_dir)
  runner_bin
end

def collect_inputs(grammar)
  info = GRAMMARS.fetch(grammar)
  dir = info[:internal_dir]
  return [] unless Dir.exist?(dir)

  Dir.glob(File.join(dir, "*")).sort.map do |path|
    { path: path, name: File.basename(path), size_bytes: File.size(path) }
  end
end

def run_internal
  ensure_dirs(BUILD_DIR, RESULTS_DIR)

  GRAMMARS.each_key do |grammar|
    inputs = collect_inputs(grammar)
    if inputs.empty?
      puts "SKIP #{grammar}: no internal inputs (run gen first)"
      next
    end

    csv_path = File.join(RESULTS_DIR, "internal_#{grammar}.csv")
    rows = []

    MEMOIZE_MODES.each do |memoize|
      OPT_LEVELS.fetch(grammar).each do |opt|
        opt_label = opt.delete("-")
        prefix = "bench_#{grammar}"
        build_dir = File.join(BUILD_DIR, "internal", grammar, "#{memoize}_#{opt_label}")

        puts "==> #{grammar} memoize=#{memoize} opt=#{opt}"
        begin
          runner_bin = build_nest_runner(grammar, prefix, memoize, opt, build_dir)
        rescue StandardError => e
          $stderr.puts "  BUILD FAILED: #{e.message}"
          next
        end

        inputs.each do |input|
          begin
            iters = adaptive_iterations(runner_bin, input[:path])
            out, rss_kb, err = run_with_rss([runner_bin, input[:path], iters.to_s])
            csv_line = out.strip.lines.last&.strip || ""
            fields = csv_line.split(",")
            parse_us = fields[0].to_f
            token_count = fields[2].to_i
            chunk_count = fields[3].to_i
            throughput = input[:size_bytes] / (parse_us / 1e6) / 1e6
            memoize_bytes = parse_trace_total_malloc(err)

            row = {
              "grammar"       => grammar,
              "input"         => input[:name],
              "size_bytes"    => input[:size_bytes],
              "memoize"       => memoize,
              "opt"           => opt,
              "parse_us"      => parse_us.round(1),
              "throughput_mbs" => throughput.round(2),
              "rss_kb"        => rss_kb ? rss_kb.round(1) : "",
              "memoize_bytes" => memoize_bytes || "",
              "token_count"   => token_count,
              "chunk_count"   => chunk_count
            }
            rows << row
            puts "  #{input[:name]}: #{parse_us.round(1)} µs, #{throughput.round(1)} MB/s"
          rescue StandardError => e
            $stderr.puts "  FAIL #{input[:name]}: #{e.message}"
          end
        end
      end
    end

    # Write CSV
    if rows.any?
      headers = rows.first.keys
      File.open(csv_path, "w") do |f|
        f.puts(headers.join(","))
        rows.each { |r| f.puts(headers.map { |h| r[h] }.join(",")) }
      end
      puts "  wrote #{csv_path}"
    end
  end
end

def preferred_input_name(rows, size_label)
  %w[mixed base flat deep wide].each do |variant|
    name = "#{size_label}_#{variant}"
    return name if rows.any? { |r| r["input"] == name }
  end

  rows.find { |r| r["input"].start_with?("#{size_label}_") }&.fetch("input", nil)
end

def find_row(rows, input_name, memoize, opt)
  return nil unless input_name

  rows.find { |r| r["input"] == input_name && r["memoize"] == memoize && r["opt"] == opt }
end

def fmt_metric(v, digits = 1)
  return "-" if v.nil? || v == ""

  format("%.#{digits}f", v.to_f)
end

def fmt_rss_mb(v)
  return "-" if v.nil? || v == ""

  format("%.1f", v.to_f / 1024.0)
end

def fmt_bytes_mb(v)
  return "-" if v.nil? || v == ""

  format("%.1f", v.to_f / 1024.0 / 1024.0)
end

def metric_cells(row)
  return ["-", "-", "-", "-", "-"] unless row

  [
    fmt_metric(row["throughput_mbs"]),
    fmt_rss_mb(row["rss_kb"]),
    fmt_bytes_mb(row["memoize_bytes"]),
    row["token_count"].to_i.to_s,
    row["chunk_count"].to_i.to_s
  ]
end

def md_table_line(cells)
  "| #{cells.join(' | ')} |\n"
end

def md_table_sep(headers)
  md_table_line(headers.map { |header| "-" * [header.length, 3].max })
end

def append_md_table(lines, headers, rows)
  lines << md_table_line(headers)
  lines << md_table_sep(headers)
  rows.each do |row|
    lines << md_table_line(row)
  end
  lines << "\n"
end

def report_calc_tables(lines, grammar_rows)
  calc_rows = grammar_rows.fetch("calc")

  SIZE_LABELS.each do |size_label|
    input_name = preferred_input_name(calc_rows, size_label)
    next unless input_name

    lines << "## calc — #{input_name}\n\n"
    headers = ["Memoize"] + ["-O0", "-O2"].flat_map { |opt| REPORT_METRICS.map { |metric| "#{opt} #{metric}" } }
    rows = MEMOIZE_MODES.map do |memoize|
      [memoize] +
        metric_cells(find_row(calc_rows, input_name, memoize, "-O0")) +
        metric_cells(find_row(calc_rows, input_name, memoize, "-O2"))
    end
    append_md_table(lines, headers, rows)
  end
end

def report_all_grammar_tables(lines, grammar_rows)
  SIZE_LABELS.each do |size_label|
    inputs = GRAMMARS.each_key.each_with_object({}) do |grammar, acc|
      input_name = preferred_input_name(grammar_rows.fetch(grammar), size_label)
      acc[grammar] = input_name if input_name
    end
    next if inputs.empty?

    lines << "## all grammars — #{size_label}, -O2\n\n"
    lines << "Inputs: #{GRAMMARS.each_key.map { |grammar| "#{grammar}=`#{inputs[grammar] || '-'}'" }.join(', ')}\n\n"

    headers = ["Memoize"] + GRAMMARS.each_key.flat_map { |grammar| REPORT_METRICS.map { |metric| "#{grammar} #{metric}" } }
    rows = MEMOIZE_MODES.map do |memoize|
      [memoize] + GRAMMARS.each_key.flat_map do |grammar|
        metric_cells(find_row(grammar_rows.fetch(grammar), inputs[grammar], memoize, "-O2"))
      end
    end
    append_md_table(lines, headers, rows)
  end
end

def report_internal
  md_path = File.join(RESULTS_DIR, "internal_report.md")
  lines = ["# Internal Benchmark Report\n\n"]
  grammar_rows = GRAMMARS.each_with_object({}) do |(grammar, _info), acc|
    acc[grammar] = csv_rows(File.join(RESULTS_DIR, "internal_#{grammar}.csv"))
  end

  report_calc_tables(lines, grammar_rows)
  report_all_grammar_tables(lines, grammar_rows)

  File.write(md_path, lines.join)
  puts "  wrote #{md_path}"
end

if __FILE__ == $0
  cmd = ARGV[0] || "run"
  case cmd
  when "run"     then run_internal
  when "report"  then report_internal
  when "all"     then run_internal; report_internal
  else
    $stderr.puts "usage: ruby run_internal.rb [run|report|all]"
    exit 1
  end
end

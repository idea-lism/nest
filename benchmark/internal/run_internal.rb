# frozen_string_literal: true

# benchmark/internal/run_internal.rb
# Nest-only internal benchmarks: memoize × clang-opt matrix.
# Output: benchmark/results/internal_<grammar>.csv + markdown summary.

require_relative "../common"
include BenchmarkCommon

CC = "xcrun clang"
MEMOIZE_MODES = %w[none naive shared].freeze
OPT_LEVELS = %w[-O0 -O2].freeze

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
      OPT_LEVELS.each do |opt|
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
            out, rss_kb, = run_with_rss([runner_bin, input[:path], iters.to_s])
            fields = out.strip.split(",")
            parse_us = fields[0].to_f
            token_count = fields[2].to_i
            chunk_count = fields[3].to_i
            throughput = input[:size_bytes] / (parse_us / 1e6) / 1e6

            row = {
              "grammar"       => grammar,
              "input"         => input[:name],
              "size_bytes"    => input[:size_bytes],
              "memoize"       => memoize,
              "opt"           => opt,
              "parse_us"      => parse_us.round(1),
              "throughput_mbs" => throughput.round(2),
              "rss_kb"        => rss_kb ? rss_kb.round(1) : "",
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

def report_internal
  md_path = File.join(RESULTS_DIR, "internal_report.md")
  lines = ["# Internal Benchmark Report\n\n"]

  GRAMMARS.each_key do |grammar|
    csv_path = File.join(RESULTS_DIR, "internal_#{grammar}.csv")
    rows = csv_rows(csv_path)
    next if rows.empty?

    lines << "## #{grammar}\n\n"
    lines << "| Input | Memoize | Opt | Parse µs | MB/s | RSS KB | Tokens | Chunks |\n"
    lines << "|-------|---------|-----|----------|------|--------|--------|--------|\n"
    rows.each do |r|
      lines << "| #{r['input']} | #{r['memoize']} | #{r['opt']} | #{r['parse_us']} | #{r['throughput_mbs']} | #{r['rss_kb']} | #{r['token_count']} | #{r['chunk_count']} |\n"
    end
    lines << "\n"
  end

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

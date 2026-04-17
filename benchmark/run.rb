# frozen_string_literal: true

# benchmark/run.rb — main orchestrator.
# Subcommands: setup, gen, internal, compare, report, all

require_relative "common"
include BenchmarkCommon

CC = "xcrun clang"

# ---------- setup ----------

def cmd_setup
  load File.join(ROOT, "setup.rb")
end

# ---------- gen ----------

def cmd_gen
  load File.join(INPUTS_DIR, "gen_inputs.rb")
end

# ---------- internal ----------

def cmd_internal
  require_relative "internal/run_internal"
  run_internal
end

# ---------- compare ----------

def build_nest_compare(grammar, build_dir)
  info = GRAMMARS.fetch(grammar)
  prefix = "bench_#{grammar}"
  ensure_dirs(build_dir)

  compile_ms = timed_command(
    NEST_BIN, "c", info[:nest_grammar], "-p", prefix, "-m", "shared",
    chdir: build_dir
  )

  runner_erb = File.join(RUNNERS_DIR, "nest_runner.c.erb")
  rendered = ERB.new(File.read(runner_erb), trim_mode: "-").result_with_hash(prefix: prefix)
  File.write(File.join(build_dir, "runner.c"), rendered)

  build_ms = timed_command(
    "#{CC} -O2 -std=c23 runner.c #{prefix}.ll -o runner",
    chdir: build_dir
  )

  bin = File.join(build_dir, "runner")
  bin_bytes = File.size(bin)
  { bin: bin, compile_ms: compile_ms.round(1), build_ms: build_ms.round(1), binary_bytes: bin_bytes }
end

def build_packcc_compare(grammar, build_dir)
  info = GRAMMARS.fetch(grammar)
  prefix = grammar # packcc uses grammar name as prefix
  ensure_dirs(build_dir)

  # packcc generate
  compile_ms = timed_command(
    PACKCC_BIN, "-o", "parser", info[:packcc_grammar],
    chdir: build_dir
  )

  # We need to strip the embedded main() from parser.c for linking with our runner.
  # PackCC puts main() after the %% marker; we compile parser.c with -DPACKCC_RUNNER_SKIP_MAIN
  # Actually, simpler: compile parser.c as object, then link runner with it.
  # But the generated parser.c has main(). We strip it by compiling with -Dmain=packcc_original_main
  runner_src = File.join(RUNNERS_DIR, "packcc_runner.c")

  build_ms = timed_command(
    "#{CC} -O2 -Dmain=_packcc_original_main -DPACKCC_PREFIX=#{prefix} " \
    "#{runner_src} parser.c -o runner",
    chdir: build_dir
  )

  bin = File.join(build_dir, "runner")
  bin_bytes = File.size(bin)
  { bin: bin, compile_ms: compile_ms.round(1), build_ms: build_ms.round(1), binary_bytes: bin_bytes }
end

def build_treesitter_compare(grammar, build_dir)
  info = GRAMMARS.fetch(grammar)
  ts_dir = info[:tree_sitter_dir]
  lang = info[:tree_sitter_lang]
  lang_fn = "tree_sitter_#{lang}"
  ensure_dirs(build_dir)

  # tree-sitter generate (if grammar.js exists and src/parser.c doesn't, or always regenerate)
  grammar_js = File.join(ts_dir, "grammar.js")
  parser_c = File.join(ts_dir, "src", "parser.c")
  scanner_c = File.join(ts_dir, "src", "scanner.c")

  compile_ms = 0.0
  if File.exist?(grammar_js)
    compile_ms = timed_command("tree-sitter", "generate", chdir: ts_dir)
  end

  runner_src = File.join(RUNNERS_DIR, "treesitter_runner.c")
  ts_cflags = tree_sitter_cflags

  src_files = [runner_src, parser_c]
  src_files << scanner_c if File.exist?(scanner_c)

  build_ms = timed_command(
    "#{CC} -O2 -DLANG_FN=#{lang_fn} " \
    "#{src_files.join(' ')} " \
    "-I#{File.join(ts_dir, 'src')} " \
    "#{ts_cflags.join(' ')} " \
    "-o runner",
    chdir: build_dir
  )

  bin = File.join(build_dir, "runner")
  bin_bytes = File.size(bin)
  { bin: bin, compile_ms: compile_ms.round(1), build_ms: build_ms.round(1), binary_bytes: bin_bytes }
end

FRAMEWORKS = {
  "nest"        => method(:build_nest_compare),
  "packcc"      => method(:build_packcc_compare),
  "tree-sitter" => method(:build_treesitter_compare)
}.freeze

def cmd_compare
  ensure_dirs(BUILD_DIR, RESULTS_DIR)
  csv_path = File.join(RESULTS_DIR, "compare.csv")
  rows = []

  GRAMMARS.each do |grammar, info|
    input_path = info[:compare_input]
    unless File.exist?(input_path)
      puts "SKIP #{grammar}: no comparison input at #{input_path}"
      next
    end
    size_bytes = File.size(input_path)

    FRAMEWORKS.each do |fw_name, builder|
      build_dir = File.join(BUILD_DIR, "compare", grammar, fw_name)
      puts "==> #{fw_name}/#{grammar}"

      begin
        result = builder.call(grammar, build_dir)
        iters = adaptive_iterations(result[:bin], input_path)
        out, rss_kb, = run_with_rss([result[:bin], input_path, iters.to_s])
        parse_us = out.strip.split(",").first.to_f
        throughput = size_bytes / (parse_us / 1e6) / 1e6

        row = {
          "framework"    => fw_name,
          "grammar"      => grammar,
          "input"        => File.basename(input_path),
          "size_bytes"   => size_bytes,
          "parse_us"     => parse_us.round(1),
          "throughput_mbs" => throughput.round(2),
          "rss_kb"       => rss_kb ? rss_kb.round(1) : "",
          "compile_ms"   => result[:compile_ms],
          "build_ms"     => result[:build_ms],
          "binary_bytes" => result[:binary_bytes]
        }
        rows << row
        puts "  #{parse_us.round(1)} µs, #{throughput.round(1)} MB/s, RSS #{rss_kb&.round(0)} KB"
      rescue StandardError => e
        $stderr.puts "  FAIL #{fw_name}/#{grammar}: #{e.message}"
      end
    end
  end

  if rows.any?
    headers = rows.first.keys
    File.open(csv_path, "w") do |f|
      f.puts(headers.join(","))
      rows.each { |r| f.puts(headers.map { |h| r[h] }.join(",")) }
    end
    puts "  wrote #{csv_path}"
  end
end

# ---------- report ----------

def cmd_report
  # Internal report
  require_relative "internal/run_internal"
  report_internal

  # Comparison report
  csv_path = File.join(RESULTS_DIR, "compare.csv")
  rows = csv_rows(csv_path)
  return if rows.empty?

  md_path = File.join(RESULTS_DIR, "compare_report.md")
  lines = ["# Comparison Benchmark Report\n\n"]

  by_grammar = rows.group_by { |r| r["grammar"] }
  by_grammar.each do |grammar, grammar_rows|
    lines << "## #{grammar}\n\n"
    lines << "| Framework | Parse µs | MB/s | RSS KB | Compile ms | Build ms | Binary KB |\n"
    lines << "|-----------|----------|------|--------|------------|----------|-----------|\n"
    grammar_rows.each do |r|
      bin_kb = r["binary_bytes"].to_i / 1024
      lines << "| #{r['framework']} | #{r['parse_us']} | #{r['throughput_mbs']} | #{r['rss_kb']} | #{r['compile_ms']} | #{r['build_ms']} | #{bin_kb} |\n"
    end
    lines << "\n"
  end

  File.write(md_path, lines.join)
  puts "  wrote #{md_path}"
end

# ---------- all ----------

def cmd_all
  cmd_gen
  cmd_internal
  cmd_compare
  cmd_report
end

# ---------- main ----------

COMMANDS = {
  "setup"    => method(:cmd_setup),
  "gen"      => method(:cmd_gen),
  "internal" => method(:cmd_internal),
  "compare"  => method(:cmd_compare),
  "report"   => method(:cmd_report),
  "all"      => method(:cmd_all)
}.freeze

cmd = ARGV[0]
unless cmd && COMMANDS.key?(cmd)
  $stderr.puts "usage: ruby benchmark/run.rb <#{COMMANDS.keys.join('|')}>"
  exit 1
end

COMMANDS[cmd].call

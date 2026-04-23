# frozen_string_literal: true

require "csv"
require "erb"
require "fileutils"
require "open3"
require "shellwords"

module BenchmarkCommon
  module_function

  ROOT = File.expand_path(__dir__)
  PROJECT_ROOT = File.expand_path("..", ROOT)
  BUILD_DIR = File.join(ROOT, "build")
  RESULTS_DIR = File.join(ROOT, "results")
  RUNNERS_DIR = File.join(ROOT, "runners")
  GRAMMARS_DIR = File.join(ROOT, "grammars")
  INPUTS_DIR = File.join(ROOT, "inputs")
  INPUTS_INTERNAL_DIR = File.join(INPUTS_DIR, "internal")
  INPUTS_COMPARE_DIR = File.join(INPUTS_DIR, "compare")
  VENDOR_DIR = File.join(ROOT, "vendor")

  NEST_BIN = File.join(PROJECT_ROOT, "build", "release", "nest")
  PACKCC_BIN = File.join(VENDOR_DIR, "packcc", "packcc")

  PACKCC_REPO = "https://github.com/arithy/packcc.git"
  TREE_SITTER_JSON_REPO = "https://github.com/tree-sitter/tree-sitter-json.git"
  TREE_SITTER_KOTLIN_REPO = "https://github.com/tree-sitter-grammars/tree-sitter-kotlin.git"

  GRAMMARS = {
    "calc" => {
      input_ext: ".txt",
      compare_input: File.join(INPUTS_COMPARE_DIR, "calc.txt"),
      internal_dir: File.join(INPUTS_INTERNAL_DIR, "calc"),
      nest_grammar: File.join(GRAMMARS_DIR, "calc", "grammar.nest"),
      packcc_grammar: File.join(GRAMMARS_DIR, "calc", "grammar.peg"),
      tree_sitter_dir: File.join(GRAMMARS_DIR, "calc"),
      tree_sitter_lang: "calc"
    },
    "json" => {
      input_ext: ".json",
      compare_input: File.join(INPUTS_COMPARE_DIR, "json.json"),
      internal_dir: File.join(INPUTS_INTERNAL_DIR, "json"),
      nest_grammar: File.join(GRAMMARS_DIR, "json", "grammar.nest"),
      packcc_grammar: File.join(GRAMMARS_DIR, "json", "grammar.peg"),
      tree_sitter_dir: File.join(VENDOR_DIR, "tree-sitter-json"),
      tree_sitter_lang: "json"
    },
    "kotlin" => {
      input_ext: ".kt",
      compare_input: File.join(INPUTS_COMPARE_DIR, "kotlin.kt"),
      internal_dir: File.join(INPUTS_INTERNAL_DIR, "kotlin"),
      nest_grammar: File.join(GRAMMARS_DIR, "kotlin", "grammar.nest"),
      packcc_grammar: File.join(GRAMMARS_DIR, "kotlin", "grammar.peg"),
      tree_sitter_dir: File.join(VENDOR_DIR, "tree-sitter-kotlin"),
      tree_sitter_lang: "kotlin"
    }
  }.freeze

  def ensure_dirs(*paths)
    paths.flatten.each { |path| FileUtils.mkdir_p(path) }
  end

  def write(path, content)
    ensure_dirs(File.dirname(path))
    File.write(path, content)
  end

  def render_erb(path, vars)
    ERB.new(File.read(path), trim_mode: "-").result_with_hash(vars)
  end

  def sh *cmd, chdir: nil, env: nil, **_ignored
    print "system "
    p cmd
    opts = {}
    opts[:chdir] = chdir if chdir
    p opts
    args = env && !env.empty? ? [env, *cmd] : cmd
    system *args, out: $stdout, err: $stderr, **opts or abort "sh failed"
  end

  def timed_command(*cmd, chdir: nil, env: nil)
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    sh(*cmd, chdir: chdir, env: env)
    t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    (t1 - t0) * 1000.0
  end

  def shell_join(parts)
    parts.map { |part| Shellwords.escape(part.to_s) }.join(" ")
  end

  def parse_rss_kb(stderr)
    line = stderr.lines.find { |it| it.include?("maximum resident set size") }
    return nil unless line

    bytes = line[/([0-9]+)/, 1]
    return nil unless bytes

    bytes.to_i / 1024.0
  end

  def parse_trace_total_malloc(stderr)
    line = stderr.lines.find { |it| it.start_with?("TRACE_TOTAL_MALLOC=") }
    return nil unless line

    line[/TRACE_TOTAL_MALLOC=([0-9]+)/, 1]&.to_i
  end

  def run_with_rss(cmd, chdir: nil, env: {})
    opts = {}
    opts[:chdir] = chdir if chdir
    out, err, status = Open3.capture3(env, "/usr/bin/time", "-l", *cmd, **opts)
    raise "command failed: #{shell_join(cmd)}\n#{err}" unless status.success?

    [out, parse_rss_kb(err), err]
  end

  def adaptive_iterations(bin_path, input_path, chdir: nil)
    out, = run_with_rss([bin_path, input_path, "1"], chdir: chdir)
    csv_line = out.strip.lines.last&.strip || ""
    parse_us = csv_line.split(",", 2).first.to_f
    return 5 if parse_us <= 0.0

    [[(1_000_000.0 / parse_us).ceil, 5].max, 1000].min
  rescue StandardError
    5
  end

  def csv_rows(path)
    return [] unless File.exist?(path)

    CSV.read(path, headers: true).map(&:to_h)
  end

  def tree_sitter_prefix
    @tree_sitter_prefix ||= begin
      out, err, status = Open3.capture3("brew", "--prefix", "tree-sitter")
      raise "brew --prefix tree-sitter failed\n#{err}" unless status.success?

      out.strip
    end
  end

  def tree_sitter_cflags
    prefix = tree_sitter_prefix
    ["-I#{File.join(prefix, 'include')}", "-L#{File.join(prefix, 'lib')}", "-ltree-sitter"]
  end

  def sync_git_repo(url, dir)
    if Dir.exist?(dir)
      sh("git", "-C", dir, "fetch", "origin")
      sh("git", "-C", dir, "reset", "--hard", "origin/HEAD")
    else
      ensure_dirs(File.dirname(dir))
      sh("git", "clone", "--depth", "1", url, dir)
    end
  end

  def copy_if_exists(src, dst)
    return unless File.exist?(src)

    ensure_dirs(File.dirname(dst))
    FileUtils.cp(src, dst)
  end

  def preferred_internal_input(grammar)
    dir = GRAMMARS.fetch(grammar)[:internal_dir]
    candidates = Dir.glob(File.join(dir, "*"))
    preferred = candidates.find { |path| File.basename(path).start_with?("1m_mixed") } ||
      candidates.find { |path| File.basename(path).start_with?("1m_base") }
    preferred || candidates.first
  end
end

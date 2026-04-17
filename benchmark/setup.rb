# frozen_string_literal: true

# benchmark/setup.rb — install competitors and copy comparison inputs/grammars.
# All builds use `xcrun clang`. macOS only.

require_relative "common"
include BenchmarkCommon

CC = "xcrun clang"

def step(msg)
  puts "==> #{msg}"
end

def setup_packcc
  step "PackCC: clone + build"
  repo_dir = File.join(VENDOR_DIR, "packcc")
  sync_git_repo(PACKCC_REPO, repo_dir)

  src = File.join(repo_dir, "src", "packcc.c")
  out = File.join(repo_dir, "packcc")
  sh("#{CC} -O2 #{src} -o #{out}")
  puts "  built #{out}"
end

def setup_tree_sitter
  step "tree-sitter: check brew"
  begin
    sh("brew", "list", "tree-sitter", quiet: true)
    puts "  already installed"
  rescue StandardError
    puts "  installing via brew..."
    sh("brew", "install", "tree-sitter")
  end
end

def setup_tree_sitter_grammars
  step "tree-sitter grammars: clone"
  sync_git_repo(TREE_SITTER_JSON_REPO, File.join(VENDOR_DIR, "tree-sitter-json"))
  sync_git_repo(TREE_SITTER_KOTLIN_REPO, File.join(VENDOR_DIR, "tree-sitter-kotlin"))
end

def copy_compare_inputs
  step "Copy PackCC benchmark inputs → inputs/compare/"
  packcc_dir = File.join(VENDOR_DIR, "packcc")
  ensure_dirs(INPUTS_COMPARE_DIR)
  {
    "benchmark/inputs/calc.txt"   => "calc.txt",
    "benchmark/inputs/json.json"  => "json.json",
    "benchmark/inputs/kotlin.kt"  => "kotlin.kt"
  }.each do |src_rel, dst_name|
    src = File.join(packcc_dir, src_rel)
    dst = File.join(INPUTS_COMPARE_DIR, dst_name)
    copy_if_exists(src, dst)
    puts "  #{dst_name} (#{File.size(dst)} bytes)" if File.exist?(dst)
  end
end

def copy_licenses
  step "Copy upstream licenses → licenses/"
  licenses_dir = File.join(ROOT, "licenses")
  ensure_dirs(licenses_dir)
  {
    File.join(VENDOR_DIR, "packcc", "LICENSE")             => "packcc.LICENSE",
    File.join(VENDOR_DIR, "tree-sitter-json", "LICENSE")   => "tree-sitter-json.LICENSE",
    File.join(VENDOR_DIR, "tree-sitter-kotlin", "LICENSE") => "tree-sitter-kotlin.LICENSE"
  }.each do |src, dst_name|
    dst = File.join(licenses_dir, dst_name)
    copy_if_exists(src, dst)
    puts "  #{dst_name}" if File.exist?(dst)
  end
end

def main
  ensure_dirs(VENDOR_DIR)
  setup_packcc
  setup_tree_sitter
  setup_tree_sitter_grammars
  copy_compare_inputs
  copy_licenses
  puts "\nSetup complete."
end

main

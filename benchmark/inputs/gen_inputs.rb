# frozen_string_literal: true

# Deterministic input generator for INTERNAL benchmarks / ablation studies only.
# Comparison benchmarks use PackCC benchmark repo inputs verbatim.

require "fileutils"

SEED = 42
SIZES = {
  "1k"  => 1_024,
  "10k" => 10_240,
  "1m"  => 1_048_576
}.freeze

OUT_DIR = File.join(__dir__, "internal")

def gen_calc_flat(rng, target)
  buf = +""
  loop do
    n = rng.rand(1..99999)
    op = ["+", "-", "*", "/"][rng.rand(4)]
    line = buf.empty? ? n.to_s : "#{op} #{n}"
    buf << line
    break if buf.bytesize >= target

    buf << "\n" if rng.rand(10) < 3
  end
  buf << "\n"
  buf
end

def gen_calc_deep(rng, target)
  buf = +""
  depth = 0
  loop do
    if rng.rand(3) == 0 && depth < 200
      buf << "("
      depth += 1
    end
    buf << rng.rand(1..99999).to_s
    break if buf.bytesize >= target

    if depth > 0 && rng.rand(3) == 0
      buf << ")"
      depth -= 1
    end
    buf << [" + ", " - ", " * ", " / "][rng.rand(4)]
  end
  depth.times { buf << ")" }
  buf << "\n"
  buf
end

def gen_calc_mixed(rng, target)
  buf = +""
  loop do
    case rng.rand(4)
    when 0
      buf << "("
    when 1
      buf << ")" if buf.count("(") > buf.count(")")
    when 2
      buf << rng.rand(1..99999).to_s
    when 3
      buf << [" + ", " - ", " * ", " / "][rng.rand(4)]
    end
    break if buf.bytesize >= target

    buf << "\n" if rng.rand(20) == 0
  end
  excess = buf.count("(") - buf.count(")")
  excess.times { buf << ")" } if excess > 0
  buf << "\n"
  buf
end

def gen_json_flat(rng, target)
  buf = +"["
  first = true
  loop do
    buf << "," unless first
    first = false
    buf << rng.rand(-99999..99999).to_s
    break if buf.bytesize >= target
  end
  buf << "]\n"
  buf
end

def gen_json_deep(rng, target)
  buf = +""
  total_depth = 0
  while buf.bytesize < target
    depth = 0
    while buf.bytesize < target && depth < 200
      key = "k#{rng.rand(1000)}"
      buf << "," if total_depth > 0 && depth == 0 && !buf.empty?
      buf << "{\"#{key}\":"
      depth += 1
    end
    buf << rng.rand(1..99999).to_s
    depth.times { buf << "}" }
    total_depth += depth
  end
  buf << "\n"
  buf
end

def gen_json_wide(rng, target)
  buf = +"{"
  first = true
  idx = 0
  loop do
    buf << "," unless first
    first = false
    buf << "\"key_#{idx}\":#{rng.rand(-9999..9999)}"
    idx += 1
    break if buf.bytesize >= target
  end
  buf << "}\n"
  buf
end

def gen_json_mixed(rng, target)
  buf = +""
  # Build a top-level array of mixed values until target reached
  buf << "["
  first = true
  while buf.bytesize < target
    buf << "," unless first
    first = false
    _gen_json_value(rng, buf, target, 0)
  end
  buf << "]\n"
  buf
end

def _gen_json_value(rng, buf, target, depth)
  if depth > 12 || buf.bytesize >= target
    buf << rng.rand(-9999..9999).to_s
    return
  end

  case rng.rand(6)
  when 0 # number
    buf << rng.rand(-9999..9999).to_s
  when 1 # string
    buf << "\"str_#{rng.rand(10000)}\""
  when 2 # bool
    buf << (rng.rand(2) == 0 ? "true" : "false")
  when 3 # null
    buf << "null"
  when 4 # array
    n = rng.rand(1..8)
    buf << "["
    n.times do |i|
      buf << "," if i > 0
      _gen_json_value(rng, buf, target, depth + 1)
    end
    buf << "]"
  when 5 # object
    n = rng.rand(1..6)
    buf << "{"
    n.times do |i|
      buf << "," if i > 0
      buf << "\"f#{rng.rand(10000)}\":"
      _gen_json_value(rng, buf, target, depth + 1)
    end
    buf << "}"
  end
end

KOTLIN_FUNS = [
  "fun f%d(x: Int): Int = x + %d\n",
  "val v%d = %d\n",
  "fun g%d(a: Int, b: Int): Int {\n    val c = a + b\n    return c * %d\n}\n",
  "class C%d(val x: Int = %d)\n"
].freeze

def gen_kotlin_base(rng, target)
  buf = +"package bench\n\n"
  idx = 0
  loop do
    tmpl = KOTLIN_FUNS[rng.rand(KOTLIN_FUNS.size)]
    buf << format(tmpl, idx, rng.rand(1..9999))
    buf << "\n"
    idx += 1
    break if buf.bytesize >= target
  end
  buf
end

GENERATORS = {
  "calc" => {
    "flat"  => method(:gen_calc_flat),
    "deep"  => method(:gen_calc_deep),
    "mixed" => method(:gen_calc_mixed)
  },
  "json" => {
    "flat"  => method(:gen_json_flat),
    "deep"  => method(:gen_json_deep),
    "wide"  => method(:gen_json_wide),
    "mixed" => method(:gen_json_mixed)
  },
  "kotlin" => {
    "base" => method(:gen_kotlin_base)
  }
}.freeze

def main
  rng = Random.new(SEED)
  total = 0

  GENERATORS.each do |grammar, variants|
    dir = File.join(OUT_DIR, grammar)
    FileUtils.mkdir_p(dir)

    SIZES.each do |label, target|
      variants.each do |variant, gen|
        name = "#{label}_#{variant}"
        path = File.join(dir, name)
        content = gen.call(rng, target)
        File.write(path, content)
        total += 1
        puts "  #{grammar}/#{name}  #{content.bytesize} bytes"
      end
    end
  end

  puts "\nGenerated #{total} input files in #{OUT_DIR}"
end

main

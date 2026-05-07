#!/usr/bin/env ruby
# frozen_string_literal: true

# Collect the KotlinLexer.g4 DEFAULT_MODE (Nest `main`) rule closure.
#
# The output is a newline-separated list of lexer rule names reachable from the
# ANTLR default mode, following references to lexer rules/fragments but not
# following pushMode targets such as Inside, LineString, or MultiLineString.

require "net/http"
require "uri"

DEFAULT_URL = "https://raw.githubusercontent.com/Kotlin/kotlin-spec/release/grammar/src/main/antlr/KotlinLexer.g4"
MODE_NAMES = %w[DEFAULT_MODE Inside LineString MultiLineString].to_set.freeze
SKIP_REFS = (MODE_NAMES | %w[HIDDEN]).freeze
ANTLR_WORDS = %w[
  channel pushMode popMode type mode fragment
  if isEmpty skip more
].to_set.freeze

def read_source
  if ARGV[0]
    File.read(ARGV[0])
  else
    uri = URI(DEFAULT_URL)
    Net::HTTP.get(uri)
  end
end

def mask_comments(src)
  out = src.chars
  i = 0
  n = src.size
  in_single = false
  in_class = false
  escaped = false

  while i < n
    c = src[i]
    if in_single
      if escaped
        escaped = false
      elsif c == "\\"
        escaped = true
      elsif c == "'"
        in_single = false
      end
      i += 1
    elsif in_class
      if escaped
        escaped = false
      elsif c == "\\"
        escaped = true
      elsif c == "]"
        in_class = false
      end
      i += 1
    elsif c == "'"
      in_single = true
      i += 1
    elsif c == "["
      in_class = true
      i += 1
    elsif src[i, 2] == "//"
      j = src.index("\n", i) || n
      (i...j).each { |k| out[k] = " " }
      i = j
    elsif src[i, 2] == "/*"
      j = src.index("*/", i + 2)
      j = n - 2 if j.nil?
      (i...(j + 2)).each { |k| out[k] = " " unless out[k] == "\n" }
      i = j + 2
    else
      i += 1
    end
  end
  out.join
end

def parse_rules(src)
  rules = {}
  default_rules = []
  current_mode = "DEFAULT_MODE"
  i = 0
  n = src.size

  while i < n
    if (m = src[i..].match(/\A\s*mode\s+([A-Za-z_][A-Za-z0-9_]*)\s*;/))
      current_mode = m[1]
      i += m[0].size
      next
    end

    m = src[i..].match(/\A\s*(fragment\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:/)
    unless m
      i += 1
      next
    end

    is_fragment = !m[1].nil?
    name = m[2]
    body_start = i + m[0].size
    j = body_start
    in_single = false
    in_class = false
    escaped = false

    while j < n
      c = src[j]
      if in_single
        if escaped
          escaped = false
        elsif c == "\\"
          escaped = true
        elsif c == "'"
          in_single = false
        end
      elsif in_class
        if escaped
          escaped = false
        elsif c == "\\"
          escaped = true
        elsif c == "]"
          in_class = false
        end
      else
        if c == "'"
          in_single = true
        elsif c == "["
          in_class = true
        elsif c == ";"
          break
        end
      end
      j += 1
    end

    body = src[body_start...j]
    rules[name] = body
    default_rules << name if current_mode == "DEFAULT_MODE" && !is_fragment
    i = j + 1
  end

  [rules, default_rules]
end

def mask_literals_and_classes(body)
  chars = body.chars
  i = 0
  n = body.size
  in_single = false
  in_class = false
  escaped = false
  start = 0

  while i < n
    c = body[i]
    if in_single
      if escaped
        escaped = false
      elsif c == "\\"
        escaped = true
      elsif c == "'"
        (start..i).each { |k| chars[k] = " " unless chars[k] == "\n" }
        in_single = false
      end
      i += 1
    elsif in_class
      if escaped
        escaped = false
      elsif c == "\\"
        escaped = true
      elsif c == "]"
        (start..i).each { |k| chars[k] = " " unless chars[k] == "\n" }
        in_class = false
      end
      i += 1
    elsif c == "'"
      in_single = true
      start = i
      i += 1
    elsif c == "["
      in_class = true
      start = i
      i += 1
    else
      i += 1
    end
  end
  chars.join
end

def refs_in_body(body)
  body = mask_literals_and_classes(body)
  refs = []
  body.scan(/\b[A-Za-z_][A-Za-z0-9_]*\b/) do |name|
    next if SKIP_REFS.include?(name) || ANTLR_WORDS.include?(name)
    refs << name
  end
  refs
end

def closure(rules, roots)
  seen = Set.new
  out = []

  visit = lambda do |name|
    return if seen.include?(name) || !rules.key?(name)
    seen.add(name)
    out << name
    refs_in_body(rules[name]).each { |ref| visit.call(ref) }
  end

  roots.each { |root| visit.call(root) }
  out
end

def main
  require "set"
  src = mask_comments(read_source)
  rules, default_rules = parse_rules(src)
  closure(rules, default_rules).each { |name| puts name }
  0
end

exit(main)

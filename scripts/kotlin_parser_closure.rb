#!/usr/bin/env ruby
# frozen_string_literal: true

# Collect a KotlinParser.g4 parser-rule closure.
#
# Default root is `kotlinFile`. The default stop set is intended for the Nest
# `main` PEG scope: include boundary parser rules in the list, but do not expand
# rules whose contents belong to nested lexer/parser scopes such as blocks,
# parenthesized/bracketed Inside chunks, or string literals.

require "net/http"
require "uri"
require "set"
require "optparse"

DEFAULT_URL = "https://raw.githubusercontent.com/Kotlin/kotlin-spec/release/grammar/src/main/antlr/KotlinParser.g4"

DEFAULT_STOP_RULES = %w[
  block
  classBody
  enumClassBody
  lambdaLiteral
  whenExpression
  classParameters
  functionValueParameters
  parametersWithOptionalType
  multiVariableDeclaration
  functionTypeParameters
  parenthesizedType
  parenthesizedUserType
  parenthesizedExpression
  parenthesizedAssignableExpression
  parenthesizedDirectlyAssignableExpression
  indexingSuffix
  collectionLiteral
  valueArguments
  whenSubject
  catchBlock
  stringLiteral
  lineStringLiteral
  multiLineStringLiteral
].to_set.freeze

ANTLR_WORDS = %w[parser grammar options tokenVocab].to_set.freeze

def read_source(path)
  if path
    File.read(path)
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
    elsif c == "'"
      in_single = true
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
  i = 0
  n = src.size

  while i < n
    m = src[i..].match(/\A\s*([a-z][A-Za-z0-9_]*)\s*:/)
    unless m
      i += 1
      next
    end

    name = m[1]
    body_start = i + m[0].size
    j = body_start
    in_single = false
    escaped = false
    paren = 0
    bracket = 0
    brace = 0

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
      else
        case c
        when "'"
          in_single = true
        when "("
          paren += 1
        when ")"
          paren -= 1 if paren > 0
        when "["
          bracket += 1
        when "]"
          bracket -= 1 if bracket > 0
        when "{"
          brace += 1
        when "}"
          brace -= 1 if brace > 0
        when ";"
          break if paren == 0 && bracket == 0 && brace == 0
        end
      end
      j += 1
    end

    rules[name] = src[body_start...j]
    i = j + 1
  end
  rules
end

def mask_literals(body)
  chars = body.chars
  i = 0
  n = body.size
  in_single = false
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
    elsif c == "'"
      in_single = true
      start = i
      i += 1
    else
      i += 1
    end
  end
  chars.join
end

def refs_in_body(body, rule_names)
  body = mask_literals(body)
  refs = []
  body.scan(/\b[a-z][A-Za-z0-9_]*\b/) do |name|
    next if ANTLR_WORDS.include?(name)
    refs << name if rule_names.include?(name)
  end
  refs
end

def closure(rules, root, stop)
  rule_names = rules.keys.to_set
  seen = Set.new
  out = []

  visit = lambda do |name|
    return if seen.include?(name) || !rules.key?(name)
    seen.add(name)
    out << name
    return if stop.include?(name)
    refs_in_body(rules[name], rule_names).each { |ref| visit.call(ref) }
  end

  visit.call(root)
  out
end

def main
  grammar_path = nil
  root = "kotlinFile"
  no_default_stops = false
  extra_stops = []

  OptionParser.new do |opts|
    opts.banner = "Usage: #{$PROGRAM_NAME} [options] [grammar.g4]"
    opts.on("--root ROOT", "Root rule (default: kotlinFile)") { |v| root = v }
    opts.on("--no-default-stops", "Disable default stop rules") { no_default_stops = true }
    opts.on("--stop RULE", "Extra stop rule (may be repeated)") { |v| extra_stops << v }
  end.parse!

  grammar_path = ARGV[0]

  src = mask_comments(read_source(grammar_path))
  rules = parse_rules(src)
  stop = extra_stops.to_set
  stop.merge(DEFAULT_STOP_RULES) unless no_default_stops
  closure(rules, root, stop).each { |name| puts name }
  0
end

exit(main)

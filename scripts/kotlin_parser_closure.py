#!/usr/bin/env python3
"""Collect a KotlinParser.g4 parser-rule closure.

Default root is `kotlinFile`. The default stop set is intended for the Nest
`main` PEG scope: include boundary parser rules in the list, but do not expand
rules whose contents belong to nested lexer/parser scopes such as blocks,
parenthesized/bracketed Inside chunks, or string literals.
"""

from __future__ import annotations

import argparse
import re
import urllib.request
from pathlib import Path

DEFAULT_URL = "https://raw.githubusercontent.com/Kotlin/kotlin-spec/release/grammar/src/main/antlr/KotlinParser.g4"

DEFAULT_STOP_RULES = {
    # `{ ... }` DEFAULT_MODE nested chunks.
    "block",
    "classBody",
    "enumClassBody",
    "lambdaLiteral",
    "whenExpression",
    # `Inside` chunks: parentheses / brackets.
    "classParameters",
    "functionValueParameters",
    "parametersWithOptionalType",
    "multiVariableDeclaration",
    "functionTypeParameters",
    "parenthesizedType",
    "parenthesizedUserType",
    "parenthesizedExpression",
    "parenthesizedAssignableExpression",
    "parenthesizedDirectlyAssignableExpression",
    "indexingSuffix",
    "collectionLiteral",
    "valueArguments",
    "whenSubject",
    "catchBlock",
    # String VPA scopes.
    "stringLiteral",
    "lineStringLiteral",
    "multiLineStringLiteral",
}

ANTLR_WORDS = {
    "parser", "grammar", "options", "tokenVocab",
}


def read_source(path: str | None) -> str:
    if path:
        return Path(path).read_text()
    with urllib.request.urlopen(DEFAULT_URL, timeout=30) as r:
        return r.read().decode()


def mask_comments(src: str) -> str:
    out = list(src)
    i = 0
    n = len(src)
    in_single = False
    escaped = False
    while i < n:
        c = src[i]
        if in_single:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == "'":
                in_single = False
            i += 1
        elif c == "'":
            in_single = True
            i += 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            if j < 0:
                j = n
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            if j < 0:
                j = n - 2
            for k in range(i, j + 2):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 2
        else:
            i += 1
    return "".join(out)


def parse_rules(src: str) -> dict[str, str]:
    rules: dict[str, str] = {}
    i = 0
    n = len(src)
    while i < n:
        # Parser rules start lowercase in ANTLR grammar.
        m = re.match(r"\s*([a-z][A-Za-z0-9_]*)\s*:", src[i:])
        if not m:
            i += 1
            continue
        name = m.group(1)
        body_start = i + m.end()
        j = body_start
        in_single = False
        escaped = False
        paren = bracket = brace = 0
        while j < n:
            c = src[j]
            if in_single:
                if escaped:
                    escaped = False
                elif c == "\\":
                    escaped = True
                elif c == "'":
                    in_single = False
            else:
                if c == "'":
                    in_single = True
                elif c == "(":
                    paren += 1
                elif c == ")" and paren > 0:
                    paren -= 1
                elif c == "[":
                    bracket += 1
                elif c == "]" and bracket > 0:
                    bracket -= 1
                elif c == "{":
                    brace += 1
                elif c == "}" and brace > 0:
                    brace -= 1
                elif c == ";" and paren == 0 and bracket == 0 and brace == 0:
                    break
            j += 1
        rules[name] = src[body_start:j]
        i = j + 1
    return rules


def mask_literals(body: str) -> str:
    chars = list(body)
    i = 0
    n = len(body)
    in_single = False
    escaped = False
    start = 0
    while i < n:
        c = body[i]
        if in_single:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == "'":
                for k in range(start, i + 1):
                    if chars[k] != "\n":
                        chars[k] = " "
                in_single = False
            i += 1
        elif c == "'":
            in_single = True
            start = i
            i += 1
        else:
            i += 1
    return "".join(chars)


def refs_in_body(body: str, rule_names: set[str]) -> list[str]:
    body = mask_literals(body)
    refs: list[str] = []
    for name in re.findall(r"\b[a-z][A-Za-z0-9_]*\b", body):
        if name in ANTLR_WORDS:
            continue
        if name in rule_names:
            refs.append(name)
    return refs


def closure(rules: dict[str, str], root: str, stop: set[str]) -> list[str]:
    rule_names = set(rules)
    seen: set[str] = set()
    out: list[str] = []

    def visit(name: str) -> None:
        if name in seen or name not in rules:
            return
        seen.add(name)
        out.append(name)
        if name in stop:
            return
        for ref in refs_in_body(rules[name], rule_names):
            visit(ref)

    visit(root)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("grammar", nargs="?", help="path to KotlinParser.g4; fetches official raw file if omitted")
    ap.add_argument("--root", default="kotlinFile")
    ap.add_argument("--no-default-stops", action="store_true")
    ap.add_argument("--stop", action="append", default=[], help="extra stop rule; may be repeated")
    args = ap.parse_args()

    src = mask_comments(read_source(args.grammar))
    rules = parse_rules(src)
    stop = set(args.stop)
    if not args.no_default_stops:
        stop |= DEFAULT_STOP_RULES
    for name in closure(rules, args.root, stop):
        print(name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

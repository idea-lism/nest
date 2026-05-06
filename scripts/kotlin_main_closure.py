#!/usr/bin/env python3
"""Collect the KotlinLexer.g4 DEFAULT_MODE (Nest `main`) rule closure.

The output is a newline-separated list of lexer rule names reachable from the
ANTLR default mode, following references to lexer rules/fragments but not
following pushMode targets such as Inside, LineString, or MultiLineString.
"""

from __future__ import annotations

import re
import sys
import urllib.request
from pathlib import Path

DEFAULT_URL = "https://raw.githubusercontent.com/Kotlin/kotlin-spec/release/grammar/src/main/antlr/KotlinLexer.g4"
MODE_NAMES = {"DEFAULT_MODE", "Inside", "LineString", "MultiLineString"}
SKIP_REFS = MODE_NAMES | {"HIDDEN"}
ANTLR_WORDS = {
    "channel", "pushMode", "popMode", "type", "mode", "fragment",
    "if", "isEmpty", "skip", "more",
}


def read_source() -> str:
    if len(sys.argv) > 1:
        return Path(sys.argv[1]).read_text()
    with urllib.request.urlopen(DEFAULT_URL, timeout=30) as r:
        return r.read().decode()


def mask_comments(src: str) -> str:
    """Replace comments with spaces, but leave quoted literals/classes intact."""
    out = list(src)
    i = 0
    n = len(src)
    in_single = False
    in_class = False
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
        elif in_class:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == "]":
                in_class = False
            i += 1
        elif c == "'":
            in_single = True
            i += 1
        elif c == "[":
            in_class = True
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


def parse_rules(src: str) -> tuple[dict[str, str], list[str]]:
    rules: dict[str, str] = {}
    default_rules: list[str] = []
    current_mode = "DEFAULT_MODE"
    i = 0
    n = len(src)

    while i < n:
        mode_match = re.match(r"\s*mode\s+([A-Za-z_][A-Za-z0-9_]*)\s*;", src[i:])
        if mode_match:
            current_mode = mode_match.group(1)
            i += mode_match.end()
            continue

        rule_match = re.match(r"\s*(fragment\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:", src[i:])
        if not rule_match:
            i += 1
            continue

        is_fragment = bool(rule_match.group(1))
        name = rule_match.group(2)
        body_start = i + rule_match.end()
        j = body_start
        in_single = False
        in_class = False
        escaped = False
        while j < n:
            c = src[j]
            if in_single:
                if escaped:
                    escaped = False
                elif c == "\\":
                    escaped = True
                elif c == "'":
                    in_single = False
            elif in_class:
                if escaped:
                    escaped = False
                elif c == "\\":
                    escaped = True
                elif c == "]":
                    in_class = False
            else:
                if c == "'":
                    in_single = True
                elif c == "[":
                    in_class = True
                elif c == ";":
                    break
            j += 1

        body = src[body_start:j]
        rules[name] = body
        if current_mode == "DEFAULT_MODE" and not is_fragment:
            default_rules.append(name)
        i = j + 1

    return rules, default_rules


def mask_literals_and_classes(body: str) -> str:
    chars = list(body)
    i = 0
    n = len(body)
    in_single = False
    in_class = False
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
        elif in_class:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == "]":
                for k in range(start, i + 1):
                    if chars[k] != "\n":
                        chars[k] = " "
                in_class = False
            i += 1
        elif c == "'":
            in_single = True
            start = i
            i += 1
        elif c == "[":
            in_class = True
            start = i
            i += 1
        else:
            i += 1
    return "".join(chars)


def refs_in_body(body: str) -> list[str]:
    body = mask_literals_and_classes(body)
    refs = []
    for name in re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", body):
        if name in SKIP_REFS or name in ANTLR_WORDS:
            continue
        refs.append(name)
    return refs


def closure(rules: dict[str, str], roots: list[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []

    def visit(name: str) -> None:
        if name in seen or name not in rules:
            return
        seen.add(name)
        out.append(name)
        for ref in refs_in_body(rules[name]):
            visit(ref)

    for root in roots:
        visit(root)
    return out


def main() -> int:
    src = mask_comments(read_source())
    rules, default_rules = parse_rules(src)
    for name in closure(rules, default_rules):
        print(name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/bin/sh
set -e
set -x

cd "$(dirname "$0")"

CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"
TARGET="${TARGET:-$(${CC} -dumpmachine)}"
ULEX="${ULEX:-../build/release/ulex}"

$ULEX -t "$TARGET" tokens.txt lex.ll
$CC -c lex.ll -o lex.o
$CC $CFLAGS -I ../out main.c lex.o ../out/libre.a -o main

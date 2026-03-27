#!/bin/sh
set -e
set -x

cd "$(dirname "$0")"

CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"
NEST="${NEST:-../build/release/nest}"

$NEST l tokens.txt -o lex.ll
$CC -c lex.ll -o lex.o
$CC $CFLAGS -I ../out main.c lex.o ../out/libre.a -o main

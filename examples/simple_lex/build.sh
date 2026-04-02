#!/bin/sh
set -e

CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"
NEST="${NEST:-../../build/debug/nest}"

$NEST l tokens.txt -o lex.ll
$CC $CFLAGS -c lex.ll -o lex.o
$CC $CFLAGS main.c lex.o -o main

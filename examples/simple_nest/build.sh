#!/bin/sh
set -e

CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"
NEST="${NEST:-../../build/debug/nest}"

$NEST c grammar.nest -o grammar.ll -p calc
$CC $CFLAGS -c grammar.ll -o grammar.o
$CC $CFLAGS main.c grammar.o -o main

#!/bin/sh
set -e

CC="${CC:-clang}"
CFLAGS="${CFLAGS:--std=c23 -O0 -g}"
NEST="${NEST:-../../build/debug/nest}"

$NEST c grammar.nest -p calc
$CC $CFLAGS calc.c calc.ll -o main

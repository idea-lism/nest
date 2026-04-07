OPTION(o, output, 1, "output LLVM IR file")
OPTION(t, triple, 2, "target triple (default: probe clang)")
OPTION(k, compress - memoize, 2, "compress memoize table (default: true)")
OPTION(m, main - parse, 1, "generated main parse function name")
#undef OPTION

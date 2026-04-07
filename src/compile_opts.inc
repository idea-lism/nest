OPTION(o, output, 1, "output LLVM IR file")
OPTION(t, triple, 2, "target triple (default: probe clang)")
OPTION(k, compress - memoize, 2, "compress memoize table (default: true)")
OPTION(p, prefix, 1, "generated function prefix")
#undef OPTION

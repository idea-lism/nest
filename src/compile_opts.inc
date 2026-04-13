OPTION(t, triple, 2, "target triple (default: probe clang)")
OPTION(m, memoize, 1, "memoize mode: none, naive, shared (default: shared)")
OPTION(p, prefix, 1, "generated function prefix")
OPTION(v, verbose, 2, "verbosity level (default: 0)")
#undef OPTION

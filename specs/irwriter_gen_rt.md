# Runtime IR Compilation

The generated `.ll` is self-contained: nest runtime (darray, token_tree, ustr) compiled to LLVM IR + nest-generated parser/lexer IR.

## Amalgamating header and source files

`scripts/amalgamate.rb` reads input files line by line, removes headers appeared before, removes redundant `#pragma once`.

That is (we don't use the build/tools/amalgamate any more, and remove the tool download script if there's any):

```ruby
case line
when /^#pragma once/
  # don't emit, common_header_gen already emits one, and amalgamated impl doesn't need any
when /^#include </
  # system include, no need rewrite
when /^#include "(\w+).h"/
  # search for include dir for the header
  # if $1 not included yet, read it and recursively process it
else
  # output
end
```

In `config.rb`, the `amalgamate` method reads inputs and merge them into one:

```ruby
def amalgamate name, src:, include_dirs: []
  # call scripts/amalgamate.rb to merge them
end
```

In `config.in.rb`, define these two amalgamates:

```ruby
# runtime header for user interface
gen_str_header "build/nest_rt.inc",
  from: "build/nest_rt.h"

amalgamate "build/nest_rt.h",
  src: %w[src/nest_rt.h],
  include_dirs: %w[src]

gen_str_header "build/nest_rt_impl.inc",
  from: "build/nest_rt.c"

amalgamate "build/nest_rt.c",
  src: %w[src/darray.c src/bitset.c src/token_tree.c src/ustr.c src/ustr_avx.c src/ustr_neon.c]
  include_dirs: %w[src]
```

## Interface

Create `irwriter_gen_rt.c`, which implements the following interfaces (interface defines in irwriter.h):

```c
// pipes string buf nest_rt to cc and print to IrWriter's stream
// if cc/clang not found or doesn't complete, or prints something weird, print out everything from the command's stderr/stdout, and abort()
void irwriter_gen_rt(IrWriter*, const char* source_file, const char* directory);

// similar to rt_ir_begin,
// but only pipes empty string to cc and print to IrWriter's stream
// used by parse_gen.c / llir_parse_gen.c / tests
void irwriter_gen_rt_simple(IrWriter*);
```

## IR Parsing and Transforming

`irwriter_gen_rt.c` also parses LLVM IR to do pre-processing.

Create `src/llir_parse_gen.c`, which is similar to `parse_gen.c` that creates a lexer to analyze LLVM IR.

rt_ir will lex the stdout of `cc/clang` (input driven). it is a state machine, which does a partial parse:
- rewrites `source_filename = ` by arg.
- rewrites `directory = ` by arg.
- rewrites `!DIFile` to "nest".
- tracks functions start / end, treats the internal of a function as bunch of flat tokens and just forward.
- to hide non-user-facing runtime functions, rewrite `define {fn_name}` to `define internal {fn_name}`, if `fn_name` is not any of:
  - `ustr_*`
  - `tt_locate`
  - `darray_size`
- traces the debug metadata numbering, and call `irwriter_start(w, max_num + 1)`

So we will have roughly these states in the parser:

```c
typedef enum {
  IR_STATE_NORMAL,
  IR_STATE_AFTER_DEFINE,
  IR_STATE_AFTER_DIFILE, // after !DIFile
  IR_STATE_AFTER_DIFILE_FILENAME, // after !DIFile(finename: -- this is when we can override the name
  ...
  IR_STATE_IN_FUNC,
} IRState;
```

Parser is also greedy and stashes one last item for action changes.

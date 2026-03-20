Simple LLVM IR writer

- For convenient text IR building -- so building does not depend on LLVM project
- simple, just write to `FILE*`
- maps state name to basic block label
- auto numbering

We are targeting DFA generating, so there is no loop, so no need complex Dominance-Frontier algorithm.

API

- basic: `irwriter_new(FILE*)`, `irwriter_del()`
- module prelude and epilogure `irwriter_start()`, `irwriter_end()`
- function prelude and epilogure `irwriter_define_start()`, `irwriter_define_end()`
- starting a basic block
- binop, binop_imm, etc

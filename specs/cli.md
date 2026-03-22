Command line tool that generates 

- `-h` show help
- subcommands:
  - `nest l` simple lex
  - `nest c` generate parser by complete syntax
- common options
  - `-t <target_triple>` specify target triple, if none given, probe clang's default triple

### lex

- calling: `nest l input_file.txt -o output_file.ll -m <mode_flag> -f <function_name> -t <target_triple>`
- input file format:
  - each line is a regexp, auto assigning action_id (starting from 1)

### parser

- invokes parse API to process.

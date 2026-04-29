# Background

Writing a grammar is not trivial. We can use AI to help.

# Visualizable Syntax Harness

Assume we have a folder with content

foo/
  spec.txt -- the language syntax spec, can be .txt, or .md
  example1.foo -- grammar example
  example2.foo -- grammar example
  ...

Create script `nest-compose`, when run with a folder `nest-compose foo`, it start a web server and a agent to compose the grammar.

# Initializing

It will first check what files are defined in the folder, read them and create prompt, loop the harness, print progress in stdout, and notify results to front-end.

The harness stops when agent called `break`, or all examples are parsed and no `TODO` rules remaining.

# The Web

Use vue from CDN.

The web page layout:

- top: current iteration (edit calls of the agent), context, with a stale counter to count last message from agent
- shows 2 columns,
  - the left 
    - current syntax, colored by bootstrap.nest syntax, scrollable
    - agent messages, scollable
  - the right
    - a list of examples, when click an example, show an example file below
    - example file, which is highlighted by token (assign rainbow colors to each token, color should not be very red).
      - when token error, make it red
      - when mouse over, show a tip of token / syntax element

In each loop, notify front-end.

# The Harness

Use `pi --mode rpc` for the harness. the prompt:

`````markdown
Nest is a parser generator. compose a syntax that parses examples with `nest`.

<%= File.read 'doc/nest_syntax.md' %>

# Nest Syntax in Nest

```nest
<%= File.read 'specs/bootstrap.nest' %>
```

<%= File.read 'doc/lex_tricks.md' %>

# The Syntax Definition

<%= File.read (Dir.glob "foo/spec.{md,txt}").first %>

# Examples

<% examples.each do |ex| %>
```<%= file extension of ex %>
<%= file content of ex %>
```
<% end %>

# Incremental Work

Focus on VPA lexing first, after lexer passed, for each scope, assign `= TODO` in `[[peg]]`. Then work on each scope, one by one.
`````

Provided tools:

- `read`: no args, show the whole current grammar and the parse result of each examples.
- `edit`: patch the grammar. after written, the harness compiles it (outputs to the input folder too) and returns compilation result.
- `break {reason}`: can't continue due to difficulties.

Context management:

- if prompt size exceeds 100K, show error and exit immediately.
- call `/compress` when context reaches 200K.

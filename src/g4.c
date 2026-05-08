// specs/g4.md — ANTLR .g4 to Nest grammar importer
// This file is the driver/converter around the Nest-generated parser.
// Recognition of .g4 syntax is performed by the grammar in src/g4.nest.

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "g4_grammar.h"
#pragma clang diagnostic pop

// --- Utility: dynamic string buffer ---

typedef struct {
  char* data;
  int32_t len;
  int32_t cap;
} Buf;

static void _buf_init(Buf* b) {
  b->cap = 256;
  b->data = malloc(b->cap);
  b->data[0] = '\0';
  b->len = 0;
}

static void _buf_append(Buf* b, const char* s, int32_t n) {
  if (b->len + n + 1 > b->cap) {
    while (b->len + n + 1 > b->cap) {
      b->cap *= 2;
    }
    b->data = realloc(b->data, b->cap);
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void _buf_puts(Buf* b, const char* s) { _buf_append(b, s, (int32_t)strlen(s)); }

static void _buf_putc(Buf* b, char c) { _buf_append(b, &c, 1); }

static void _buf_printf(Buf* b, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmp[2048];
  int32_t n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n > 0) {
    _buf_append(b, tmp, n);
  }
}

static void _buf_free(Buf* b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
}

// --- Utility: dynamic array of strings ---

typedef struct {
  char** items;
  int32_t len;
  int32_t cap;
} StrArr;

static void _strarr_init(StrArr* a) {
  a->items = NULL;
  a->len = 0;
  a->cap = 0;
}

static void _strarr_push(StrArr* a, const char* s) {
  if (a->len >= a->cap) {
    a->cap = a->cap ? a->cap * 2 : 8;
    a->items = realloc(a->items, sizeof(char*) * a->cap);
  }
  a->items[a->len++] = strdup(s);
}

static void _strarr_free(StrArr* a) {
  for (int32_t i = 0; i < a->len; i++) {
    free(a->items[i]);
  }
  free(a->items);
  a->items = NULL;
  a->len = 0;
  a->cap = 0;
}

static int32_t _strarr_find(StrArr* a, const char* s) {
  for (int32_t i = 0; i < a->len; i++) {
    if (strcmp(a->items[i], s) == 0) {
      return i;
    }
  }
  return -1;
}

// --- Name conversion utilities ---

// Convert UPPER_CASE or CamelCase to snake_case
static char* _to_snake_case(const char* name) {
  char buf[512];
  int32_t j = 0;
  for (int32_t i = 0; name[i] && j < 510; i++) {
    char c = name[i];
    if (c == '_') {
      buf[j++] = '_';
    } else if (isupper((unsigned char)c)) {
      if (i > 0 && (islower((unsigned char)name[i - 1]) ||
                    (isupper((unsigned char)name[i - 1]) && name[i + 1] && islower((unsigned char)name[i + 1])))) {
        buf[j++] = '_';
      }
      buf[j++] = (char)tolower((unsigned char)c);
    } else {
      buf[j++] = c;
    }
  }
  buf[j] = '\0';
  // Collapse double underscores
  char* result = malloc(j + 2);
  int32_t k = 0;
  for (int32_t i = 0; buf[i]; i++) {
    if (buf[i] == '_' && k > 0 && result[k - 1] == '_') {
      continue;
    }
    result[k++] = buf[i];
  }
  result[k] = '\0';
  return result;
}

// --- Data structures for collected grammar ---

typedef struct {
  char* name;       // ANTLR name (original)
  char* snake_name; // snake_case converted
  bool is_fragment;
  bool is_hidden;   // channel(HIDDEN)
  bool is_skipped;  // -> skip
  char* type_alias; // -> type(X), NULL if none
  char* push_mode;  // -> pushMode(X), NULL if none
  bool pop_mode;    // -> popMode
  char* mode_name;  // which mode this rule belongs to (NULL = DEFAULT_MODE)
  // Store alt_list ref for later body generation
  PegRef alt_list_ref;
  TokenTree* tt; // source token tree for this rule
} LexerRule;

typedef struct {
  char* name;       // ANTLR name (original)
  char* snake_name; // snake_case converted
  // Store alt_list ref for later body generation
  PegRef alt_list_ref;
  TokenTree* tt;         // source token tree for this rule
  int32_t paren_counter; // counter for generated helper rules
} ParserRule;

typedef struct {
  char* mode_name;
  char* parser_rule; // command-line mapping
} ModeMapping;

typedef struct {
  // Grammar metadata
  char* grammar_name;
  bool is_lexer;
  bool is_parser;
  bool is_combined;

  // Collected data
  LexerRule* lexer_rules;
  int32_t lexer_rule_count;
  int32_t lexer_rule_cap;

  ParserRule* parser_rules;
  int32_t parser_rule_count;
  int32_t parser_rule_cap;

  StrArr modes;     // non-default mode names
  StrArr fragments; // fragment names
  StrArr imports;   // imported grammar names

  // Command-line mappings
  ModeMapping* mode_maps;
  int32_t mode_map_count;
  int32_t mode_map_cap;

  // State
  char* current_mode;
  bool has_nongreedy;
} G4State;

static void _g4_state_init(G4State* st) {
  memset(st, 0, sizeof(*st));
  _strarr_init(&st->modes);
  _strarr_init(&st->fragments);
  _strarr_init(&st->imports);
}

static void _add_lexer_rule(G4State* st, LexerRule* r) {
  if (st->lexer_rule_count >= st->lexer_rule_cap) {
    st->lexer_rule_cap = st->lexer_rule_cap ? st->lexer_rule_cap * 2 : 32;
    st->lexer_rules = realloc(st->lexer_rules, sizeof(LexerRule) * st->lexer_rule_cap);
  }
  st->lexer_rules[st->lexer_rule_count++] = *r;
}

static void _add_parser_rule(G4State* st, ParserRule* r) {
  if (st->parser_rule_count >= st->parser_rule_cap) {
    st->parser_rule_cap = st->parser_rule_cap ? st->parser_rule_cap * 2 : 32;
    st->parser_rules = realloc(st->parser_rules, sizeof(ParserRule) * st->parser_rule_cap);
  }
  st->parser_rules[st->parser_rule_count++] = *r;
}

// --- Token text extraction ---

static char* _get_tok_text(TokenTree* tt, PegRef ref) {
  Token tok = ref.tc->tokens[ref.col];
  UstrByteSlice sl = ustr_slice_bytes(tt->src, tok.cp_start, tok.cp_start + tok.cp_size);
  char* s = malloc(sl.size + 1);
  memcpy(s, sl.s, sl.size);
  s[sl.size] = '\0';
  return s;
}

// --- Convert ANTLR string literal to Nest regex pattern ---
static char* _antlr_str_to_regex(const char* lit) {
  Buf b;
  _buf_init(&b);
  int32_t len = (int32_t)strlen(lit);
  // Strip surrounding quotes
  const char* s = lit + 1;
  const char* end = lit + len - 1;
  while (s < end) {
    if (*s == '\\') {
      s++;
      switch (*s) {
      case 'n':
        _buf_puts(&b, "\\n");
        break;
      case 'r':
        _buf_puts(&b, "\\r");
        break;
      case 't':
        _buf_puts(&b, "\\t");
        break;
      case '\\':
        _buf_puts(&b, "\\\\");
        break;
      case '\'':
        _buf_puts(&b, "'");
        break;
      case '"':
        _buf_puts(&b, "\\\"");
        break;
      case 'u': {
        _buf_puts(&b, "\\u{");
        s++;
        for (int32_t i = 0; i < 4 && s < end; i++, s++) {
          _buf_putc(&b, *s);
        }
        s--;
        _buf_putc(&b, '}');
        break;
      }
      default:
        _buf_putc(&b, '\\');
        _buf_putc(&b, *s);
        break;
      }
    } else {
      char c = *s;
      if (c == '/' || c == '.' || c == '*' || c == '+' || c == '?' || c == '(' || c == ')' || c == '[' || c == ']' ||
          c == '{' || c == '}' || c == '^' || c == '$' || c == '|' || c == '\\') {
        _buf_putc(&b, '\\');
      }
      _buf_putc(&b, c);
    }
    s++;
  }
  char* result = strdup(b.data);
  _buf_free(&b);
  return result;
}

// Convert ANTLR string literal to Nest PEG double-quoted string
static char* _antlr_str_to_peg(const char* lit) {
  Buf b;
  _buf_init(&b);
  _buf_putc(&b, '"');
  int32_t len = (int32_t)strlen(lit);
  const char* s = lit + 1;
  const char* end = lit + len - 1;
  while (s < end) {
    if (*s == '\\') {
      s++;
      switch (*s) {
      case 'n':
        _buf_puts(&b, "\\n");
        break;
      case 'r':
        _buf_puts(&b, "\\r");
        break;
      case 't':
        _buf_puts(&b, "\\t");
        break;
      case '\\':
        _buf_puts(&b, "\\\\");
        break;
      case '\'':
        _buf_puts(&b, "'");
        break;
      case '"':
        _buf_puts(&b, "\\\"");
        break;
      default:
        _buf_putc(&b, '\\');
        _buf_putc(&b, *s);
        break;
      }
    } else {
      char c = *s;
      if (c == '"') {
        _buf_puts(&b, "\\\"");
      } else {
        _buf_putc(&b, c);
      }
    }
    s++;
  }
  _buf_putc(&b, '"');
  char* result = strdup(b.data);
  _buf_free(&b);
  return result;
}

// Convert ANTLR charset to Nest regex charset (handle \uXXXX -> \u{XXXX})
static char* _antlr_charset_to_nest(const char* cs) {
  Buf b;
  _buf_init(&b);
  int32_t len = (int32_t)strlen(cs);
  for (int32_t i = 0; i < len; i++) {
    if (cs[i] == '\\' && i + 1 < len && cs[i + 1] == 'u') {
      _buf_puts(&b, "\\u{");
      i += 2;
      for (int32_t j = 0; j < 4 && i < len && isxdigit((unsigned char)cs[i]); j++, i++) {
        _buf_putc(&b, cs[i]);
      }
      i--;
      _buf_putc(&b, '}');
    } else {
      _buf_putc(&b, cs[i]);
    }
  }
  char* result = strdup(b.data);
  _buf_free(&b);
  return result;
}

// --- Tree walking: convert alt_list to string representation ---

static void _walk_alt_list(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out);
static void _walk_alt(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out);
static void _walk_element(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out);
static void _walk_atom(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out, int32_t* paren_counter,
                       const char* rule_snake_name);

static void _walk_alt_list(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out) {
  if (g4_grammar_peg_size(ref) <= 0) {
    return;
  }
  Node_alt_list node = g4_grammar_load_alt_list(ref);
  int32_t alt_count = 0;
  for (PegLink link = node.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    if (alt_count > 0) {
      if (is_lexer) {
        _buf_puts(out, " | ");
      } else {
        _buf_puts(out, "\n  ");
      }
    }
    PegRef alt_ref = g4_grammar_get_lhs(&link);
    _walk_alt(st, tt, alt_ref, is_lexer, out);
    alt_count++;
  }
}

static void _walk_alt(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out) {
  if (g4_grammar_peg_size(ref) <= 0) {
    return;
  }
  Node_alt node = g4_grammar_load_alt(ref);
  int32_t elem_count = 0;
  for (PegLink link = node.labeled_element; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef le_ref = g4_grammar_get_lhs(&link);
    Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
    if (!is_lexer && elem_count > 0) {
      _buf_putc(out, ' ');
    }
    _walk_element(st, tt, le.element, is_lexer, out);
    elem_count++;
  }
}

static void _walk_element(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out) {
  if (g4_grammar_peg_size(ref) <= 0) {
    return;
  }
  Node_element node = g4_grammar_load_element(ref);
  _walk_atom(st, tt, node.atom, is_lexer, out, NULL, NULL);

  // Handle EBNF suffix
  if (g4_grammar_has_elem(&node.ebnf_suffix)) {
    Node_ebnf_suffix suffix = g4_grammar_load_ebnf_suffix(g4_grammar_get_lhs(&node.ebnf_suffix));
    if (suffix.is.maybe) {
      _buf_putc(out, '?');
      if (g4_grammar_has_elem(&suffix._question$1)) {
        st->has_nongreedy = true;
      }
    } else if (suffix.is.star_suffix) {
      _buf_putc(out, '*');
      if (g4_grammar_has_elem(&suffix._question$2)) {
        st->has_nongreedy = true;
      }
    } else if (suffix.is.plus_suffix) {
      _buf_putc(out, '+');
      if (g4_grammar_has_elem(&suffix._question$3)) {
        st->has_nongreedy = true;
      }
    }
  }
}

static void _walk_atom(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out, int32_t* paren_counter,
                       const char* rule_snake_name) {
  (void)paren_counter;
  (void)rule_snake_name;
  if (g4_grammar_peg_size(ref) <= 0) {
    return;
  }
  Node_atom node = g4_grammar_load_atom(ref);

  if (node.is.string_atom) {
    char* lit = _get_tok_text(tt, node._string_lit$2);
    if (is_lexer) {
      char* regex = _antlr_str_to_regex(lit);
      _buf_puts(out, regex);
      free(regex);
    } else {
      char* peg_str = _antlr_str_to_peg(lit);
      _buf_puts(out, peg_str);
      free(peg_str);
    }
    free(lit);
  } else if (node.is.range_atom) {
    char* from_lit = _get_tok_text(tt, node._string_lit);
    char* to_lit = _get_tok_text(tt, node._string_lit$1);
    // Convert 'a'..'z' to [a-z] charset
    char* from_regex = _antlr_str_to_regex(from_lit);
    char* to_regex = _antlr_str_to_regex(to_lit);
    _buf_printf(out, "[%s-%s]", from_regex, to_regex);
    free(from_regex);
    free(to_regex);
    free(from_lit);
    free(to_lit);
  } else if (node.is.charset_atom) {
    char* cs = _get_tok_text(tt, node._charset);
    char* nest_cs = _antlr_charset_to_nest(cs);
    _buf_puts(out, nest_cs);
    free(nest_cs);
    free(cs);
  } else if (node.is.dot_atom) {
    _buf_putc(out, '.');
  } else if (node.is.not_atom) {
    _buf_putc(out, '~');
    // Walk not_atom_inner
    if (g4_grammar_peg_size(node.not_atom_inner) > 0) {
      Node_not_atom_inner ni = g4_grammar_load_not_atom_inner(node.not_atom_inner);
      if (ni.is._string_lit) {
        char* lit = _get_tok_text(tt, ni._string_lit);
        char* regex = _antlr_str_to_regex(lit);
        _buf_puts(out, regex);
        free(regex);
        free(lit);
      } else if (ni.is._charset) {
        char* cs = _get_tok_text(tt, ni._charset);
        char* nest_cs = _antlr_charset_to_nest(cs);
        _buf_puts(out, nest_cs);
        free(nest_cs);
        free(cs);
      } else if (ni.is._upper_id) {
        char* name = _get_tok_text(tt, ni._upper_id);
        char* snake = _to_snake_case(name);
        _buf_printf(out, "{%s}", snake);
        free(snake);
        free(name);
      } else if (ni.is._lower_id) {
        char* name = _get_tok_text(tt, ni._lower_id);
        char* snake = _to_snake_case(name);
        _buf_puts(out, snake);
        free(snake);
        free(name);
      } else if (ni.is._lparen) {
        _buf_putc(out, '(');
        _walk_alt_list(st, tt, ni.alt_list, is_lexer, out);
        _buf_putc(out, ')');
      }
    }
  } else if (node.is.upper_ref) {
    char* name = _get_tok_text(tt, node._upper_id);
    if (is_lexer) {
      // In lexer context, uppercase refs are fragment/token references
      char* snake = _to_snake_case(name);
      _buf_printf(out, "{%s}", snake);
      free(snake);
    } else {
      // In parser context, uppercase refs are token references
      char* snake = _to_snake_case(name);
      _buf_printf(out, "@%s", snake);
      free(snake);
    }
    free(name);
  } else if (node.is.lower_ref) {
    char* name = _get_tok_text(tt, node._lower_id);
    char* snake = _to_snake_case(name);
    _buf_puts(out, snake);
    free(snake);
    free(name);
  } else if (node.is.group_atom) {
    // Parenthesized subexpression
    if (is_lexer) {
      _buf_putc(out, '(');
      _walk_alt_list(st, tt, node.alt_list, is_lexer, out);
      _buf_putc(out, ')');
    } else {
      // In parser context, emit as inline branch block
      // Count alternatives
      Node_alt_list al = g4_grammar_load_alt_list(node.alt_list);
      int32_t count = 0;
      PegLink cnt = al.alt;
      for (; g4_grammar_has_elem(&cnt); g4_grammar_get_next(&cnt)) {
        count++;
      }
      if (count > 1) {
        _buf_puts(out, "[");
        for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
          _buf_putc(out, ' ');
          PegRef alt_ref = g4_grammar_get_lhs(&link);
          _walk_alt(st, tt, alt_ref, is_lexer, out);
        }
        _buf_puts(out, " ]");
      } else {
        _walk_alt_list(st, tt, node.alt_list, is_lexer, out);
      }
    }
  } else if (node.is.action_atom) {
    // Semantic actions and predicates are dropped
  } else if (node.is.eof_atom) {
    if (!is_lexer) {
      // Drop EOF in parser context per spec
    } else {
      _buf_puts(out, "EOF");
    }
  }
}

// --- Walk a rule definition and collect it ---

static void _walk_rule_def(G4State* st, TokenTree* tt, PegRef ref) {
  if (g4_grammar_peg_size(ref) <= 0) {
    return;
  }
  Node_rule_def node = g4_grammar_load_rule_def(ref);
  Node_rule_head head = g4_grammar_load_rule_head(node.rule_head);

  bool is_fragment = head.is.fragment_head;
  bool is_lexer_rule = head.is.lexer_head || head.is.fragment_head;

  char* rule_name = NULL;
  if (head.is.fragment_head) {
    rule_name = _get_tok_text(tt, head._upper_id);
  } else if (head.is.lexer_head) {
    rule_name = _get_tok_text(tt, head._upper_id$1);
  } else if (head.is.parser_head) {
    rule_name = _get_tok_text(tt, head._lower_id);
  }
  if (!rule_name) {
    return;
  }

  if (is_lexer_rule) {
    LexerRule lr = {0};
    lr.name = rule_name;
    lr.snake_name = _to_snake_case(rule_name);
    lr.is_fragment = is_fragment;
    lr.mode_name = st->current_mode ? strdup(st->current_mode) : NULL;
    lr.alt_list_ref = node.alt_list;
    lr.tt = tt;

    // Process lexer commands
    if (g4_grammar_has_elem(&node.lexer_commands)) {
      PegRef cmds_ref = g4_grammar_get_lhs(&node.lexer_commands);
      Node_lexer_commands cmds = g4_grammar_load_lexer_commands(cmds_ref);
      for (PegLink link = cmds.lexer_command; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
        PegRef cmd_ref = g4_grammar_get_lhs(&link);
        Node_lexer_command cmd = g4_grammar_load_lexer_command(cmd_ref);
        if (cmd.is.cmd_with_arg) {
          char* cmd_name = _get_tok_text(tt, cmd._lower_id);
          Node_channel_arg ca = g4_grammar_load_channel_arg(cmd.channel_arg);
          char* arg = NULL;
          if (ca.is._upper_id) {
            arg = _get_tok_text(tt, ca._upper_id);
          } else if (ca.is._lower_id) {
            arg = _get_tok_text(tt, ca._lower_id);
          }
          if (strcmp(cmd_name, "channel") == 0) {
            if (arg && (strcmp(arg, "HIDDEN") == 0 || strcmp(arg, "hidden") == 0)) {
              lr.is_hidden = true;
            }
          } else if (strcmp(cmd_name, "type") == 0) {
            lr.type_alias = arg ? strdup(arg) : NULL;
          } else if (strcmp(cmd_name, "pushMode") == 0) {
            lr.push_mode = arg ? strdup(arg) : NULL;
          } else if (strcmp(cmd_name, "mode") == 0) {
            lr.push_mode = arg ? strdup(arg) : NULL;
          }
          free(cmd_name);
          free(arg);
        } else if (cmd.is.cmd_simple) {
          char* cmd_name = _get_tok_text(tt, cmd._lower_id$1);
          if (strcmp(cmd_name, "skip") == 0) {
            lr.is_skipped = true;
          } else if (strcmp(cmd_name, "popMode") == 0) {
            lr.pop_mode = true;
          }
          free(cmd_name);
        }
      }
    }

    if (is_fragment) {
      _strarr_push(&st->fragments, rule_name);
    }
    _add_lexer_rule(st, &lr);
  } else {
    // Parser rule
    ParserRule pr = {0};
    pr.name = rule_name;
    pr.snake_name = _to_snake_case(rule_name);
    pr.alt_list_ref = node.alt_list;
    pr.tt = tt;
    pr.paren_counter = 0;
    _add_parser_rule(st, &pr);
  }
}

// --- Process a single .g4 file ---

// Keep parsed results alive for the duration of the program
typedef struct {
  ParseResult* results;
  char** sources;
  int32_t count;
  int32_t cap;
} ParsedFiles;

static ParsedFiles g_parsed = {0};

static void _process_file(G4State* st, const char* filename) {
  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    fprintf(stderr, "error: cannot open '%s'\n", filename);
    exit(1);
  }
  char* src = ustr_from_file(fp);
  fclose(fp);

  ParseContext ctx = {.source_file_name = filename};
  ParseResult res = g4_grammar_parse(&ctx, src);
  TokenTree* tt = res.tt;

  if (g4_grammar_peg_size(res.main) <= 0) {
    fprintf(stderr, "error: parse error in '%s'\n", filename);
    g4_grammar_cleanup(&res);
    ustr_del(src);
    exit(1);
  }

  // Store parsed result to keep alive
  if (g_parsed.count >= g_parsed.cap) {
    g_parsed.cap = g_parsed.cap ? g_parsed.cap * 2 : 4;
    g_parsed.results = realloc(g_parsed.results, sizeof(ParseResult) * g_parsed.cap);
    g_parsed.sources = realloc(g_parsed.sources, sizeof(char*) * g_parsed.cap);
  }
  g_parsed.results[g_parsed.count] = res;
  g_parsed.sources[g_parsed.count] = src;
  g_parsed.count++;

  // Walk the parse tree
  Node_main main_node = g4_grammar_load_main(res.main);

  // Get grammar type
  Node_grammar_decl gdecl = g4_grammar_load_grammar_decl(main_node.grammar_decl);
  Node_grammar_type gtype = g4_grammar_load_grammar_type(gdecl.grammar_type);

  if (gtype.is.lexer_type) {
    st->is_lexer = true;
  } else if (gtype.is.parser_type) {
    st->is_parser = true;
  } else {
    st->is_combined = true;
  }

  if (!st->grammar_name) {
    st->grammar_name = _get_tok_text(tt, gdecl._upper_id);
  }

  // Walk body items
  for (PegLink link = main_node.body_item; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef item_ref = g4_grammar_get_lhs(&link);
    Node_body_item item = g4_grammar_load_body_item(item_ref);

    if (item.is.mode_decl) {
      Node_mode_decl md = g4_grammar_load_mode_decl(item.mode_decl);
      char* mode_name = _get_tok_text(tt, md._upper_id);
      if (strcmp(mode_name, "DEFAULT_MODE") != 0) {
        if (_strarr_find(&st->modes, mode_name) < 0) {
          _strarr_push(&st->modes, mode_name);
        }
      }
      free(st->current_mode);
      st->current_mode = mode_name;
    } else if (item.is.rule_def) {
      _walk_rule_def(st, tt, item.rule_def);
    } else if (item.is.import_decl) {
      Node_import_decl id = g4_grammar_load_import_decl(item.import_decl);
      for (PegLink ilink = id.import_name; g4_grammar_has_elem(&ilink); g4_grammar_get_next(&ilink)) {
        PegRef iref = g4_grammar_get_lhs(&ilink);
        Node_import_name in = g4_grammar_load_import_name(iref);
        char* name = _get_tok_text(tt, in._upper_id);
        _strarr_push(&st->imports, name);
        free(name);
      }
    }
    // options_decl, channels_decl, tokens_decl are accepted but don't affect Nest output
  }
}

// --- Nest output generation ---

static void _emit_lexer_rule_body(G4State* st, LexerRule* r, Buf* out) {
  _walk_alt_list(st, r->tt, r->alt_list_ref, true, out);
}

static void _emit_parser_rule_body(G4State* st, ParserRule* r, Buf* out) {
  if (g4_grammar_peg_size(r->alt_list_ref) <= 0) {
    return;
  }
  Node_alt_list node = g4_grammar_load_alt_list(r->alt_list_ref);

  // Count alternatives
  int32_t alt_count = 0;
  PegLink counting = node.alt;
  for (; g4_grammar_has_elem(&counting); g4_grammar_get_next(&counting)) {
    alt_count++;
  }

  if (alt_count == 1) {
    // Single alternative — emit as sequence
    PegLink link = node.alt;
    PegRef alt_ref = g4_grammar_get_lhs(&link);
    _walk_alt(st, r->tt, alt_ref, false, out);
  } else {
    // Multiple alternatives — emit as branch block
    _buf_puts(out, "[\n");
    for (PegLink link = node.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
      PegRef alt_ref = g4_grammar_get_lhs(&link);
      _buf_puts(out, "  ");
      _walk_alt(st, r->tt, alt_ref, false, out);
      _buf_putc(out, '\n');
    }
    _buf_putc(out, ']');
  }
}

static void _emit_nest_output(G4State* st, FILE* outfp) {
  Buf out;
  _buf_init(&out);

  // Collect ignored tokens
  StrArr ignore_tokens;
  _strarr_init(&ignore_tokens);
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (r->is_fragment || r->type_alias) {
      continue;
    }
    if (r->is_hidden || r->is_skipped) {
      char tok_name[256];
      snprintf(tok_name, sizeof(tok_name), "@%s", r->snake_name);
      if (_strarr_find(&ignore_tokens, tok_name) < 0) {
        _strarr_push(&ignore_tokens, tok_name);
      }
    }
  }

  // --- VPA Section ---
  _buf_puts(&out, "[[vpa]]\n\n");

  // %ignore
  if (ignore_tokens.len > 0) {
    _buf_puts(&out, "%ignore");
    for (int32_t i = 0; i < ignore_tokens.len; i++) {
      _buf_printf(&out, " %s", ignore_tokens.items[i]);
    }
    _buf_puts(&out, "\n\n");
  }

  // Main scope wraps default and EOF
  _buf_puts(&out, "main = {\n");

  // Emit default mode rules
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (r->is_fragment) {
      continue;
    }
    const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
    if (strcmp(rule_mode, "DEFAULT_MODE") != 0) {
      continue;
    }
    if (r->type_alias) {
      continue;
    }

    Buf body;
    _buf_init(&body);
    _emit_lexer_rule_body(st, r, &body);
    _buf_printf(&out, "  /%s/ @%s\n", body.data, r->snake_name);
    _buf_free(&body);
  }

  _buf_puts(&out, "  EOF .end\n");
  _buf_puts(&out, "}\n");

  // Emit non-default mode scopes
  for (int32_t m = 0; m < st->modes.len; m++) {
    const char* mode = st->modes.items[m];
    char* scope_name = _to_snake_case(mode);

    // Find the rule that pushes into this mode to determine the begin pattern
    _buf_printf(&out, "\n# Mode: %s\n", mode);

    // Find which rule triggers this mode (pushMode) to determine begin/end
    bool emitted_scope_begin = false;
    for (int32_t i = 0; i < st->lexer_rule_count; i++) {
      LexerRule* r = &st->lexer_rules[i];
      if (r->push_mode && strcmp(r->push_mode, mode) == 0) {
        Buf body;
        _buf_init(&body);
        _emit_lexer_rule_body(st, r, &body);
        _buf_printf(&out, "%s = /%s/ .begin @%s {\n", scope_name, body.data, r->snake_name);
        _buf_free(&body);
        emitted_scope_begin = true;
        break;
      }
    }
    if (!emitted_scope_begin) {
      _buf_printf(&out, "# %s scope (no pushMode trigger found)\n", scope_name);
      free(scope_name);
      continue;
    }

    // Emit mode rules
    for (int32_t i = 0; i < st->lexer_rule_count; i++) {
      LexerRule* r = &st->lexer_rules[i];
      if (r->is_fragment) {
        continue;
      }
      const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
      if (strcmp(rule_mode, mode) != 0) {
        continue;
      }
      if (r->type_alias) {
        continue;
      }

      Buf body;
      _buf_init(&body);
      _emit_lexer_rule_body(st, r, &body);

      if (r->pop_mode) {
        _buf_printf(&out, "  /%s/ @%s .end\n", body.data, r->snake_name);
      } else {
        _buf_printf(&out, "  /%s/ @%s\n", body.data, r->snake_name);
      }
      _buf_free(&body);
    }

    _buf_puts(&out, "}\n");
    free(scope_name);
  }

  // --- PEG Section ---
  _buf_puts(&out, "\n[[peg]]\n\n");

  // Determine the entrance rule
  if (st->parser_rule_count > 0) {
    bool has_main = false;
    for (int32_t i = 0; i < st->parser_rule_count; i++) {
      if (strcmp(st->parser_rules[i].snake_name, "main") == 0) {
        has_main = true;
        break;
      }
    }
    if (!has_main) {
      _buf_printf(&out, "main = %s\n", st->parser_rules[0].snake_name);
    }
  }

  // Emit parser rules
  for (int32_t i = 0; i < st->parser_rule_count; i++) {
    ParserRule* r = &st->parser_rules[i];
    Buf body;
    _buf_init(&body);
    _emit_parser_rule_body(st, r, &body);
    _buf_printf(&out, "%s = %s\n", r->snake_name, body.data);
    _buf_free(&body);
  }

  // Write output
  fprintf(outfp, "%s", out.data);
  _buf_free(&out);
  _strarr_free(&ignore_tokens);
}

// --- Main ---

static void _usage(const char* prog) {
  fprintf(stderr, "usage: %s <input.g4>... -o <output.nest> [MODE=parserRule ...]\n", prog);
  exit(1);
}

int32_t g4_grammar_next_cp(void* userdata) { return ustr_iter_next((UstrIter*)userdata); }

int main(int argc, char** argv) {
  if (argc < 4) {
    _usage(argv[0]);
  }

  // Parse arguments
  StrArr input_files;
  _strarr_init(&input_files);
  const char* output_file = NULL;
  StrArr mode_mappings;
  _strarr_init(&mode_mappings);

  for (int32_t i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0) {
      if (i + 1 >= argc) {
        _usage(argv[0]);
      }
      output_file = argv[++i];
    } else if (strchr(argv[i], '=') && argv[i][0] != '-' && argv[i][0] != '/') {
      // MODE=parserRule or LEXER_RULE=parserRule mapping
      _strarr_push(&mode_mappings, argv[i]);
    } else {
      _strarr_push(&input_files, argv[i]);
    }
  }

  if (!output_file || input_files.len == 0) {
    _usage(argv[0]);
  }

  // Initialize state
  G4State st;
  _g4_state_init(&st);

  // Parse mode mappings
  for (int32_t i = 0; i < mode_mappings.len; i++) {
    char* mapping = strdup(mode_mappings.items[i]);
    char* eq = strchr(mapping, '=');
    *eq = '\0';
    ModeMapping mm = {.mode_name = strdup(mapping), .parser_rule = strdup(eq + 1)};
    if (st.mode_map_count >= st.mode_map_cap) {
      st.mode_map_cap = st.mode_map_cap ? st.mode_map_cap * 2 : 8;
      st.mode_maps = realloc(st.mode_maps, sizeof(ModeMapping) * st.mode_map_cap);
    }
    st.mode_maps[st.mode_map_count++] = mm;
    free(mapping);
  }

  // Process all input files
  for (int32_t i = 0; i < input_files.len; i++) {
    free(st.current_mode);
    st.current_mode = NULL;
    _process_file(&st, input_files.items[i]);
  }

  // Validate: check all non-default modes have mappings
  for (int32_t i = 0; i < st.modes.len; i++) {
    const char* mode = st.modes.items[i];
    bool found = false;
    for (int32_t j = 0; j < st.mode_map_count; j++) {
      if (strcmp(st.mode_maps[j].mode_name, mode) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "error: non-default mode '%s' has no command-line mapping\n", mode);
      fprintf(stderr, "  add: %s=<parserRule>\n", mode);
      exit(1);
    }
  }

  // Write output
  FILE* outfp = fopen(output_file, "w");
  if (!outfp) {
    fprintf(stderr, "error: cannot open output file '%s'\n", output_file);
    exit(1);
  }

  _emit_nest_output(&st, outfp);
  fclose(outfp);

  // Print warnings
  if (st.has_nongreedy) {
    fprintf(stderr, "warning: generated output contains non-greedy approximations and must be reviewed/edited manually "
                    "after generation\n");
  }

  printf("generated %s from %d file(s)\n", output_file, input_files.len);

  // Cleanup
  for (int32_t i = 0; i < g_parsed.count; i++) {
    g4_grammar_cleanup(&g_parsed.results[i]);
    ustr_del(g_parsed.sources[i]);
  }
  free(g_parsed.results);
  free(g_parsed.sources);

  _strarr_free(&input_files);
  _strarr_free(&mode_mappings);

  return 0;
}

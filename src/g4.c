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
  va_list ap2;
  va_copy(ap2, ap);
  int32_t n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n > 0) {
    char* tmp = malloc(n + 1);
    vsnprintf(tmp, n + 1, fmt, ap2);
    _buf_append(b, tmp, n);
    free(tmp);
  }
  va_end(ap2);
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
  bool is_recursive; // references itself (directly or transitively)
  bool needs_scope;  // references a recursive lexer rule and cannot be a pure regexp VPA line
  bool needs_scope_traced;
  bool from_lexer_grammar;
  int32_t lexer_grammar_index;
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

// Helper rule generated for parenthesized sub-expressions that need
// their own rule in nest (e.g. single-alt groups with suffix)
typedef struct {
  char* name; // generated name like "rule_name$1"
  char* body; // already-emitted body text
} HelperRule;

typedef struct {
  HelperRule* items;
  int32_t count;
  int32_t cap;
} HelperRules;

typedef struct {
  char* mode_name;
  char* parser_rule; // command-line mapping
} ModeMapping;

typedef struct {
  // Grammar metadata
  char* grammar_name;
  char* command_line;
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
  bool suppress_nongreedy_warning;
  int32_t lexer_grammar_count;
  bool current_file_is_lexer_grammar;
  int32_t current_lexer_grammar_index;
  StrArr parser_tokens;

  // Helper rules generated during emission
  HelperRules helpers;
  char* current_rule_snake; // name of rule currently being emitted (for helper naming)
  int32_t current_rule_paren_counter;
} G4State;

static void _g4_state_init(G4State* st) {
  memset(st, 0, sizeof(*st));
  _strarr_init(&st->modes);
  _strarr_init(&st->fragments);
  _strarr_init(&st->imports);
  _strarr_init(&st->parser_tokens);
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

static void _append_char_class_content(Buf* out, const char* regex_atom) {
  int32_t len = (int32_t)strlen(regex_atom);
  if (len >= 2 && regex_atom[0] == '[' && regex_atom[len - 1] == ']') {
    int32_t start = (len >= 3 && regex_atom[1] == '^') ? 2 : 1;
    _buf_append(out, regex_atom + start, len - start - 1);
  } else {
    _buf_puts(out, regex_atom);
  }
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
    } else if (cs[i] == '-' && (i == 1 || (i == 2 && cs[1] == '^') || i + 1 == len - 1)) {
      _buf_puts(&b, "\\-");
    } else {
      _buf_putc(&b, cs[i]);
    }
  }
  char* result = strdup(b.data);
  _buf_free(&b);
  return result;
}

// --- Tree walking: convert alt_list to string representation ---

static LexerRule* _find_lexer_rule(G4State* st, const char* name);
static bool _parser_uses_token(G4State* st, const char* token_snake);
static char* _edge_name(const char* caller, const char* callee);
static const char* _find_mapping(G4State* st, const char* name);
static bool _mode_is_flat(G4State* st, const char* mode);
static bool _is_entrance_lexer_rule(G4State* st, LexerRule* r);

static void _walk_alt_list(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out);
static void _walk_alt(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out);
static void _walk_element(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out);
static void _walk_atom(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out, int32_t* paren_counter,
                       const char* rule_snake_name, bool has_suffix);
static bool _action_contains_pop_mode(TokenTree* tt, PegRef brace_block_ref);
static void _collect_parser_tokens_from_alt_list(G4State* st, TokenTree* tt, PegRef ref);

static bool _parser_uses_token(G4State* st, const char* token_snake) {
  return _strarr_find(&st->parser_tokens, token_snake) >= 0;
}

static const char* _edge_parser_rule(G4State* st, const char* caller, const char* callee) {
  char key[512];
  snprintf(key, sizeof(key), "%s:%s", caller, callee);
  const char* mapped = _find_mapping(st, key);
  if (mapped && mapped[0] != '\0') return mapped;
  mapped = _find_mapping(st, callee);
  if (mapped && mapped[0] != '\0') return mapped;
  int32_t callee_len = (int32_t)strlen(callee);
  for (int32_t i = 0; i < st->mode_map_count; i++) {
    if (st->mode_maps[i].parser_rule[0] == '\0') continue;
    int32_t key_len = (int32_t)strlen(st->mode_maps[i].mode_name);
    if (key_len > callee_len + 1 && st->mode_maps[i].mode_name[key_len - callee_len - 1] == ':' &&
        strcmp(st->mode_maps[i].mode_name + key_len - callee_len, callee) == 0) {
      return st->mode_maps[i].parser_rule;
    }
  }
  return NULL;
}

static void _edge_wrappers_for_parser_rule(G4State* st, const char* parser_rule_snake, StrArr* wrappers) {
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r) || !r->push_mode || _mode_is_flat(st, r->push_mode)) continue;
    const char* caller = r->mode_name ? r->mode_name : "DEFAULT_MODE";
    if (_mode_is_flat(st, caller)) continue;
    if (strcmp(caller, "DEFAULT_MODE") == 0 && strcmp(r->push_mode, "DEFAULT_MODE") == 0) continue;
    const char* mapped = _edge_parser_rule(st, caller, r->push_mode);
    if (!mapped) continue;
    char* mapped_snake = _to_snake_case(mapped);
    bool matches = strcmp(mapped_snake, parser_rule_snake) == 0;
    free(mapped_snake);
    if (!matches) continue;
    char* edge = _edge_name(caller, r->push_mode);
    if (_strarr_find(wrappers, edge) < 0) {
      _strarr_push(wrappers, edge);
    }
    free(edge);
  }
}

static void _collect_parser_tokens_from_atom(G4State* st, TokenTree* tt, PegRef ref) {
  if (g4_grammar_peg_size(ref) <= 0) return;
  Node_atom node = g4_grammar_load_atom(ref);
  if (node.is.upper_ref) {
    char* name = _get_tok_text(tt, node._upper_id);
    char* snake = _to_snake_case(name);
    if (_strarr_find(&st->parser_tokens, snake) < 0) {
      _strarr_push(&st->parser_tokens, snake);
    }
    free(snake);
    free(name);
  } else if (node.is.group_atom) {
    _collect_parser_tokens_from_alt_list(st, tt, node.alt_list);
  }
}

static void _collect_parser_tokens_from_alt(G4State* st, TokenTree* tt, PegRef ref) {
  if (g4_grammar_peg_size(ref) <= 0) return;
  Node_alt alt = g4_grammar_load_alt(ref);
  for (PegLink link = alt.labeled_element; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef le_ref = g4_grammar_get_lhs(&link);
    Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
    Node_element el = g4_grammar_load_element(le.element);
    _collect_parser_tokens_from_atom(st, tt, el.atom);
  }
}

static void _collect_parser_tokens_from_alt_list(G4State* st, TokenTree* tt, PegRef ref) {
  if (g4_grammar_peg_size(ref) <= 0) return;
  Node_alt_list al = g4_grammar_load_alt_list(ref);
  for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    _collect_parser_tokens_from_alt(st, tt, g4_grammar_get_lhs(&link));
  }
}

static void _walk_alt_list(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out) {
  if (g4_grammar_peg_size(ref) <= 0) {
    return;
  }
  Node_alt_list node = g4_grammar_load_alt_list(ref);
  int32_t alt_count = 0;
  for (PegLink link = node.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    if (alt_count > 0) {
      if (is_lexer) {
        _buf_putc(out, '|');
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
  if (!is_lexer) {
    Node_alt check_node = g4_grammar_load_alt(ref);
    PegLink check_link = check_node.labeled_element;
    if (g4_grammar_has_elem(&check_link)) {
      PegRef le_ref = g4_grammar_get_lhs(&check_link);
      g4_grammar_get_next(&check_link);
      if (!g4_grammar_has_elem(&check_link)) {
        Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
        Node_element el = g4_grammar_load_element(le.element);
        if (!g4_grammar_has_elem(&el.ebnf_suffix)) {
          Node_atom atom = g4_grammar_load_atom(el.atom);
          if (atom.is.lower_ref) {
            char* name = _get_tok_text(tt, atom._lower_id);
            char* snake = _to_snake_case(name);
            if (!st->current_rule_snake || strcmp(st->current_rule_snake, snake) != 0) {
              StrArr wrappers;
              _strarr_init(&wrappers);
              _edge_wrappers_for_parser_rule(st, snake, &wrappers);
              if (wrappers.len > 0) {
                if (wrappers.len == 1) {
                  _buf_puts(out, wrappers.items[0]);
                } else {
                  _buf_puts(out, snake);
                }
                _strarr_free(&wrappers);
                free(snake);
                free(name);
                return;
              }
              _strarr_free(&wrappers);
            }
            free(snake);
            free(name);
          }
        }
      }
    }
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
  bool has_suffix = g4_grammar_has_elem(&node.ebnf_suffix);
  _walk_atom(st, tt, node.atom, is_lexer, out, is_lexer ? NULL : &st->current_rule_paren_counter,
             st->current_rule_snake, has_suffix);

  // Handle EBNF suffix
  if (has_suffix) {
    Node_ebnf_suffix suffix = g4_grammar_load_ebnf_suffix(g4_grammar_get_lhs(&node.ebnf_suffix));
    if (suffix.is.maybe) {
      _buf_putc(out, '?');
      if (g4_grammar_has_elem(&suffix._question$1) && !st->suppress_nongreedy_warning) {
        st->has_nongreedy = true;
      }
    } else if (suffix.is.star_suffix) {
      _buf_putc(out, '*');
      if (g4_grammar_has_elem(&suffix._question$2) && !st->suppress_nongreedy_warning) {
        st->has_nongreedy = true;
      }
    } else if (suffix.is.plus_suffix) {
      _buf_putc(out, '+');
      if (g4_grammar_has_elem(&suffix._question$3) && !st->suppress_nongreedy_warning) {
        st->has_nongreedy = true;
      }
    }
  }
}

static void _walk_atom(G4State* st, TokenTree* tt, PegRef ref, bool is_lexer, Buf* out, int32_t* paren_counter,
                       const char* rule_snake_name, bool has_suffix) {
  (void)has_suffix;
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
    _buf_puts(out, "[^");
    // Walk not_atom_inner
    if (g4_grammar_peg_size(node.not_atom_inner) > 0) {
      Node_not_atom_inner ni = g4_grammar_load_not_atom_inner(node.not_atom_inner);
      if (ni.is._string_lit) {
        char* lit = _get_tok_text(tt, ni._string_lit);
        char* regex = _antlr_str_to_regex(lit);
        _append_char_class_content(out, regex);
        free(regex);
        free(lit);
      } else if (ni.is._charset) {
        char* cs = _get_tok_text(tt, ni._charset);
        char* nest_cs = _antlr_charset_to_nest(cs);
        _append_char_class_content(out, nest_cs);
        free(nest_cs);
        free(cs);
      } else if (ni.is._upper_id) {
        char* name = _get_tok_text(tt, ni._upper_id);
        _buf_printf(out, "#{%s}", name);
        free(name);
      } else if (ni.is._lower_id) {
        char* name = _get_tok_text(tt, ni._lower_id);
        char* snake = _to_snake_case(name);
        _buf_puts(out, snake);
        free(snake);
        free(name);
      } else if (ni.is._lparen) {
        Node_alt_list al = g4_grammar_load_alt_list(ni.alt_list);
        for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
          PegRef alt_ref = g4_grammar_get_lhs(&link);
          Buf alt_buf;
          _buf_init(&alt_buf);
          _walk_alt(st, tt, alt_ref, is_lexer, &alt_buf);
          _append_char_class_content(out, alt_buf.data);
          _buf_free(&alt_buf);
        }
      }
    }
    _buf_putc(out, ']');
  } else if (node.is.upper_ref) {
    char* name = _get_tok_text(tt, node._upper_id);
    if (is_lexer) {
      // In lexer context, uppercase refs are fragment/token references -> #{Name}.
      // Scope-like references are expanded only when emitting scope bodies.
      _buf_printf(out, "#{%s}", name);
    } else {
      // In parser context, uppercase refs are token references -> @snake_case
      char* snake = _to_snake_case(name);
      if (_strarr_find(&st->parser_tokens, snake) < 0) {
        _strarr_push(&st->parser_tokens, snake);
      }
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
      // In parser context: always emit as helper rule to avoid nested brackets
      Node_alt_list al = g4_grammar_load_alt_list(node.alt_list);
      int32_t count = 0;
      PegLink cnt = al.alt;
      for (; g4_grammar_has_elem(&cnt); g4_grammar_get_next(&cnt)) {
        count++;
      }
      // Generate a helper rule for every parser parenthesized subexpression.
      Buf helper_body;
      _buf_init(&helper_body);
      if (count > 1) {
        _buf_puts(&helper_body, "[\n");
        int32_t tag_idx = 0;
        for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
          _buf_puts(&helper_body, "  ");
          PegRef alt_ref = g4_grammar_get_lhs(&link);
          _walk_alt(st, tt, alt_ref, is_lexer, &helper_body);
          _buf_printf(&helper_body, " : alt_%d\n", ++tag_idx);
        }
        _buf_putc(&helper_body, ']');
      } else {
        PegLink link = al.alt;
        if (g4_grammar_has_elem(&link)) {
          PegRef alt_ref = g4_grammar_get_lhs(&link);
          _walk_alt(st, tt, alt_ref, is_lexer, &helper_body);
        }
      }
      // Generate helper rule name
      int32_t paren_id;
      if (paren_counter) {
        paren_id = ++(*paren_counter);
      } else {
        paren_id = ++st->current_rule_paren_counter;
      }
      char helper_name[256];
      snprintf(helper_name, sizeof(helper_name), "_%s_%d",
               rule_snake_name ? rule_snake_name : (st->current_rule_snake ? st->current_rule_snake : "anon"),
               paren_id);
      st->helpers.count++;
      // Store helper
      if (st->helpers.count > st->helpers.cap) {
        st->helpers.cap = st->helpers.cap ? st->helpers.cap * 2 : 16;
        st->helpers.items = realloc(st->helpers.items, sizeof(HelperRule) * st->helpers.cap);
      }
      st->helpers.items[st->helpers.count - 1] = (HelperRule){.name = strdup(helper_name), .body = helper_body.data};
      StrArr wrappers;
      _strarr_init(&wrappers);
      _edge_wrappers_for_parser_rule(st, helper_name, &wrappers);
      if (wrappers.len > 1) {
        _buf_puts(out, helper_name);
      } else if (wrappers.len == 1) {
        _buf_puts(out, wrappers.items[0]);
      } else {
        // Emit reference to helper
        _buf_puts(out, helper_name);
      }
      _strarr_free(&wrappers);
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

static bool _action_contains_pop_mode(TokenTree* tt, PegRef brace_block_ref) {
  if (g4_grammar_peg_size(brace_block_ref) <= 0) {
    return false;
  }
  Node_brace_block bb = g4_grammar_load_brace_block(brace_block_ref);
  for (PegLink link = bb.brace_content; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef content_ref = g4_grammar_get_lhs(&link);
    Node_brace_content content = g4_grammar_load_brace_content(content_ref);
    if (content.is._brace_text) {
      char* text = _get_tok_text(tt, content._brace_text);
      bool has_pop = strstr(text, "popMode") != NULL;
      free(text);
      if (has_pop) {
        return true;
      }
    } else if (content.is.brace_block && _action_contains_pop_mode(tt, content.brace_block)) {
      return true;
    }
  }
  return false;
}

static bool _alt_has_pop_mode_action(TokenTree* tt, PegRef alt_ref) {
  Node_alt alt = g4_grammar_load_alt(alt_ref);
  for (PegLink link = alt.labeled_element; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef le_ref = g4_grammar_get_lhs(&link);
    Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
    Node_element el = g4_grammar_load_element(le.element);
    Node_atom atom = g4_grammar_load_atom(el.atom);
    if (atom.is.action_atom && _action_contains_pop_mode(tt, atom.brace_block)) {
      return true;
    }
  }
  return false;
}

static bool _alt_list_has_pop_mode_action(TokenTree* tt, PegRef alt_list_ref) {
  Node_alt_list al = g4_grammar_load_alt_list(alt_list_ref);
  for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    if (_alt_has_pop_mode_action(tt, g4_grammar_get_lhs(&link))) {
      return true;
    }
  }
  return false;
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
    lr.from_lexer_grammar = st->current_file_is_lexer_grammar;
    lr.lexer_grammar_index = st->current_lexer_grammar_index;
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

    if (!lr.pop_mode && _alt_list_has_pop_mode_action(tt, lr.alt_list_ref)) {
      lr.pop_mode = true;
    }
    if (lr.pop_mode) {
      lr.is_fragment = false;
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

  st->current_file_is_lexer_grammar = false;
  st->current_lexer_grammar_index = -1;
  if (gtype.is.lexer_type) {
    st->is_lexer = true;
    st->current_file_is_lexer_grammar = true;
    st->current_lexer_grammar_index = ++st->lexer_grammar_count;
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

static LexerRule* _find_lexer_rule(G4State* st, const char* name) {
  for (int32_t i = st->lexer_rule_count - 1; i >= 0; i--) {
    if (strcmp(st->lexer_rules[i].name, name) == 0) {
      return &st->lexer_rules[i];
    }
  }
  return NULL;
}

static void _emit_lexer_rule_body(G4State* st, LexerRule* r, Buf* out) {
  _walk_alt_list(st, r->tt, r->alt_list_ref, true, out);
}

static char* _lexer_effective_token_snake(LexerRule* r) {
  if (r->is_skipped) {
    return NULL;
  }
  if (r->type_alias) {
    return _to_snake_case(r->type_alias);
  }
  return strdup(r->snake_name);
}

static void _emit_end_action(Buf* out, const char* tok_name, bool skipped) {
  if (skipped) {
    _buf_puts(out, " .end");
  } else {
    _buf_printf(out, " @%s .end", tok_name);
  }
}

static bool _alt_single_upper(TokenTree* tt, PegRef alt_ref, char** name_out) {
  Node_alt alt = g4_grammar_load_alt(alt_ref);
  PegLink link = alt.labeled_element;
  if (!g4_grammar_has_elem(&link)) return false;
  PegRef le_ref = g4_grammar_get_lhs(&link);
  g4_grammar_get_next(&link);
  if (g4_grammar_has_elem(&link)) return false;
  Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
  Node_element el = g4_grammar_load_element(le.element);
  if (g4_grammar_has_elem(&el.ebnf_suffix)) return false;
  Node_atom atom = g4_grammar_load_atom(el.atom);
  if (!atom.is.upper_ref) return false;
  *name_out = _get_tok_text(tt, atom._upper_id);
  return true;
}

static void _emit_scope_end_alts(G4State* st, TokenTree* tt, PegRef alt_list_ref, const char* tok_name, bool skipped,
                                 Buf* out);

static void _emit_scope_end_ref(G4State* st, const char* name, const char* tok_name, bool skipped, Buf* out) {
  LexerRule* ref = _find_lexer_rule(st, name);
  if (ref && ref->is_fragment && ref->needs_scope) {
    _emit_scope_end_alts(st, ref->tt, ref->alt_list_ref, tok_name, skipped, out);
  } else if (ref && (ref->is_recursive || ref->needs_scope) && !ref->is_fragment) {
    _buf_printf(out, "  %s", ref->snake_name);
    _emit_end_action(out, tok_name, skipped);
    _buf_putc(out, '\n');
  } else {
    _buf_printf(out, "  /#{%s}/", name);
    _emit_end_action(out, tok_name, skipped);
    _buf_putc(out, '\n');
  }
}

static void _emit_scope_end_alts(G4State* st, TokenTree* tt, PegRef alt_list_ref, const char* tok_name, bool skipped,
                                 Buf* out) {
  Node_alt_list al = g4_grammar_load_alt_list(alt_list_ref);
  for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef alt_ref = g4_grammar_get_lhs(&link);
    char* name = NULL;
    if (_alt_single_upper(tt, alt_ref, &name)) {
      _emit_scope_end_ref(st, name, tok_name, skipped, out);
      free(name);
    } else {
      Buf body;
      _buf_init(&body);
      _walk_alt(st, tt, alt_ref, true, &body);
      _buf_printf(out, "  /%s/", body.data);
      _emit_end_action(out, tok_name, skipped);
      _buf_putc(out, '\n');
      _buf_free(&body);
    }
  }
}

static bool _lexer_rule_needs_scope(G4State* st, LexerRule* r);

static bool _alt_needs_scope(G4State* st, TokenTree* tt, PegRef alt_ref) {
  Node_alt alt = g4_grammar_load_alt(alt_ref);
  for (PegLink link = alt.labeled_element; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef le_ref = g4_grammar_get_lhs(&link);
    Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
    Node_element el = g4_grammar_load_element(le.element);
    if (g4_grammar_has_elem(&el.ebnf_suffix)) continue;
    Node_atom atom = g4_grammar_load_atom(el.atom);
    if (atom.is.upper_ref) {
      char* name = _get_tok_text(tt, atom._upper_id);
      LexerRule* ref = _find_lexer_rule(st, name);
      bool needs = ref && _lexer_rule_needs_scope(st, ref);
      free(name);
      if (needs) return true;
    } else if (atom.is.group_atom) {
      Node_alt_list al = g4_grammar_load_alt_list(atom.alt_list);
      for (PegLink alink = al.alt; g4_grammar_has_elem(&alink); g4_grammar_get_next(&alink)) {
        if (_alt_needs_scope(st, tt, g4_grammar_get_lhs(&alink))) return true;
      }
    }
  }
  return false;
}

static bool _lexer_rule_needs_scope(G4State* st, LexerRule* r) {
  if (r->is_recursive || r->needs_scope) return true;
  if (r->needs_scope_traced) return false;
  r->needs_scope_traced = true;
  Node_alt_list al = g4_grammar_load_alt_list(r->alt_list_ref);
  for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    if (_alt_needs_scope(st, r->tt, g4_grammar_get_lhs(&link))) {
      r->needs_scope = true;
      return true;
    }
  }
  return false;
}

static void _emit_scope_end_element(G4State* st, TokenTree* tt, PegRef element_ref, const char* tok_name, bool skipped,
                                    Buf* out) {
  Node_element el = g4_grammar_load_element(element_ref);
  Node_atom atom = g4_grammar_load_atom(el.atom);
  if (!g4_grammar_has_elem(&el.ebnf_suffix) && atom.is.upper_ref) {
    char* name = _get_tok_text(tt, atom._upper_id);
    _emit_scope_end_ref(st, name, tok_name, skipped, out);
    free(name);
  } else if (!g4_grammar_has_elem(&el.ebnf_suffix) && atom.is.group_atom) {
    _emit_scope_end_alts(st, tt, atom.alt_list, tok_name, skipped, out);
  } else {
    Buf body;
    _buf_init(&body);
    _walk_element(st, tt, element_ref, true, &body);
    _buf_printf(out, "  /%s/", body.data);
    _emit_end_action(out, tok_name, skipped);
    _buf_putc(out, '\n');
    _buf_free(&body);
  }
}

static void _emit_scope_tail_alt(G4State* st, TokenTree* tt, PegRef alt_ref, int32_t tail_start, const char* tok_name,
                                 bool skipped, Buf* out) {
  Node_alt alt = g4_grammar_load_alt(alt_ref);
  PegLink link = alt.labeled_element;
  int32_t idx = 0;
  for (; idx < tail_start && g4_grammar_has_elem(&link); idx++) {
    g4_grammar_get_next(&link);
  }
  if (!g4_grammar_has_elem(&link)) {
    _buf_puts(out, "  /./ .unparse");
    _emit_end_action(out, tok_name, skipped);
    _buf_putc(out, '\n');
    return;
  }
  for (; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef le_ref = g4_grammar_get_lhs(&link);
    Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
    _emit_scope_end_element(st, tt, le.element, tok_name, skipped, out);
  }
}

static void _emit_scope_like_rule(G4State* st, LexerRule* r, Buf* out) {
  Node_alt_list al = g4_grammar_load_alt_list(r->alt_list_ref);
  char* tok_name = _lexer_effective_token_snake(r);
  bool skipped = tok_name == NULL || !_parser_uses_token(st, tok_name);
  bool emitted = false;
  for (PegLink alink = al.alt; g4_grammar_has_elem(&alink); g4_grammar_get_next(&alink)) {
    PegRef alt_ref = g4_grammar_get_lhs(&alink);
    Node_alt alt = g4_grammar_load_alt(alt_ref);
    PegLink elem_link = alt.labeled_element;
    if (!g4_grammar_has_elem(&elem_link)) continue;
    PegRef begin_le_ref = g4_grammar_get_lhs(&elem_link);
    Node_labeled_element begin_le = g4_grammar_load_labeled_element(begin_le_ref);
    Node_element begin_el = g4_grammar_load_element(begin_le.element);
    Node_atom begin_atom = g4_grammar_load_atom(begin_el.atom);
    bool begin_is_scope = false;
    char* begin_scope_name = NULL;
    if (!g4_grammar_has_elem(&begin_el.ebnf_suffix)) {
      if (begin_atom.is.group_atom) {
        Node_alt_list begin_al = g4_grammar_load_alt_list(begin_atom.alt_list);
        for (PegLink blink = begin_al.alt; g4_grammar_has_elem(&blink); g4_grammar_get_next(&blink)) {
          PegRef begin_alt_ref = g4_grammar_get_lhs(&blink);
          char* name = NULL;
          if (_alt_single_upper(r->tt, begin_alt_ref, &name)) {
            LexerRule* ref = _find_lexer_rule(st, name);
            if (ref && _lexer_rule_needs_scope(st, ref)) {
              begin_is_scope = true;
              begin_scope_name = name;
              break;
            }
            free(name);
          } else if (_alt_needs_scope(st, r->tt, begin_alt_ref)) {
            begin_is_scope = true;
            break;
          }
        }
      } else if (begin_atom.is.upper_ref) {
        char* name = _get_tok_text(r->tt, begin_atom._upper_id);
        LexerRule* ref = _find_lexer_rule(st, name);
        begin_is_scope = ref && _lexer_rule_needs_scope(st, ref);
        if (begin_is_scope) {
          begin_scope_name = name;
        } else {
          free(name);
        }
      }
    }

    if (begin_is_scope) {
      if (!emitted) {
        _buf_printf(out, "%s = /./ .unparse .begin {\n", r->snake_name);
        emitted = true;
      }
      _emit_scope_end_element(st, r->tt, begin_le.element, tok_name, skipped, out);
      _emit_scope_tail_alt(st, r->tt, alt_ref, 1, tok_name, skipped, out);
      free(begin_scope_name);
    } else {
      Buf begin_buf;
      _buf_init(&begin_buf);
      _walk_element(st, r->tt, begin_le.element, true, &begin_buf);
      if (!emitted) {
        _buf_printf(out, "%s = /%s/ .begin {\n", r->snake_name, begin_buf.data);
        emitted = true;
      } else {
        _buf_printf(out, "  /%s/ .unparse\n", begin_buf.data);
      }
      _emit_scope_tail_alt(st, r->tt, alt_ref, 1, tok_name, skipped, out);
      _buf_free(&begin_buf);
    }
  }
  if (!emitted) {
    _buf_printf(out, "%s = /.*/ .begin {\n", r->snake_name);
    _buf_puts(out, "  /./ .unparse");
    _emit_end_action(out, tok_name, skipped);
    _buf_putc(out, '\n');
  }
  _buf_puts(out, "}\n");
  free(tok_name);
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
    // Multiple alternatives — emit as branch block with explicit tags
    _buf_puts(out, "[\n");
    int32_t tag_idx = 0;
    for (PegLink link = node.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
      PegRef alt_ref = g4_grammar_get_lhs(&link);
      _buf_puts(out, "  ");
      _walk_alt(st, r->tt, alt_ref, false, out);
      _buf_printf(out, " : alt_%d\n", ++tag_idx);
    }
    _buf_putc(out, ']');
  }
}

static bool _is_entrance_lexer_rule(G4State* st, LexerRule* r) {
  return !r->from_lexer_grammar || r->lexer_grammar_index == st->lexer_grammar_count;
}

static bool _is_entrance_mode(G4State* st, const char* mode) {
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r)) continue;
    if (r->mode_name && strcmp(r->mode_name, mode) == 0) return true;
    if (r->push_mode && strcmp(r->push_mode, mode) == 0) return true;
  }
  return false;
}

static const char* _find_mapping(G4State* st, const char* name) {
  for (int32_t i = 0; i < st->mode_map_count; i++) {
    if (strcmp(st->mode_maps[i].mode_name, name) == 0) {
      return st->mode_maps[i].parser_rule;
    }
  }
  return NULL;
}

static bool _mode_is_flat(G4State* st, const char* mode) {
  const char* mapped = _find_mapping(st, mode);
  return mapped && mapped[0] == '\0';
}

static char* _edge_name(const char* caller, const char* callee) {
  char* caller_snake = _to_snake_case(caller);
  char* callee_snake = _to_snake_case(callee);
  int32_t len = snprintf(NULL, 0, "%s__%s", caller_snake, callee_snake);
  char* name = malloc(len + 1);
  snprintf(name, len + 1, "%s__%s", caller_snake, callee_snake);
  free(caller_snake);
  free(callee_snake);
  return name;
}

static char* _mode_macro_name(const char* mode) {
  return _to_snake_case(mode);
}

static ParserRule* _find_parser_rule_snake(G4State* st, const char* snake_name) {
  for (int32_t i = 0; i < st->parser_rule_count; i++) {
    if (strcmp(st->parser_rules[i].snake_name, snake_name) == 0) {
      return &st->parser_rules[i];
    }
  }
  return NULL;
}

static void _collect_rule_token_set_from_alt_list(G4State* st, TokenTree* tt, PegRef ref, StrArr* tokens,
                                                  StrArr* visited_rules);

static void _collect_rule_token_set_from_atom(G4State* st, TokenTree* tt, PegRef ref, StrArr* tokens,
                                              StrArr* visited_rules) {
  if (g4_grammar_peg_size(ref) <= 0) return;
  Node_atom node = g4_grammar_load_atom(ref);
  if (node.is.upper_ref) {
    char* name = _get_tok_text(tt, node._upper_id);
    char* snake = _to_snake_case(name);
    if (_strarr_find(tokens, snake) < 0) {
      _strarr_push(tokens, snake);
    }
    free(snake);
    free(name);
  } else if (node.is.lower_ref) {
    char* name = _get_tok_text(tt, node._lower_id);
    char* snake = _to_snake_case(name);
    if (_strarr_find(visited_rules, snake) < 0) {
      ParserRule* pr = _find_parser_rule_snake(st, snake);
      if (pr) {
        _strarr_push(visited_rules, snake);
        _collect_rule_token_set_from_alt_list(st, pr->tt, pr->alt_list_ref, tokens, visited_rules);
      }
    }
    free(snake);
    free(name);
  } else if (node.is.group_atom) {
    _collect_rule_token_set_from_alt_list(st, tt, node.alt_list, tokens, visited_rules);
  }
}

static void _collect_rule_token_set_from_alt(G4State* st, TokenTree* tt, PegRef ref, StrArr* tokens,
                                             StrArr* visited_rules) {
  if (g4_grammar_peg_size(ref) <= 0) return;
  Node_alt alt = g4_grammar_load_alt(ref);
  for (PegLink link = alt.labeled_element; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    PegRef le_ref = g4_grammar_get_lhs(&link);
    Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
    Node_element el = g4_grammar_load_element(le.element);
    _collect_rule_token_set_from_atom(st, tt, el.atom, tokens, visited_rules);
  }
}

static void _collect_rule_token_set_from_alt_list(G4State* st, TokenTree* tt, PegRef ref, StrArr* tokens,
                                                  StrArr* visited_rules) {
  if (g4_grammar_peg_size(ref) <= 0) return;
  Node_alt_list al = g4_grammar_load_alt_list(ref);
  for (PegLink link = al.alt; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
    _collect_rule_token_set_from_alt(st, tt, g4_grammar_get_lhs(&link), tokens, visited_rules);
  }
}

static void _collect_parser_rule_token_set(G4State* st, const char* parser_rule_name, StrArr* tokens) {
  char* snake = _to_snake_case(parser_rule_name);
  ParserRule* pr = _find_parser_rule_snake(st, snake);
  if (pr) {
    StrArr visited;
    _strarr_init(&visited);
    _strarr_push(&visited, snake);
    _collect_rule_token_set_from_alt_list(st, pr->tt, pr->alt_list_ref, tokens, &visited);
    _strarr_free(&visited);
  }
  free(snake);
}

static void _emit_default_mode_subset(G4State* st, StrArr* tokens, Buf* out) {
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r) || r->is_fragment || r->is_recursive || r->needs_scope) continue;
    const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
    bool flat_mode = strcmp(rule_mode, "DEFAULT_MODE") != 0 && _mode_is_flat(st, rule_mode);
    if (strcmp(rule_mode, "DEFAULT_MODE") != 0 && !flat_mode) continue;
    char* tok_name = _lexer_effective_token_snake(r);
    bool token_in_set = tok_name && _strarr_find(tokens, tok_name) >= 0;
    bool keep = r->is_skipped || r->is_hidden || token_in_set;
    if (!keep) {
      free(tok_name);
      continue;
    }
    Buf body;
    _buf_init(&body);
    _emit_lexer_rule_body(st, r, &body);
    bool emit_token = tok_name && !r->is_skipped && token_in_set && strcmp(tok_name, "reserved") != 0;
    if (r->push_mode && !_mode_is_flat(st, r->push_mode) && strcmp(r->push_mode, "DEFAULT_MODE") != 0) {
      char* wrapper_name = _edge_name("DEFAULT_MODE", r->push_mode);
      _buf_printf(out, "  %s\n", wrapper_name);
      free(wrapper_name);
    } else if (r->pop_mode) {
      _buf_printf(out, "  /%s/", body.data);
      if (emit_token) {
        _buf_printf(out, " @%s", tok_name);
      }
      _buf_puts(out, " .end\n");
    } else if (r->push_mode && strcmp(r->push_mode, "DEFAULT_MODE") == 0) {
      _buf_printf(out, "  /%s/", body.data);
      if (emit_token) {
        _buf_printf(out, " @%s", tok_name);
      }
      _buf_puts(out, " .begin\n");
    } else if (emit_token) {
      _buf_printf(out, "  /%s/ @%s\n", body.data, tok_name);
    } else {
      _buf_printf(out, "  /%s/\n", body.data);
    }
    free(tok_name);
    _buf_free(&body);
  }
}

static void _emit_nest_output(G4State* st, FILE* outfp) {
  Buf out;
  _buf_init(&out);

  for (int32_t i = 0; i < st->parser_rule_count; i++) {
    _collect_parser_tokens_from_alt_list(st, st->parser_rules[i].tt, st->parser_rules[i].alt_list_ref);
  }

  // Collect ignored tokens
  StrArr ignore_tokens;
  _strarr_init(&ignore_tokens);
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r) || r->is_fragment || r->is_skipped) {
      continue;
    }
    if (r->is_hidden) {
      char* token_snake = _lexer_effective_token_snake(r);
      if (token_snake) {
        char tok_name[256];
        snprintf(tok_name, sizeof(tok_name), "@%s", token_snake);
        free(token_snake);
        if (_strarr_find(&ignore_tokens, tok_name) < 0) {
          _strarr_push(&ignore_tokens, tok_name);
        }
      }
    }
  }

  if (st->command_line) {
    _buf_printf(&out, "# generated by nest.g4 %s\n", st->command_line);
  } else {
    _buf_puts(&out, "# generated by nest.g4\n");
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

  // %define only for pure regexp rules that are referenced by another lexer
  // regexp/scope body. A lexer command changes how the rule itself emits in its
  // mode, but references to the rule still need its plain regexp body. This is
  // why DEFAULT_MODE `LPAREN: '(' -> pushMode(Inside)` may still get a
  // `%define LPAREN`, while unreferenced alias rules like
  // `Inside_RPAREN: RPAREN -> type(RPAREN)` do not get useless definitions.
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (r->is_recursive || r->needs_scope) {
      continue;
    }
    bool needed = false;
    char ref_pattern[256];
    snprintf(ref_pattern, sizeof(ref_pattern), "#{%s}", r->name);
    for (int32_t j = 0; j < st->lexer_rule_count && !needed; j++) {
      if (i == j) continue;
      LexerRule* user = &st->lexer_rules[j];
      Buf user_body;
      _buf_init(&user_body);
      _emit_lexer_rule_body(st, user, &user_body);
      needed = strstr(user_body.data, ref_pattern) != NULL;
      _buf_free(&user_body);
    }
    if (!needed) {
      continue;
    }
    Buf body;
    _buf_init(&body);
    _emit_lexer_rule_body(st, r, &body);
    _buf_printf(&out, "%%define %s /%s/\n", r->name, body.data);
    _buf_free(&body);
  }
  _buf_putc(&out, '\n');

  // Main scope wraps the default lexer macro and is the only generated VPA
  // scope that handles source EOF.
  _buf_puts(&out, "main = {\n");
  _buf_puts(&out, "  *default_mode\n");
  _buf_puts(&out, "  EOF .end\n");
  _buf_puts(&out, "}\n\n");

  _buf_puts(&out, "*default_mode = {\n");

  // Emit default mode rules
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r) || r->is_fragment || r->is_recursive) {
      continue;
    }
    const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
    bool flat_mode = strcmp(rule_mode, "DEFAULT_MODE") != 0 && _mode_is_flat(st, rule_mode);
    if (strcmp(rule_mode, "DEFAULT_MODE") != 0 && !flat_mode) {
      continue;
    }
    char* tok_name_for_filter = _lexer_effective_token_snake(r);
    bool token_used = !tok_name_for_filter || _parser_uses_token(st, tok_name_for_filter);
    bool omit_unused_default = tok_name_for_filter && !token_used && !r->is_skipped && !r->is_hidden && !r->push_mode && !r->pop_mode;
    if (omit_unused_default) {
      free(tok_name_for_filter);
      continue;
    }
    if (r->needs_scope) {
      if (token_used) {
        _buf_printf(&out, "  %s\n", r->snake_name);
      }
      free(tok_name_for_filter);
      continue;
    }

    Buf body;
    _buf_init(&body);
    _emit_lexer_rule_body(st, r, &body);
    char* tok_name = tok_name_for_filter;
    tok_name_for_filter = NULL;
    bool emit_token = tok_name && !r->is_skipped && token_used && strcmp(tok_name, "reserved") != 0;
    if (r->pop_mode) {
        _buf_printf(&out, "  /%s/", body.data);
        if (emit_token) {
          _buf_printf(&out, " @%s", tok_name);
        }
        _buf_puts(&out, " .end\n");
      } else if (r->push_mode && flat_mode && !_mode_is_flat(st, r->push_mode) && strcmp(r->push_mode, "DEFAULT_MODE") != 0) {
        char* wrapper_name = _edge_name("DEFAULT_MODE", r->push_mode);
        _buf_printf(&out, "  %s\n", wrapper_name);
        free(wrapper_name);
      } else if (r->push_mode && !_mode_is_flat(st, r->push_mode)) {
        if (strcmp(rule_mode, "DEFAULT_MODE") == 0 && strcmp(r->push_mode, "DEFAULT_MODE") == 0) {
          _buf_printf(&out, "  /%s/", body.data);
          if (emit_token) {
            _buf_printf(&out, " @%s", tok_name);
          }
          _buf_puts(&out, " .begin\n");
        } else if (flat_mode && strcmp(r->push_mode, "DEFAULT_MODE") == 0) {
          _buf_printf(&out, "  /%s/", body.data);
          if (emit_token) {
            _buf_printf(&out, " @%s", tok_name);
          }
          _buf_puts(&out, " .begin\n");
        } else {
          char* wrapper_name = _edge_name(rule_mode, r->push_mode);
          _buf_printf(&out, "  %s\n", wrapper_name);
          free(wrapper_name);
        }
    } else if (emit_token) {
      _buf_printf(&out, "  /%s/ @%s\n", body.data, tok_name);
    } else {
      _buf_printf(&out, "  /%s/\n", body.data);
    }
    free(tok_name);
    free(tok_name_for_filter);
    _buf_free(&body);
  }

  // Reference recursive scopes and mode scopes for reachability
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (_is_entrance_lexer_rule(st, r) && (r->is_recursive || r->needs_scope) && !r->is_fragment) {
      const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
      char* tok_name = _lexer_effective_token_snake(r);
      bool token_used = r->is_skipped || r->is_hidden || _parser_uses_token(st, tok_name);
      free(tok_name);
      if (!token_used) continue;
      if (r->needs_scope && r->type_alias && !r->is_fragment) {
        continue;
      }
      if (strcmp(rule_mode, "DEFAULT_MODE") == 0 || _mode_is_flat(st, rule_mode)) {
        _buf_printf(&out, "  %s\n", r->snake_name);
      }
    }
  }
  for (int32_t m = 0; m < st->modes.len; m++) {
    if (!_is_entrance_mode(st, st->modes.items[m]) || _mode_is_flat(st, st->modes.items[m])) continue;
  }
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r) || !r->needs_scope || r->is_fragment) continue;
    const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
    if (strcmp(rule_mode, "DEFAULT_MODE") == 0 || _mode_is_flat(st, rule_mode)) continue;
    _buf_printf(&out, "  %s\n", r->snake_name);
  }

  _buf_puts(&out, "}\n");

  // Emit recursive lexer rules as scopes
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r) || r->is_fragment || (!r->is_recursive && !r->needs_scope)) continue;
    const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
    if (strcmp(rule_mode, "DEFAULT_MODE") != 0 && !_mode_is_flat(st, rule_mode)) continue;
    char* scope_tok_name = _lexer_effective_token_snake(r);
    if (scope_tok_name && !r->is_hidden && !_parser_uses_token(st, scope_tok_name)) {
      free(scope_tok_name);
      continue;
    }
    free(scope_tok_name);

    _buf_printf(&out, "\n# Scope-like lexer rule: %s\n", r->name);

    if (!r->is_recursive && r->needs_scope) {
      _emit_scope_like_rule(st, r, &out);
      continue;
    }

    // Walk the body to extract parts
    st->suppress_nongreedy_warning = true;
    Node_alt_list al = g4_grammar_load_alt_list(r->alt_list_ref);
    PegLink alt_link = al.alt;
    PegRef alt_ref = g4_grammar_get_lhs(&alt_link);
    Node_alt alt_node = g4_grammar_load_alt(alt_ref);

    // Collect all elements
    Buf begin_buf, end_buf;
    _buf_init(&begin_buf);
    _buf_init(&end_buf);
    typedef struct { Buf buf; bool is_self; bool is_nongreedy; } ScopePart;
    ScopePart* parts = NULL;
    int32_t part_count = 0;
    int32_t part_cap = 0;

    int32_t elem_idx = 0;
    for (PegLink link = alt_node.labeled_element; g4_grammar_has_elem(&link); g4_grammar_get_next(&link)) {
      PegRef le_ref = g4_grammar_get_lhs(&link);
      Node_labeled_element le = g4_grammar_load_labeled_element(le_ref);
      Node_element el = g4_grammar_load_element(le.element);

      Buf elem_buf;
      _buf_init(&elem_buf);
      _walk_atom(st, r->tt, el.atom, true, &elem_buf, NULL, NULL, false);

      // Check if this element is a self-reference
      char self_ref[256];
      snprintf(self_ref, sizeof(self_ref), "#{%s}", r->name);
      bool is_self = (strcmp(elem_buf.data, self_ref) == 0);

      // Check if this element is a group containing self-reference
      bool group_has_self = (!is_self && strstr(elem_buf.data, self_ref) != NULL);

      if (elem_idx == 0 && !is_self && !group_has_self) {
        // First non-self element: begin pattern
        _buf_puts(&begin_buf, elem_buf.data);
        // Apply suffix
        if (g4_grammar_has_elem(&el.ebnf_suffix)) {
          Node_ebnf_suffix suffix = g4_grammar_load_ebnf_suffix(g4_grammar_get_lhs(&el.ebnf_suffix));
          if (suffix.is.star_suffix) _buf_putc(&begin_buf, '*');
          else if (suffix.is.plus_suffix) _buf_putc(&begin_buf, '+');
          else if (suffix.is.maybe) _buf_putc(&begin_buf, '?');
        }
      } else {
        // Remaining elements: check if last non-self, non-group is end
        if (part_count >= part_cap) {
          part_cap = part_cap ? part_cap * 2 : 8;
          parts = realloc(parts, sizeof(ScopePart) * part_cap);
        }
        parts[part_count].is_self = is_self || group_has_self;
        parts[part_count].is_nongreedy = false;
        _buf_init(&parts[part_count].buf);
        _buf_puts(&parts[part_count].buf, elem_buf.data);
        if (g4_grammar_has_elem(&el.ebnf_suffix)) {
          Node_ebnf_suffix suffix = g4_grammar_load_ebnf_suffix(g4_grammar_get_lhs(&el.ebnf_suffix));
          if (suffix.is.star_suffix) {
            _buf_putc(&parts[part_count].buf, '*');
            if (g4_grammar_has_elem(&suffix._question$2)) {
              parts[part_count].is_nongreedy = true;
            }
          } else if (suffix.is.plus_suffix) {
            _buf_putc(&parts[part_count].buf, '+');
            if (g4_grammar_has_elem(&suffix._question$3)) {
              parts[part_count].is_nongreedy = true;
            }
          } else if (suffix.is.maybe) {
            _buf_putc(&parts[part_count].buf, '?');
            if (g4_grammar_has_elem(&suffix._question$1)) {
              parts[part_count].is_nongreedy = true;
            }
          }
        }
        part_count++;
      }
      _buf_free(&elem_buf);
      elem_idx++;
    }

    // Last non-self part is the end pattern
    int32_t end_idx = -1;
    for (int32_t p = part_count - 1; p >= 0; p--) {
      if (!parts[p].is_self) {
        end_idx = p;
        _buf_puts(&end_buf, parts[p].buf.data);
        break;
      }
    }

    bool close_first = false;
    for (int32_t p = 0; p < part_count; p++) {
      if (p == end_idx) continue;
      if (parts[p].is_nongreedy) {
        close_first = true;
        break;
      }
    }

    const char* rule_prefix = r->is_fragment ? "*" : "";
    const char* begin_action = r->is_fragment ? "" : " @%s";

    // Emit scope
    _buf_printf(&out, "%s%s = /%s/ .begin", rule_prefix, r->snake_name, begin_buf.data);
    if (!r->is_fragment) {
      _buf_printf(&out, begin_action, r->snake_name);
    }
    _buf_puts(&out, " {\n");
    if (close_first) {
      if (end_idx >= 0) {
        _buf_printf(&out, "  /%s/ .end\n", end_buf.data);
      } else {
        _buf_puts(&out, "  /./ .end\n");
      }
    }
    // Emit self-reference and inner parts (excluding begin and end)
    for (int32_t p = 0; p < part_count; p++) {
      if (p == end_idx) continue;
      if (parts[p].is_self) {
        // Emit self-reference
        _buf_printf(&out, "  %s\n", r->snake_name);
        // If the buffer contains more than just self-ref (group with alternates),
        // extract and emit the non-self alternatives as /regex/ patterns
        char self_ref_str[256];
        snprintf(self_ref_str, sizeof(self_ref_str), "#{%s}", r->name);
        const char* buf = parts[p].buf.data;
        // Look for content after removing self-ref and group markers
        // Pattern: (*self_ref|<other>)suffix
        if (buf[0] == '(') {
          // Find the closing paren (may have suffix after it)
          const char* close_paren = strrchr(buf, ')');
          if (close_paren) {
          // Find alternatives separated by |
          const char* search = buf + 1; // skip (
          while (search < close_paren) {
            // Skip to next | or )
            const char* pipe = NULL;
            for (const char* s = search; s < close_paren; s++) {
              if (*s == '|') { pipe = s; break; }
            }
            if (!pipe) pipe = close_paren;
            // Extract this alternative
            int32_t alt_len = (int32_t)(pipe - search);
            char* alt = malloc(alt_len + 1);
            memcpy(alt, search, alt_len);
            alt[alt_len] = '\0';
            // Trim spaces
            char* trimmed = alt;
            while (*trimmed == ' ') trimmed++;
            int32_t tlen = (int32_t)strlen(trimmed);
            while (tlen > 0 && trimmed[tlen - 1] == ' ') trimmed[--tlen] = '\0';
            // Skip if it's the self-reference
            if (strlen(trimmed) > 0 && strcmp(trimmed, self_ref_str) != 0) {
              if (trimmed[0] == '*') {
                _buf_printf(&out, "  %s\n", trimmed);
              } else {
                _buf_printf(&out, "  /%s/\n", trimmed);
              }
            }
            free(alt);
            if (pipe >= close_paren) break;
            search = pipe + 1;
          }
          }
        }
      } else if (parts[p].buf.data[0] == '*') {
        _buf_printf(&out, "  %s\n", parts[p].buf.data);
      } else {
        _buf_printf(&out, "  /%s/\n", parts[p].buf.data);
      }
    }
    // Emit end pattern with .end
    if (!close_first) {
      if (end_idx >= 0) {
        _buf_printf(&out, "  /%s/ .end\n", end_buf.data);
      } else {
        _buf_puts(&out, "  /./ .end\n");
      }
    }
    _buf_puts(&out, "}\n");
    st->suppress_nongreedy_warning = false;

    // Cleanup
    _buf_free(&begin_buf);
    _buf_free(&end_buf);
    for (int32_t p = 0; p < part_count; p++) {
      _buf_free(&parts[p].buf);
    }
    free(parts);
  }

  // Emit mode macros
  for (int32_t m = 0; m < st->modes.len; m++) {
    const char* mode = st->modes.items[m];
    if (!_is_entrance_mode(st, mode) || _mode_is_flat(st, mode)) continue;
    char* macro_name = _mode_macro_name(mode);
    _buf_printf(&out, "\n# Mode macro: %s\n", mode);
    _buf_printf(&out, "*%s = {\n", macro_name);
    for (int32_t i = 0; i < st->lexer_rule_count; i++) {
      LexerRule* r = &st->lexer_rules[i];
      if (!_is_entrance_lexer_rule(st, r) || r->is_fragment || r->needs_scope) continue;
      const char* rule_mode = r->mode_name ? r->mode_name : "DEFAULT_MODE";
      if (strcmp(rule_mode, mode) != 0) continue;
      Buf body;
      _buf_init(&body);
      _emit_lexer_rule_body(st, r, &body);
      char* tok_name = _lexer_effective_token_snake(r);
      bool emit_token = tok_name && !r->is_skipped && _parser_uses_token(st, tok_name) && strcmp(tok_name, "reserved") != 0;
      if (r->push_mode && !_mode_is_flat(st, r->push_mode)) {
        char* wrapper_name = _edge_name(mode, r->push_mode);
        _buf_printf(&out, "  %s\n", wrapper_name);
        free(wrapper_name);
      } else if (r->pop_mode) {
        _buf_printf(&out, "  /%s/", body.data);
        if (emit_token) {
          _buf_printf(&out, " @%s", tok_name);
        }
        _buf_puts(&out, " .end\n");
      } else if (!tok_name) {
        _buf_printf(&out, "  /%s/\n", body.data);
      } else {
        _buf_printf(&out, "  /%s/ @%s\n", body.data, tok_name);
      }
      free(tok_name);
      _buf_free(&body);
    }
    _buf_puts(&out, "}\n");
    free(macro_name);
  }

  // Emit caller__callee wrappers for pushMode edges.
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* r = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, r) || !r->push_mode || _mode_is_flat(st, r->push_mode)) continue;
    const char* caller = r->mode_name ? r->mode_name : "DEFAULT_MODE";
    if (_mode_is_flat(st, caller)) continue;
    if (strcmp(caller, "DEFAULT_MODE") == 0 && strcmp(r->push_mode, "DEFAULT_MODE") == 0) continue;
    char* wrapper_name = _edge_name(caller, r->push_mode);
    char* callee_macro = _mode_macro_name(r->push_mode);
    const char* edge_parser_rule = _edge_parser_rule(st, caller, r->push_mode);
    Buf body;
    _buf_init(&body);
    _emit_lexer_rule_body(st, r, &body);
    _buf_printf(&out, "\n# Mode edge: %s -> %s\n", caller, r->push_mode);
    _buf_printf(&out, "%s = /%s/ .begin", wrapper_name, body.data);
    char* tok_name = _lexer_effective_token_snake(r);
    if (tok_name && (!edge_parser_rule || _parser_uses_token(st, tok_name))) {
      _buf_printf(&out, " @%s", tok_name);
    }
    free(tok_name);
    _buf_puts(&out, " {\n");
    if (strcmp(r->push_mode, "DEFAULT_MODE") == 0 && edge_parser_rule) {
      StrArr tokens;
      _strarr_init(&tokens);
      _collect_parser_rule_token_set(st, edge_parser_rule, &tokens);
      for (int32_t i = 0; i < st->lexer_rule_count; i++) {
        LexerRule* tr = &st->lexer_rules[i];
        if (!_is_entrance_lexer_rule(st, tr) || tr->is_fragment || !tr->needs_scope) continue;
        char* ts = _lexer_effective_token_snake(tr);
        if (ts && _strarr_find(&tokens, ts) >= 0) {
          _buf_printf(&out, "  %s\n", tr->snake_name);
        }
        free(ts);
      }
      _emit_default_mode_subset(st, &tokens, &out);
      _strarr_free(&tokens);
    } else {
      _buf_printf(&out, "  *%s\n", callee_macro);
    }
    _buf_puts(&out, "}\n");
    _buf_free(&body);
    free(wrapper_name);
    free(callee_macro);
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
      // Use first rule as main; if there are multiple top-level entry points
      // (e.g. kotlinFile and script), just reference the first one
      _buf_printf(&out, "main = %s\n", st->parser_rules[0].snake_name);
    }
  }

  // Emit parser rules
  // First pass: emit all rules and collect body text
  Buf* rule_bodies = malloc(sizeof(Buf) * st->parser_rule_count);
  for (int32_t i = 0; i < st->parser_rule_count; i++) {
    ParserRule* r = &st->parser_rules[i];
    _buf_init(&rule_bodies[i]);
    st->current_rule_snake = r->snake_name;
    st->current_rule_paren_counter = 0;
    _emit_parser_rule_body(st, r, &rule_bodies[i]);
    st->current_rule_snake = NULL;
  }

  // Determine which rules are referenced (transitive closure)
  bool* referenced = calloc(st->parser_rule_count, sizeof(bool));
  if (st->parser_rule_count > 0) {
    referenced[0] = true; // main entry is always referenced
  }
  for (int32_t m = 0; m < st->mode_map_count; m++) {
    if (st->mode_maps[m].parser_rule[0] == '\0') continue;
    char* mapped_rule_snake = _to_snake_case(st->mode_maps[m].parser_rule);
    for (int32_t j = 0; j < st->parser_rule_count; j++) {
      if (strcmp(st->parser_rules[j].snake_name, mapped_rule_snake) == 0) {
        referenced[j] = true;
        break;
      }
    }
    free(mapped_rule_snake);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (int32_t i = 0; i < st->parser_rule_count; i++) {
      if (!referenced[i]) continue;
      for (int32_t j = 0; j < st->parser_rule_count; j++) {
        if (referenced[j]) continue;
        if (strstr(rule_bodies[i].data, st->parser_rules[j].snake_name)) {
          referenced[j] = true;
          changed = true;
        }
      }
    }
    // Also check helper rule bodies
    for (int32_t h = 0; h < st->helpers.count; h++) {
      for (int32_t j = 0; j < st->parser_rule_count; j++) {
        if (!referenced[j] && strstr(st->helpers.items[h].body, st->parser_rules[j].snake_name)) {
          referenced[j] = true;
          changed = true;
        }
      }
    }
  }

  // Emit only referenced rules
  for (int32_t i = 0; i < st->parser_rule_count; i++) {
    if (!referenced[i]) continue;
    _buf_printf(&out, "%s = %s\n", st->parser_rules[i].snake_name, rule_bodies[i].data);
  }

  // Emit PEG wrappers for mapped caller__callee VPA edges. Parser references
  // to these mapped rules are rewritten above to the same wrapper names.
  StrArr emitted_peg_edges;
  _strarr_init(&emitted_peg_edges);
  for (int32_t i = 0; i < st->lexer_rule_count; i++) {
    LexerRule* lr = &st->lexer_rules[i];
    if (!_is_entrance_lexer_rule(st, lr) || !lr->push_mode || _mode_is_flat(st, lr->push_mode)) continue;
    const char* caller = lr->mode_name ? lr->mode_name : "DEFAULT_MODE";
    if (_mode_is_flat(st, caller)) continue;
    if (strcmp(caller, "DEFAULT_MODE") == 0 && strcmp(lr->push_mode, "DEFAULT_MODE") == 0) continue;
    const char* mapped = _edge_parser_rule(st, caller, lr->push_mode);
    if (!mapped) continue;
    char* edge = _edge_name(caller, lr->push_mode);
    if (_strarr_find(&emitted_peg_edges, edge) >= 0) {
      free(edge);
      continue;
    }
    char* mapped_snake = _to_snake_case(mapped);
    _buf_printf(&out, "%s = %s\n", edge, mapped_snake);
    _strarr_push(&emitted_peg_edges, edge);
    free(mapped_snake);
    free(edge);
  }
  _strarr_free(&emitted_peg_edges);

  // Emit helper rules (only those referenced from emitted content)
  for (int32_t i = 0; i < st->helpers.count; i++) {
    // Check if this helper is referenced from any emitted rule body or other helper
    bool helper_used = false;
    for (int32_t j = 0; j < st->parser_rule_count; j++) {
      if (!referenced[j]) continue;
      if (strstr(rule_bodies[j].data, st->helpers.items[i].name)) {
        helper_used = true;
        break;
      }
    }
    if (!helper_used) {
      for (int32_t h = 0; h < st->helpers.count; h++) {
        if (h == i) continue;
        if (strstr(st->helpers.items[h].body, st->helpers.items[i].name)) {
          helper_used = true;
          break;
        }
      }
    }
    if (helper_used) {
      _buf_printf(&out, "%s = %s\n", st->helpers.items[i].name, st->helpers.items[i].body);
    }
  }

  for (int32_t i = 0; i < st->parser_rule_count; i++) {
    _buf_free(&rule_bodies[i]);
  }
  free(rule_bodies);
  free(referenced);

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
  Buf command_line;
  _buf_init(&command_line);
  for (int32_t i = 1; i < argc; i++) {
    if (i > 1) {
      _buf_putc(&command_line, ' ');
    }
    _buf_puts(&command_line, argv[i]);
  }
  st.command_line = strdup(command_line.data);
  _buf_free(&command_line);

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
  // First: detect recursive lexer rules
  for (int32_t i = 0; i < st.lexer_rule_count; i++) {
    LexerRule* r = &st.lexer_rules[i];
    // Emit body to check for self-reference
    Buf body;
    _buf_init(&body);
    bool had_nongreedy_warning = st.has_nongreedy;
    _walk_alt_list(&st, r->tt, r->alt_list_ref, true, &body);
    // Check if body contains a reference to this rule's own name (as #{Name})
    char ref_pattern[256];
    snprintf(ref_pattern, sizeof(ref_pattern), "#{%s}", r->name);
    if (strstr(body.data, ref_pattern)) {
      r->is_recursive = true;
      st.has_nongreedy = had_nongreedy_warning;
    }
    if (_is_entrance_lexer_rule(&st, r) && r->is_recursive && !r->is_hidden && !r->is_skipped && !_find_mapping(&st, r->name)) {
      fprintf(stderr, "error: recursive lexer rule '%s' has no command-line mapping\n", r->name);
      fprintf(stderr, "  add: %s=<parserRule>\n", r->name);
      exit(1);
    }
    _buf_free(&body);
  }

  bool changed_scope = true;
  while (changed_scope) {
    changed_scope = false;
    for (int32_t i = 0; i < st.lexer_rule_count; i++) {
      LexerRule* r = &st.lexer_rules[i];
      if (r->is_recursive || r->needs_scope) continue;
      Buf body;
      _buf_init(&body);
      _walk_alt_list(&st, r->tt, r->alt_list_ref, true, &body);
      for (int32_t j = 0; j < st.lexer_rule_count; j++) {
        LexerRule* dep = &st.lexer_rules[j];
        if (!dep->is_recursive && !dep->needs_scope) continue;
        char ref_pattern[256];
        snprintf(ref_pattern, sizeof(ref_pattern), "#{%s}", dep->name);
        if (strstr(body.data, ref_pattern)) {
          r->needs_scope = true;
          changed_scope = true;
          break;
        }
      }
      _buf_free(&body);
    }
  }

  for (int32_t i = 0; i < st.modes.len; i++) {
    const char* mode = st.modes.items[i];
    if (!_is_entrance_mode(&st, mode)) continue;
    bool found = false;
    for (int32_t j = 0; j < st.mode_map_count; j++) {
      if (strcmp(st.mode_maps[j].mode_name, mode) == 0) {
        found = true;
        break;
      }
      int32_t key_len = (int32_t)strlen(st.mode_maps[j].mode_name);
      int32_t mode_len = (int32_t)strlen(mode);
      if (key_len > mode_len + 1 && st.mode_maps[j].mode_name[key_len - mode_len - 1] == ':' &&
          strcmp(st.mode_maps[j].mode_name + key_len - mode_len, mode) == 0) {
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

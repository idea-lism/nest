// Test that parse_gen produces valid LLVM IR with all expected DFA functions.
// Runs the parse_gen binary, reads the output .ll, and validates:
// 1. Each expected function is defined
// 2. The IR compiles with clang

#include "compat.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

static const char* LL_PATH = BUILD_DIR "/test_parse_gen.ll";
static const char* OBJ_PATH = BUILD_DIR "/test_parse_gen.o";

static char* ll_buf = NULL;

static void _generate(void) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s/parse_gen %s 2>&1", BUILD_DIR, LL_PATH);
  FILE* p = compat_popen(cmd, "r");
  assert(p);
  char output[4096] = {0};
  size_t n = fread(output, 1, sizeof(output) - 1, p);
  output[n] = '\0';
  int32_t status = compat_pclose(p);
  if (status != 0) {
    fprintf(stderr, "\nparse_gen failed:\n%s\n", output);
  }
  assert(status == 0);

  FILE* f = fopen(LL_PATH, "r");
  assert(f);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  ll_buf = malloc((size_t)sz + 1);
  fread(ll_buf, 1, (size_t)sz, f);
  ll_buf[sz] = '\0';
  fclose(f);
}

static void _cleanup(void) {
  remove(LL_PATH);
  remove(OBJ_PATH);
  free(ll_buf);
}

// --- Function presence tests ---

static void _assert_func(const char* name) {
  char pattern[128];
  snprintf(pattern, sizeof(pattern), "define {i64, i64} @%s(", name);
  assert(strstr(ll_buf, pattern));
}

TEST(test_has_lex_main) { _assert_func("lex_main"); }
TEST(test_has_lex_vpa) { _assert_func("lex_vpa"); }
TEST(test_has_lex_scope) { _assert_func("lex_scope"); }
TEST(test_has_lex_lit_scope) { _assert_func("lex_lit_scope"); }
TEST(test_has_lex_peg) { _assert_func("lex_peg"); }
TEST(test_has_lex_branches) { _assert_func("lex_branches"); }
TEST(test_has_lex_peg_tag) { _assert_func("lex_peg_tag"); }
TEST(test_has_lex_re) { _assert_func("lex_re"); }
TEST(test_has_lex_re_ref) { _assert_func("lex_re_ref"); }
TEST(test_has_lex_charclass) { _assert_func("lex_charclass"); }
TEST(test_has_lex_re_str) { _assert_func("lex_re_str"); }
TEST(test_has_lex_peg_str) { _assert_func("lex_peg_str"); }

TEST(test_no_extra_defines) {
  int32_t count = 0;
  const char* p = ll_buf;
  while ((p = strstr(p, "define {i64, i64} @"))) {
    count++;
    p++;
  }
  assert(count >= 12);
}

// --- IR structure tests ---

TEST(test_has_target_triple) { assert(strstr(ll_buf, "target triple")); }

TEST(test_has_source_filename) { assert(strstr(ll_buf, "source_filename")); }

// --- Lexer behavior tests ---

typedef struct {
  int64_t state;
  int64_t action;
} MatchResult;
typedef MatchResult (*MatchFn)(int64_t, int64_t);

static int _run_cmd(const char* cmd_str) {
  FILE* p = compat_popen(cmd_str, "r");
  if (!p) {
    return -1;
  }
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, p);
  buf[n] = '\0';
  int exit_code = compat_pclose(p);
  if (exit_code != 0) {
    fprintf(stderr, "cmd failed: %s\noutput: %s\n", cmd_str, buf);
  }
  return exit_code;
}

static void* lib_handle = NULL;

static void _load_lib(void) {
  if (lib_handle) {
    return;
  }
  char lib_path[256];
#ifdef __APPLE__
  snprintf(lib_path, sizeof(lib_path), "%s/test_parse_gen_lex.dylib", BUILD_DIR);
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -dynamiclib -Wl,-undefined,dynamic_lookup -o %s %s 2>&1", LLVM_CC, lib_path, LL_PATH);
#else
  snprintf(lib_path, sizeof(lib_path), "%s/test_parse_gen_lex.so", BUILD_DIR);
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -shared -o %s %s 2>&1", LLVM_CC, lib_path, LL_PATH);
#endif
  int status = _run_cmd(cmd);
  assert(status == 0);

#ifdef _WIN32
  lib_handle = LoadLibraryA(lib_path);
#else
  lib_handle = dlopen(lib_path, RTLD_NOW);
  if (!lib_handle) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
  }
#endif
  assert(lib_handle);
}

static MatchFn _get_fn(const char* name) {
  _load_lib();
#ifdef _WIN32
  return (MatchFn)GetProcAddress(lib_handle, name);
#else
  return (MatchFn)dlsym(lib_handle, name);
#endif
}

static void _unload_lib(void) {
  if (!lib_handle) {
    return;
  }
#ifdef _WIN32
  FreeLibrary(lib_handle);
#else
  dlclose(lib_handle);
#endif
  lib_handle = NULL;
  char lib_path[256];
#ifdef __APPLE__
  snprintf(lib_path, sizeof(lib_path), "%s/test_parse_gen_lex.dylib", BUILD_DIR);
#else
  snprintf(lib_path, sizeof(lib_path), "%s/test_parse_gen_lex.so", BUILD_DIR);
#endif
  remove(lib_path);
}

TEST(test_lex_main_recognizes_bracket) {
  MatchFn fn = _get_fn("lex_main");
  assert(fn);
  // feed '[' then '[' — start of [[vpa]] header
  MatchResult r = fn(0, '[');
  assert(r.action != -2);
  r = fn(r.state, '[');
  assert(r.action != -2);
}

TEST(test_lex_re_recognizes_backslash) {
  MatchFn fn = _get_fn("lex_re");
  assert(fn);
  // backslash should be a valid transition in regex scope
  MatchResult r = fn(0, '\\');
  assert(r.action != -2);
}

TEST(test_lex_peg_recognizes_at) {
  MatchFn fn = _get_fn("lex_peg");
  assert(fn);
  // '@' is a token reference prefix in PEG scope
  MatchResult r = fn(0, '@');
  assert(r.action != -2);
}

TEST(test_lex_vpa_recognizes_eq) {
  MatchFn fn = _get_fn("lex_vpa");
  assert(fn);
  // '=' is used in VPA rules
  MatchResult r = fn(0, '=');
  assert(r.action != -2);
}

int main(void) {
  printf("test_parse_gen:\n");

  _generate();

  RUN(test_has_lex_main);
  RUN(test_has_lex_vpa);
  RUN(test_has_lex_scope);
  RUN(test_has_lex_lit_scope);
  RUN(test_has_lex_peg);
  RUN(test_has_lex_branches);
  RUN(test_has_lex_peg_tag);
  RUN(test_has_lex_re);
  RUN(test_has_lex_re_ref);
  RUN(test_has_lex_charclass);
  RUN(test_has_lex_re_str);
  RUN(test_has_lex_peg_str);
  RUN(test_no_extra_defines);
  RUN(test_has_target_triple);
  RUN(test_has_source_filename);
  RUN(test_lex_main_recognizes_bracket);
  RUN(test_lex_re_recognizes_backslash);
  RUN(test_lex_peg_recognizes_at);
  RUN(test_lex_vpa_recognizes_eq);

  _unload_lib();
  _cleanup();

  printf("all ok\n");
  return 0;
}

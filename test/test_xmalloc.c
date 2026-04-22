// Test xmalloc traced leak detection
// Strategy: write small C programs, compile+run them, verify stderr output.
#include "../src/xmalloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../build/subprocess.h"
#include "compat.h"

#define TEST(name) static void name(void)
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    printf("  %s ... ", #name);                                                                                        \
    fflush(stdout);                                                                                                    \
    name();                                                                                                            \
    printf("ok\n");                                                                                                    \
  } while (0)

// --- Helpers ---

static const char* _build_dir(void) { return BUILD_DIR; }

// Write source to file, compile, run, capture stderr. Returns malloc'd stderr string.
static char* _run_program(const char* src, int* exit_code) {
  char src_path[256], bin_path[256];
  snprintf(src_path, sizeof(src_path), "%s/test_xmalloc_prog.c", _build_dir());
  snprintf(bin_path, sizeof(bin_path), "%s/test_xmalloc_prog", _build_dir());

  FILE* f = fopen(src_path, "w");
  assert(f);
  fputs(src, f);
  fclose(f);

  // Compile
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "cc -std=c23 -DXMALLOC_TRACE -I src -o %s %s src/xmalloc.c src/darray.c", bin_path,
           src_path);
  const char* compile_argv[] = {"sh", "-c", cmd, NULL};
  struct subprocess_s proc;
  int ret = subprocess_create(compile_argv,
                              subprocess_option_combined_stdout_stderr | subprocess_option_search_user_path, &proc);
  assert(ret == 0);
  int compile_exit = -1;
  subprocess_join(&proc, &compile_exit);
  if (compile_exit != 0) {
    char buf[4096];
    unsigned n = subprocess_read_stdout(&proc, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    fprintf(stderr, "compile failed: %s\n%s\n", cmd, buf);
    subprocess_destroy(&proc);
    abort();
  }
  subprocess_destroy(&proc);

  // Run, capture stderr
  const char* run_argv[] = {bin_path, NULL};
  ret = subprocess_create(run_argv, subprocess_option_enable_async, &proc);
  assert(ret == 0);
  subprocess_join(&proc, exit_code);

  // Read all stderr in a loop
  size_t cap = 4096;
  size_t total = 0;
  char* out = malloc(cap);
  for (;;) {
    if (total + 1024 > cap) {
      cap *= 2;
      out = realloc(out, cap);
    }
    unsigned n = subprocess_read_stderr(&proc, out + total, (unsigned)(cap - total - 1));
    if (n == 0) {
      break;
    }
    total += n;
  }
  out[total] = '\0';
  subprocess_destroy(&proc);

  remove(src_path);
  remove(bin_path);
  return out;
}

// Count occurrences of needle in haystack
__attribute__((unused)) static int _count_str(const char* haystack, const char* needle) {
  int count = 0;
  const char* p = haystack;
  size_t nlen = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) {
    count++;
    p += nlen;
  }
  return count;
}

// --- Tests ---

TEST(test_no_leak) {
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* p = XMALLOC(64);\n"
                    "  XFREE(p);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_single_leak) {
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* p = XMALLOC(64);\n"
                    "  (void)p;\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "XMALLOC LEAK REPORT") != NULL);
  assert(strstr(err, "1 leak(s) detected") != NULL);
  assert(strstr(err, "main:3") != NULL);
  free(err);
}

TEST(test_multiple_leaks) {
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* a = XMALLOC(8);\n"
                    "  void* b = XCALLOC(1, 16);\n"
                    "  void* c = XMALLOC(32);\n"
                    "  (void)a; (void)b; (void)c;\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "XMALLOC LEAK REPORT") != NULL);
  assert(strstr(err, "3 leak(s) detected") != NULL);
  free(err);
}

TEST(test_partial_free) {
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* a = XMALLOC(8);\n"
                    "  void* b = XMALLOC(16);\n"
                    "  void* c = XMALLOC(32);\n"
                    "  XFREE(b);\n"
                    "  (void)a; (void)c;\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "2 leak(s) detected") != NULL);
  free(err);
}

TEST(test_calloc_no_leak) {
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* p = XCALLOC(10, 8);\n"
                    "  XFREE(p);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_realloc_tracked) {
  // realloc should update tracking — no leak if freed
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* p = XMALLOC(8);\n"
                    "  p = XREALLOC(p, 1024);\n"
                    "  XFREE(p);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_realloc_leak) {
  // realloc without free — should report 1 leak
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* p = XMALLOC(8);\n"
                    "  p = XREALLOC(p, 1024);\n"
                    "  (void)p;\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "1 leak(s) detected") != NULL);
  free(err);
}

TEST(test_caller_name_from_function) {
  // Leak report should show the allocating function name and line
  const char* src = "#include \"xmalloc.h\"\n"
                    "void* my_alloc_func(void) { return XMALLOC(64); }\n"
                    "int main(void) {\n"
                    "  void* p = my_alloc_func();\n"
                    "  (void)p;\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "my_alloc_func:2") != NULL);
  free(err);
}

TEST(test_many_allocs_trigger_expand) {
  // Allocate >4096 pointers to force hash table expansion (initial cap=1024, probe limit=4)
  const char* src = "#include \"xmalloc.h\"\n"
                    "#include <stddef.h>\n"
                    "#define N 5000\n"
                    "int main(void) {\n"
                    "  void* ptrs[N];\n"
                    "  for (int i = 0; i < N; i++) ptrs[i] = XMALLOC(1);\n"
                    "  for (int i = 0; i < N; i++) XFREE(ptrs[i]);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_many_allocs_expand_with_leaks) {
  // Allocate many, free half — verify correct leak count
  const char* src = "#include \"xmalloc.h\"\n"
                    "#include <stddef.h>\n"
                    "#define N 2000\n"
                    "int main(void) {\n"
                    "  void* ptrs[N];\n"
                    "  for (int i = 0; i < N; i++) ptrs[i] = XMALLOC(1);\n"
                    "  for (int i = 0; i < N; i += 2) XFREE(ptrs[i]);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "XMALLOC LEAK REPORT") != NULL);
  assert(strstr(err, "1000 leak(s) detected") != NULL);
  free(err);
}

TEST(test_realloc_null_is_malloc) {
  // realloc(NULL, size) should behave like malloc — tracked
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  void* p = XREALLOC(NULL, 64);\n"
                    "  XFREE(p);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_free_null_safe) {
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  XFREE(NULL);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_alloc_free_cycle) {
  // Repeated alloc/free to same variable — stress probe slots
  const char* src = "#include \"xmalloc.h\"\n"
                    "int main(void) {\n"
                    "  for (int i = 0; i < 10000; i++) {\n"
                    "    void* p = XMALLOC(16);\n"
                    "    XFREE(p);\n"
                    "  }\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_darray_leak) {
  // darray_new without darray_del should report leak with caller:line
  const char* src = "#include \"darray.h\"\n"
                    "#include <stdint.h>\n"
                    "void leaky(void) {\n"
                    "  int32_t* arr = darray_new(sizeof(int32_t), 0);\n"
                    "  (void)arr;\n"
                    "}\n"
                    "int main(void) {\n"
                    "  leaky();\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "XMALLOC LEAK REPORT") != NULL);
  assert(strstr(err, "1 leak(s) detected") != NULL);
  assert(strstr(err, "leaky:4") != NULL);
  free(err);
}

TEST(test_darray_no_leak) {
  const char* src = "#include \"darray.h\"\n"
                    "#include <stdint.h>\n"
                    "int main(void) {\n"
                    "  int32_t* arr = darray_new(sizeof(int32_t), 0);\n"
                    "  darray_del(arr);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

TEST(test_darray_grow_realloc_tracked) {
  // grow triggers realloc — should still be tracked correctly
  const char* src = "#include \"darray.h\"\n"
                    "#include <stdint.h>\n"
                    "int main(void) {\n"
                    "  int32_t* arr = darray_new(sizeof(int32_t), 0);\n"
                    "  for (int i = 0; i < 1000; i++) darray_push(arr, i);\n"
                    "  darray_del(arr);\n"
                    "  return 0;\n"
                    "}\n";
  int ec;
  char* err = _run_program(src, &ec);
  assert(ec == 0);
  assert(strstr(err, "LEAK") == NULL);
  free(err);
}

int main(void) {
  printf("--- %s ---\n", _build_dir());

  RUN(test_no_leak);
  RUN(test_single_leak);
  RUN(test_multiple_leaks);
  RUN(test_partial_free);
  RUN(test_calloc_no_leak);
  RUN(test_realloc_tracked);
  RUN(test_realloc_leak);
  RUN(test_caller_name_from_function);
  RUN(test_many_allocs_trigger_expand);
  RUN(test_many_allocs_expand_with_leaks);
  RUN(test_realloc_null_is_malloc);
  RUN(test_free_null_safe);
  RUN(test_alloc_free_cycle);
  RUN(test_darray_leak);
  RUN(test_darray_no_leak);
  RUN(test_darray_grow_realloc_tracked);

  printf("all ok\n");
  return 0;
}

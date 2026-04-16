# Header Writer

Minimal indentation-aware text writer for emitting C header files.

Impl "src/hdwriter.h" and "src/hdwriter.c".

## Struct

```c
struct HeaderWriter {
  FILE* out;
  int32_t indent; // current indentation depth, starts at 0
};
```

## API

### Lifecycle

```c
HeaderWriter* hdwriter_new(FILE* out);
void hdwriter_del(HeaderWriter* hw);
```

- `hdwriter_new`: allocate + zero-init. Store `out`.
- `hdwriter_del`: free if non-NULL.

### Output

```c
void hdwriter_puts(HeaderWriter* hw, const char* text);
void hdwriter_putc(HeaderWriter* hw, char c);
void hdwriter_printf(HeaderWriter* hw, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
```

- `hdwriter_puts`: emit indentation (2 spaces × `indent`), then `fputs(text, out)`.
- `hdwriter_putc`: emit single char via `fputc`. **No indentation.**
- `hdwriter_printf`: emit indentation, then `vfprintf`.

### Block Structure

```c
void hdwriter_begin(HeaderWriter* hw);
void hdwriter_end(HeaderWriter* hw);
```

- `hdwriter_begin`: emit ` {\n` (space, open brace, newline), then increment `indent`.
- `hdwriter_end`: decrement `indent`, assert `indent >= 0`, emit indentation, then `}\n`.

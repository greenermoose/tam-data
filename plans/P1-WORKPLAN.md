# Phase 1 Workplan: Native CSV/TSV Loaders & Grid Navigation (MVP)

This workplan defines the concrete engineering tasks, module boundaries, data structures, and verification criteria to implement **Phase 1** of [`ROADMAP.md`](../ROADMAP.md).

---

## 1. Phase 1 Objective

Turn the Phase 0 foundation into a functional tabular data explorer and editor capable of opening real-world datasets from disk or pipelines:
1. Parse arbitrary CSV (RFC 4180 compliant) and TSV files from disk (`mmap`-backed zero-copy slices where possible) or piped `stdin` (`cat data.csv | tam-data` using `/dev/tty` for interactive control).
2. Enhance the sheet viewport and VisiData status bar with cached dynamic column widths, right-aligned numeric formatting, error cell styling, pinned key columns (`!`), and live cursor coordinate display (`r/c`).
3. Complete core Phase 1 navigation and column manipulation: value jumps (`<`, `>`), blank-cell jumps (`{`, `}`), column reordering (`H`, `L`), bulk/manual column width controls (`_`, `g_`, `z_`, `gv`), column renaming (`^`), and type conversion (`#`, `%`, `$`, `@`, `~`, `z~`).
4. Export and save sheets atomically to `.csv` or `.tsv` via `Ctrl+S` (`tam_fs_write_atomic`), respecting visible columns, column ordering, and `row_order` indirection.
5. Maintain 100% warning-free C11 compilation, static `libtam` linkage, and leak-free `ASan`/`UBSan` test coverage.

---

## 2. Target Directory & File Layout

```text
~/Code/tam-data/
├── Makefile
├── README.md
├── ROADMAP.md
├── plans/
│   ├── P0-WORKPLAN.md
│   └── P1-WORKPLAN.md
├── src/
│   ├── version.h       # Version constants and build metadata macros
│   ├── utf8.h          # Inline UTF-8 codec and codepoint width helpers
│   ├── arena.h         # Fast bump/page arena allocator for strings and row slices
│   ├── arena.c
│   ├── val.h           # Tagged cell values (td_val_t), type sigils, formatting
│   ├── val.c
│   ├── sheet.h         # Columns, sheets (with mmap/buffer lifetime), navigation, stack
│   ├── sheet.c
│   ├── csv.h           # NEW: mmap & streaming CSV/TSV parser and atomic saver
│   ├── csv.c           # NEW: RFC 4180 state machine, TSV scanner, tam_fs_write_atomic export
│   ├── term.h          # Raw termios, VT/ANSI profiles, double-buffered cell grid
│   ├── term.c
│   ├── input.h         # Escape parser, prefix chords (< > { } H L g_ z_ Ctrl+S), prompt kinds
│   ├── input.c
│   ├── tam-data.h      # View state, grid/status compositor, demo sheet helper
│   └── tam-data.c      # CLI file/stdin loader, prompt dispatcher, main event loop
└── tests/
    ├── test.h          # Test assertions and suite declarations (adds test_csv)
    ├── test_main.c     # Test runner entry point
    ├── test_arena.c    # Arena allocation, alignment, and reset/free tests
    ├── test_val.c      # Value parsing, type conversion, and sigil formatting tests
    ├── test_sheet.c    # Sheet, column reorder/width, value/null jump, viewport tests
    ├── test_csv.c      # NEW: CSV/TSV parser, mmap/fd loading, and atomic save tests
    ├── test_term.c     # Headless cell buffer, numeric alignment, and status bar tests
    └── test_input.c    # Escape sequence decoding, Phase 1 chords, and prompt tests
```

---

## 3. Work Items

### W1: `mmap` & Streaming Backing Store + CSV/TSV Loader (`src/csv.*`, `src/sheet.*`)

- **Zero-Copy Backing Buffer Ownership (`td_sheet_t` in `src/sheet.h`):**
  - Extend `td_sheet_t` with backing storage fields for zero-copy `td_slice_t` cells:
    - `void *mmap_base; size_t mmap_len;` — released via `munmap()` in `td_sheet_free()`.
    - Regular files opened from disk are `mmap(..., PROT_READ, MAP_PRIVATE, fd, 0)`-backed when non-empty.
    - Non-seekable streams (`STDIN_FILENO` pipes/FIFOs) are read into a contiguous buffer allocated in `s->arena` (or heap buffer attached to the sheet) so raw cell slices still avoid per-cell allocations.
- **Loader Options (`td_csv_opts_t` in `src/csv.h`):**
  - `char delimiter`: `','` for CSV, `'\t'` for TSV, `' '` / custom for `-d`, or `'\0'` for automatic detection (by `.tsv`/`.tab` vs `.csv` extension, falling back to inspecting the first line for `'\t'` vs `','`).
  - `bool has_header`: `true` by default (first record defines column names); when `false` (`--no-header`), generates `col1`, `col2`, … (`1..N`) and treats the first record as row `0`.
  - `bool strict_tsv`: when `true` (or when `delimiter == '\t'` and unquoted TSV mode is active), uses a fast byte scan for `'\t'` and `'\n'`/`"\r\n"`.
- **RFC 4180 CSV & Fast TSV Parser (`src/csv.c`):**
  - Strips a leading UTF-8 BOM (`\xEF\xBB\xBF`) if present at byte `0`.
  - Handles LF (`\n`), CRLF (`\r\n`), bare CR (`\r`), and final lines without a trailing newline.
  - **Zero-Copy Fast Path:** Unquoted fields and quoted fields containing no escaped quotes (`""`) or embedded CRs produce a `td_slice_t` pointing directly into the backing buffer.
  - **Arena Unescape Path:** Quoted fields containing doubled quotes (`""` -> `"`) or embedded `\r\n` copy the unescaped bytes into `s->arena` via `td_arena_alloc()`.
  - **Null Representation:** Empty fields (`len == 0`) produce `TD_VAL_NULL` so blank-cell navigation (`{`, `}`) and future null aggregators work consistently.
  - **Ragged Row Handling:** If a data row contains more fields than the header row, dynamically appends new columns (`colN`) via `td_sheet_add_col(s, ..., TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP)`.
- **Public Loader API (`src/csv.h`):**
  - `void td_csv_opts_default(td_csv_opts_t *opts);`
  - `char td_csv_detect_delimiter(const char *path, const char *buf, size_t len);`
  - `td_sheet_t *td_csv_load_mem(const char *name, const char *source_path, const char *buf, size_t len, const td_csv_opts_t *opts, char *err, size_t err_cap);`
  - `td_sheet_t *td_csv_load_fd(int fd, const char *name, const td_csv_opts_t *opts, char *err, size_t err_cap);`
  - `td_sheet_t *td_csv_load_file(const char *path, const td_csv_opts_t *opts, char *err, size_t err_cap);`

---

### W2: Value Navigation, Column Manipulation & Cached Auto-Width (`src/sheet.*`)

- **Cached Auto-Width (`td_col_t`):**
  - Add `int cached_width;` to `td_col_t` (`0` = dirty/uncomputed when `width < 0`).
  - `td_sheet_col_width()` computes and caches the auto-width once (scanning up to the first 1,000 rows plus the active viewport window when `nrows` is large) and invalidates `cached_width` on column rename, type change, cell update, or explicit resize. This guarantees $O(1)$ width lookup per column during `td_view_render()` even on 100k+ row files.
- **Value & Blank-Cell Navigation (`src/sheet.h`, `src/sheet.c`):**
  - All navigation functions step through visible row indices (`0 .. s->nrows - 1`), resolving physical rows via `s->row_order[vr]`:
  - `bool td_sheet_jump_value(td_sheet_t *s, int dir)`:
    - Moves `s->cursor_row` in direction `dir` (`+1` for `>`, `-1` for `<`) to the next visible row whose value in `s->cursor_col` compares unequal (`td_val_cmp != 0` after coercion to `col->type`) to the current cell's value.
  - `bool td_sheet_jump_null(td_sheet_t *s, int dir)`:
    - Moves `s->cursor_row` in direction `dir` (`+1` for `}`, `-1` for `{`) to the next visible row whose cell in `s->cursor_col` is `TD_TYPE_NULL` (or an empty slice).
- **Column Reordering & Width Overrides (`src/sheet.h`, `src/sheet.c`):**
  - `bool td_sheet_move_col(td_sheet_t *s, size_t col, int dir)`:
    - Moves `s->cols[col]` one visible column position left (`dir < 0`, `H`) or right (`dir > 0`, `L`), swapping the `td_col_t` entries and updating `s->cursor_col` to follow the moved column.
  - `void td_sheet_autosize_col(td_sheet_t *s, size_t col)` (`_`):
    - Computes the exact width required by visible/sampled cells in `col` (with no artificial cap when manually triggered, or toggling back to `-TD_AUTO_WIDTH_CAP` if already expanded).
  - `void td_sheet_autosize_all_cols(td_sheet_t *s)` (`g_`):
    - Autosizes every visible column (`width != 0`).
  - `bool td_sheet_set_col_width(td_sheet_t *s, size_t col, int width)` (`z_`):
    - Sets an explicit fixed column width (`width >= 0`; `0` hides the column and advances `cursor_col` to the nearest visible column).
  - `void td_sheet_unhide_all_cols(td_sheet_t *s)` (`gv`):
    - Restores default auto-width (`-TD_AUTO_WIDTH_CAP`) on all hidden columns (`width == 0`).

---

### W3: Phase 1 Input Chords & Prompt Mode Tagging (`src/input.*`)

- **Command Enumeration (`td_cmd_t` in `src/input.h`):**
  - Add Phase 1 commands:
    - `TD_CMD_NEXT_VALUE` (`>`)
    - `TD_CMD_PREV_VALUE` (`<`)
    - `TD_CMD_NEXT_NULL` (`}`)
    - `TD_CMD_PREV_NULL` (`{`)
    - `TD_CMD_MOVE_COL_LEFT` (`H`)
    - `TD_CMD_MOVE_COL_RIGHT` (`L`)
    - `TD_CMD_AUTOSIZE_ALL_COLS` (`g_`)
    - `TD_CMD_SET_COL_WIDTH` (`z_`)
    - `TD_CMD_UNHIDE_ALL_COLS` (`gv`)
    - `TD_CMD_SAVE_SHEET` (`Ctrl+S`, byte `19` / `0x13`)
- **Chord Decoder Updates (`td_input_feed` in `src/input.c`):**
  - Single-key table (`prefix == 0`):
    - `'<'` -> `TD_CMD_PREV_VALUE`, `'>'` -> `TD_CMD_NEXT_VALUE`
    - `'{'` -> `TD_CMD_PREV_NULL`, `'}'` -> `TD_CMD_NEXT_NULL`
    - `'H'` -> `TD_CMD_MOVE_COL_LEFT`, `'L'` -> `TD_CMD_MOVE_COL_RIGHT`
    - `19` (`Ctrl+S`) -> `TD_CMD_SAVE_SHEET`
  - `g`-prefix table (`prefix == 1`):
    - `'_'` -> `TD_CMD_AUTOSIZE_ALL_COLS`
    - `'v'` -> `TD_CMD_UNHIDE_ALL_COLS`
  - `z`-prefix table (`prefix == 2`):
    - `'_'` -> `TD_CMD_SET_COL_WIDTH`
    - `'~'` -> `TD_CMD_TYPE_ANY`
- **Prompt Action Tagging (`td_prompt_kind_t` in `src/input.h`):**
  - Add `typedef enum { TD_PROMPT_NONE = 0, TD_PROMPT_RENAME_COL, TD_PROMPT_COL_WIDTH, TD_PROMPT_SAVE_SHEET } td_prompt_kind_t;` and store `td_prompt_kind_t kind` on `td_prompt_t` (with `td_prompt_start_kind()` or setting `prompt.kind`) so the main loop cleanly routes `TD_PROMPT_ACCEPT` to the active operation.

---

### W4: Sheet Viewport & VisiData Status Bar Enhancements (`src/tam-data.*`)

- **Cell Alignment & Type-Aware Styling (`td_view_render` in `src/tam-data.c`):**
  - **Numeric Right-Alignment:** When rendering a non-error cell in a numeric column (`col->type` is `TD_TYPE_INT`, `TD_TYPE_FLOAT`, or `TD_TYPE_CURRENCY`) whose formatted codepoint width `w` is less than the column `width`, right-align the text at `bx + (width - w)`.
  - **Error Cell Highlight:** When a cell evaluates to `TD_TYPE_ERR` (`#ERR`), render with error styling (`fg = 1` red when not selected, plus `TD_ATTR_BOLD` on the cursor row) so type conversion mismatches are immediately visible.
- **VisiData Status Bar (`td_render_status` in `src/tam-data.c`):**
  - **Left Segment:** Display stack depth, sheet name, and current 1-based row/column coordinates and column name:
    - Example: `[1/1] sales.csv | r3 c2:qty` (or `[1/1] empty | r0 c0` when the sheet has 0 rows/cols).
  - **Center Segment:** Modal prompt (`rename column:`, `column width:`, `save to:`) with underlined cursor position, or transient status message.
  - **Right Segment:** Pending prefix chord (`g`, `z`, `gz`), total rows, and selected row count:
    - Example: `g | 120 rows (4 selected)` or `120 rows (0 selected)`.

---

### W5: Atomic CSV/TSV Saver (`Ctrl+S`, `src/csv.*`)

- **Serializer & Atomic Disk Writer (`src/csv.h`, `src/csv.c`):**
  - `tam_str_t td_csv_format_sheet(const td_sheet_t *s, char delimiter);`
  - `bool td_csv_save_sheet(td_sheet_t *s, const char *path, char delimiter, char *err, size_t err_cap);`
- **Export Semantics:**
  - **Delimiter Selection:** If `delimiter == '\0'`, infer `','` when `path` ends with `.csv` (case-insensitive) and `'\t'` otherwise (`.tsv`, `.tab`, or arbitrary paths).
  - **Visible Column Order:** Export only visible columns (`col->width != 0`) in the same two-pass visual order rendered on screen (pinned `is_key` columns first, followed by regular visible columns).
  - **Visible Row Order:** Export rows in `s->row_order[0 .. s->nrows - 1]` order so any row permutation is preserved in the saved file.
  - **RFC 4180 CSV Escaping (`delimiter != '\t'`):** Quote any header or formatted cell containing `delimiter`, `'"'`, `'\n'`, or `'\r'`, doubling internal `'"'` characters as `""`.
  - **TSV Sanitization (`delimiter == '\t'`):** Write fields separated by `'\t'`, replacing any embedded `'\t'`, `'\r'`, or `'\n'` with `' '` so records remain strictly line-oriented.
  - **Atomic Write:** Persist the serialized buffer using `tam_fs_write_atomic(path, buf, tam_str_len(buf), 0644)`. On success, update `s->source_path` in `s->arena` and report `"saved %zu rows to %s"` on the status bar.

---

### W6: CLI File/`stdin` Integration & Sanitizer Test Suite (`src/tam-data.c`, `Makefile`, `tests/*`)

- **CLI Options & Input Sources (`src/tam-data.c`):**
  - Usage: `tam-data [options] [file ...]`
  - Flags:
    - `-d CHAR`, `--delimiter CHAR`: explicit field delimiter.
    - `--csv`: force comma delimiter (`','`).
    - `--tsv`: force tab delimiter (`'\t'`).
    - `--no-header`: treat the first line as data and synthesize column names (`col1`, `col2`, …).
    - `--demo`: open the built-in sample sheet.
    - `-v`, `--version`: print version and build ID.
    - `-h`, `--help`: print usage and Phase 1 keybinding summary.
  - Input resolution:
    1. For each positional `file` argument: if `strcmp(file, "-") == 0`, load from `STDIN_FILENO` via `td_csv_load_fd()`; otherwise load via `td_csv_load_file()`, pushing each sheet onto `stack`.
    2. If no positional arguments and no `--demo` are given and `!isatty(STDIN_FILENO)`, automatically load piped input from `STDIN_FILENO` via `td_csv_load_fd(STDIN_FILENO, "stdin", ...)`. Because `td_term_open()` opens `/dev/tty` directly, `cat data.csv | tam-data` works seamlessly in interactive mode.
    3. If no positional arguments are given and `isatty(STDIN_FILENO)`, open `td_demo_sheet()`.
- **Sanitizer Unit Test Suite (`tests/test_csv.c` + existing test files):**
  - `test_csv`:
    - RFC 4180 parsing: quoted fields with commas, doubled quotes `""`, embedded `\n` and `\r\n`, CRLF line endings, trailing newline vs no trailing newline, UTF-8 BOM stripping, ragged rows, and `--no-header` synthetic column names.
    - TSV parsing and delimiter auto-detection (`.csv` vs `.tsv` vs content sniffing).
    - `td_csv_load_file()` (`mmap` path) and `td_csv_load_fd()` (`pipe()` streaming path) verifying zero-copy slices and leak-free `td_sheet_free()`.
    - `td_csv_save_sheet()` atomic round-trip to a temporary directory (`mkdtemp`), verifying that hidden columns (`-`) are omitted, reordered columns (`H`/`L`) and `row_order` permutations are respected, and reloading the saved `.csv`/`.tsv` produces identical values.
  - `test_sheet`:
    - Value navigation (`td_sheet_jump_value` forward `>` and backward `<` across repeated and distinct values).
    - Blank-cell navigation (`td_sheet_jump_null` forward `}` and backward `{`).
    - Column reordering (`td_sheet_move_col` left/right skipping hidden columns) and width controls (`td_sheet_autosize_all_cols`, `td_sheet_set_col_width`, `td_sheet_unhide_all_cols`).
  - `test_input`:
    - Decoding `<`, `>`, `{`, `}`, `H`, `L`, `g_`, `gv`, `z_`, `z~`, and `Ctrl+S` (`19`), plus prompt kind tracking.
  - `test_term`:
    - Right-aligned numeric cells, `#ERR` cell styling, and status bar `r/c` coordinate rendering.

---

## 4. Implementation Sequence

1. **Step 1 (W1):** Implement `src/csv.h` and `src/csv.c` (parser + `mmap` and `fd` loaders) and add `mmap_base`/`mmap_len` cleanup to `src/sheet.*`; wire `src/csv.c` into `Makefile`.
2. **Step 2 (W2):** Add cached auto-width, value/null jumps (`<`, `>`, `{`, `}`), column reordering (`H`, `L`), and width controls (`g_`, `z_`, `gv`) to `src/sheet.*` with unit tests in `tests/test_sheet.c`.
3. **Step 3 (W3):** Extend `src/input.*` with Phase 1 command IDs, `g`/`z` chords, `Ctrl+S`, and `td_prompt_kind_t`, updating `tests/test_input.c`.
4. **Step 4 (W4):** Update `src/tam-data.c` viewport renderer for numeric right-alignment, error cell styling, and `r/c` status bar coordinates, updating `tests/test_term.c`.
5. **Step 5 (W5):** Implement `td_csv_format_sheet()` and `td_csv_save_sheet()` (`tam_fs_write_atomic`) in `src/csv.c` and add `tests/test_csv.c`.
6. **Step 6 (W6):** Wire CLI file/pipe loading, `Ctrl+S` save prompt, and `z_` width prompt into `src/tam-data.c`; run full `make && make test` under `ASan`/`UBSan`.

---

## 5. Phase 1 Completion Criteria (Exit Gate to Phase 2)

- [ ] `make` builds `build/tam-data` with zero warnings under `-Wall -Wextra -Wpedantic -Werror -std=c11`.
- [ ] `ldd build/tam-data` shows only standard C runtime libraries (`libc.so`, `ld-linux`), with `libtam.a` statically linked.
- [ ] `make test` passes 100% of unit tests (`arena`, `val`, `sheet`, `csv`, `term`, `input`) under AddressSanitizer and UndefinedBehaviorSanitizer with zero memory leaks.
- [ ] `build/tam-data file.csv` and `build/tam-data file.tsv` open files via `mmap` and render headers, types, and rows accurately.
- [ ] `cat file.csv | build/tam-data` loads piped data from `stdin` and opens the interactive UI on `/dev/tty`.
- [ ] Interactive navigation (`<`, `>`, `{`, `}`), column manipulation (`H`, `L`, `-`, `gv`, `_`, `g_`, `z_`, `^`, `#`, `%`, `$`, `@`, `~`, `z~`), and atomic save (`Ctrl+S`) work cleanly in both graphical terminals (`foot`) and `TERM=linux` virtual consoles.

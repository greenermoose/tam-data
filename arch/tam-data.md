# `tam-data` Architecture: Data Structures & Algorithms

This document describes the data structures, memory ownership model, and core algorithms implemented in our C11 version of `tam-data` (`src/`).

---

## 1. Memory Management & Value Representation

### 1.1 Bump-Pointer Arena (`src/arena.h`, `src/arena.c`)
- **`td_arena_t`** owns a singly-linked list of **`td_arena_block_t`** heap blocks (`next`, `cap`, `used`, flexible array `max_align_t data[]`), defaulting to `TD_ARENA_DEFAULT_BLOCK` (64 KiB).
- **`td_arena_alloc()`** rounds requested sizes up to `alignof(max_align_t)` via `td_align_up()` and bump-allocates from `head`. Allocations larger than `block_size` get a dedicated block prepended to `head`.
- **`td_arena_reset()`** frees all blocks except `head` and resets `head->used = 0` for fast reuse (used, for example, as a per-row scratch arena during expression evaluation).

### 1.2 Non-Owning Slices & Tagged Values (`src/val.h`, `src/val.c`)
- **`td_slice_t`**: `{ const char *data; uint32_t len; }` — a non-owning view into `mmap_base`, `buf_base`, or `sheet->arena` (`data == NULL` denotes absent).
- **`td_type_t`**: `TD_TYPE_ANY`, `TD_TYPE_STR`, `TD_TYPE_INT`, `TD_TYPE_FLOAT`, `TD_TYPE_CURRENCY`, `TD_TYPE_DATE`, `TD_TYPE_NULL`, `TD_TYPE_ERR`.
- **`td_val_t`**: 24-byte tagged union `{ td_type_t type; union { td_slice_t s; int64_t i; double f; } as; }`.

---

## 2. Sheet, Columnar Storage, Virtualization & Undo

### 2.1 Columnar Sheet Model (`src/sheet.h`, `src/sheet.c`)
- **`td_col_t`**: Stores data **column-major** (`td_val_t *cells` of capacity `row_cap`), along with `name` (in `sheet->arena`), `type`, `width` (`> 0` fixed, `0` hidden, `< 0` auto-width capped at `-width`), `cached_width`, `is_key` (pinned left), and up to `TD_MAX_COL_AGGS` (12) cached aggregation slots (`aggs[]`, `naggs`, `agg_dirty`, `agg_vals[]`).
- **`td_sheet_t`**: Owns `arena`, `mmap_base` (`munmap` on free) or `buf_base`, `cols`, `row_order`, `sel_bits`, `undo`/`redo` stacks, and optional type-specific `meta` (`meta_free` callback) for metasheets/plots/frequency tables.
- **`td_stack_t`**: Dynamic stack of `td_sheet_t *` sheets; the active sheet is `sheets[len - 1]`.

### 2.2 Row Indirection & Packed Bitset Selection (`src/sheet.c`)
- **Row-order indirection**: `row_order[vis_row]` maps visible row index to physical row index (`0 .. phys_nrows - 1`). Sorting, filtering, and row deletion permute or compact `row_order` without moving columnar `cells[]` payloads.
- **Selection bitset**: `sel_bits` (`uint64_t *`, `sel_words = (row_cap + 63) / 64`) tracks selected physical rows in 1 bit/row (`td_sheet_select_row`, `td_sheet_toggle_row`, etc.).

### 2.3 Column Width Sampling & Viewport Virtualization (`src/sheet.c`)
- **`td_compute_col_width()`** avoids scanning millions of rows by sampling up to `TD_WIDTH_SAMPLE_ROWS` (1000) initial rows plus `TD_WIDTH_VIEWPORT_ROWS` (128) rows starting at `top_row`, caching the result in `cached_width` (clamped between `TD_AUTO_WIDTH_MIN = 3` and `TD_AUTO_WIDTH_CAP = 40`).
- **`td_sheet_ensure_cursor_visible()`** computes horizontal scroll `left_col` while keeping `is_key` columns pinned on the left.

### 2.4 Multi-Kind Undo / Redo (`src/sheet.c`)
- **`td_undo_stack_t`** holds **`td_undo_entry_t`** records covering 8 `td_undo_kind_t` variants: `TD_UNDO_CELLS` (cell delta list), `TD_UNDO_ROWS` (snapshots `nrows`, `row_order`, `sel_bits`, `nselected`), `TD_UNDO_COL_META`, `TD_UNDO_COL_VIS_ALL`, `TD_UNDO_COL_SWAP`, `TD_UNDO_COL_TYPE`, `TD_UNDO_COL_ADD` (transfers `td_col_t` ownership), and `TD_UNDO_COL_AGGS`.
- **`td_undo_apply()`** swaps current state with the entry's saved state in place, allowing the same entry to move symmetrically between `undo` and `redo`.

---

## 3. Parsing, Type Inference & Serialization

### 3.1 Zero-Copy CSV/TSV (`src/csv.c`)
- **`td_csv_load_file()`** maps regular files read-only via `mmap(PROT_READ, MAP_PRIVATE)` (falling back to a growing heap buffer for pipes/stdin via `td_csv_load_fd_with_path()`).
- **`td_csv_detect_delimiter()`** inspects file extension, strips UTF-8 BOM, and counts quote-aware `,` vs `\t` on the first line (up to 4096 bytes).
- **`td_csv_parse_field()` / `td_csv_commit_field()`**: Unquoted/unescaped fields become zero-copy `td_slice_t` pointers directly into the `mmap` region; fields containing `""` or `\r\n` are unescaped into `sheet->arena` via `td_csv_unescape_quoted()`.
- **`td_csv_save_sheet()`** serializes pinned key columns first, then visible regular columns, using `tam_fs_write_atomic()`.

### 3.2 JSON & Extended Formats (`src/json.c`, `src/formats.c`)
- **`src/json.c`** implements a recursive-descent parser (`jparse_value`, `jparse_object`, `jparse_array`, `jparse_string_raw` with `\uXXXX` and UTF-16 surrogate pairs) building a temporary `jval_t` AST, then `jrows_to_sheet()` discovers the union of object keys, flattens nested values to JSON strings, and infers column types.
- **`src/formats.c`** handles format dispatch (`td_load_path`), INI via `tam_ini_parse_string`, `ps` output via `tam_exec`, and **fixed-width format (`td_fwf_load_mem`)** using a 2-pass column-occupancy bitmap `occ[x]` across non-blank lines to detect whitespace-delimited column spans.

### 3.3 Lazy Value Parsing & Civil Date Math (`src/val.c`, `src/utf8.h`)
- Untyped cells remain `TD_TYPE_ANY` slices until evaluated (`td_sheet_eval_cell` / `td_val_format` / `td_val_parse`).
- **`td_parse_double()`** handles currency (`$`, commas, accounting parentheses `(1,234.50)`).
- **`td_parse_date()`** uses Howard Hinnant's proleptic Gregorian `td_days_from_civil()` algorithm for ISO-8601 `YYYY-MM-DD[T| ]HH:MM[:SS[.frac]][Z|±HH[:]MM]` timestamps as well as bare epoch integers.
- **`td_val_cmp()`** defines a total order across types (`null < numeric/date < str/any < err`) with exact `int64_t` vs `long double` numeric comparison.
- **`src/utf8.h`** provides `td_utf8_decode()` (overlong/surrogate/range-checked, replacing invalid bytes with `U+FFFD`) and `td_utf8_width()`.

---

## 4. Sorting, Expressions, Aggregation & Joins

### 4.1 Stable Multi-Column Merge Sort (`src/sheet.c`)
- **`td_sheet_sort_cols()`** uses an iterative bottom-up **stable merge sort** (`td_merge_runs()`) over `row_order[]`, comparing multi-column keys with `td_sort_cmp()` (`td_sheet_eval_phys_cell` + `td_val_cmp`).

### 4.2 Expression Engine (`src/expr.h`, `src/expr.c`)
- **Lexer & Recursive-Descent Parser**: `td_expr_compile()` parses ternary `?:`, logical `||`/`&&`, comparisons, arithmetic, right-associative `**` (`td_parse_pow`), unary ops, `${name}`/`$0` column refs, and 20 built-in functions up to `TD_EXPR_MAX_DEPTH` (128). AST nodes live in `expr->arena`; literal `match()` regexes are pre-compiled once into `expr->regexes` (`regex_t`).
- **Evaluator**: `td_expr_eval()` evaluates the AST per row using a reusable scratch arena, short-circuiting `&&`/`||`, supporting date arithmetic and overflow-checked `int64` ops (`td_i64_add`/`sub`/`mul`).

### 4.3 Open-Addressing Hash Grouping, Aggregations & Joins (`src/agg.h`, `src/agg.c`)
- **`td_group_table_t`**: Open-addressing linear-probing hash table (70% load factor, `buckets[slot]` storing `group_idx + 1`) keyed by `td_val_t *keys` using FNV-1a (`td_fnv1a_step` / `td_val_hash`, which hashes equal numeric/date values canonically).
- **Aggregations (`td_agg_compute_rows`)**:
  - Single-pass `non_null`, `nulls`, `min`, `max`, `sum`.
  - **Welford's online algorithm** for numerically stable `mean` and `stdev`.
  - `td_group_table_t` for `distinct`.
  - `qsort` + linear interpolation for `median`, `pct90`, `pct95`, `pct99`.
- **Analytical Sheets**: `td_agg_freq_sheet()` (frequency histogram with `td_freq_meta_t` for dive), `td_agg_describe_sheet()`, `td_agg_pivot_sheet()` (3 group tables: `row_tbl`, `pval_tbl`, `cell_tbl`), `td_agg_melt_sheet()`, and `td_agg_join_sheets()` (hash-join on right sheet for `inner`, `outer`, `full`, `diff`, `extend`).

---

## 5. Plotting, Command Log, Clipboard & Terminal UI

- **Braille & ASCII Plotting (`src/plot.c`)**: `td_plot_sheet_build()` extracts up to `TD_PLOT_MAX_SERIES` (8) series; `td_plot_render()` renders onto a 2x4 Unicode Braille (`U+2800..U+28FF`) bitmask canvas when `use_braille && t->profile.utf8`, falling back to an ASCII density ramp (`" .:*#"`).
- **Command Log & Replay (`src/cmdlog.c`)**: `td_cmdlog_t` records executed commands in an arena-backed array (`td_cmdlog_entry_t`) and can replay them via `td_cmdlog_replay_sheet()`.
- **Clipboard Ring (`src/clipboard.c`)**: `td_clipboard_t` is a 32-slot circular ring buffer (`TD_YANK_RING_CAP = 32`), each entry owning a `td_arena_t` for copied cell/row values.
- **Double-Buffered Terminal Renderer & Input (`src/term.c`, `src/input.c`, `src/tam-data.c`)**:
  - `td_term_t` maintains `front` and `back` arrays of 8-byte `td_cell_t` cells (`codepoint`, `fg`, `bg`, `attr`). `td_term_present()` diffs `back` against `front`, emitting minimal cursor moves and SGR transitions (bracketed by `\x1b[?2026h/l` sync output when supported).
  - `td_input_feed()` runs a modal prefix FSM (`0` none, `1` `g`, `2` `z`, `3` `gz`) over `td_key_read()` (`poll()` with 25ms `esc_timeout_ms`).

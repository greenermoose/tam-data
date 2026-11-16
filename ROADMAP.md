# tam-data Roadmap

This document outlines the architectural plan and phased roadmap for `tam-data`: a pure-C, zero-Python rewrite of [VisiData](https://www.visidata.org/) built on the Tam C libraries (`libtam`) and designed to run natively on any Tamlinux system—including bare Linux virtual terminals (`TERM=linux`).

**Current implementation:** v1.0.1 implements all planned Phases 0–6: CSV/TSV loading (disk files and piped `stdin`), grid navigation, column controls, atomic CSV/TSV/JSON/JSONL/`.vd` saving, POSIX regex row selection and search, row deletion, filtered sheet extraction, stable typed single/multi-column sorting, in-place cell/row/column editing and duplication, a cross-sheet yank ring, per-sheet undo/redo, a command log with CLI batch/interactive replay, column aggregators (`+`/`z+`) with persistent footer summaries, frequency tables (`F`/`gF`) with histogram bars and `Enter`-to-dive extraction, a statistical Describe sheet (`I`), pivot (`W`) and melt (`M`), stack-based sheet joins (`&`), an embedded C expression evaluator (`td_expr`), computed/derived columns (`=`/`g=`), delimiter/regex column splitting (`:`/`;`), metasheets (`S`/`gS`, `C`/`gC`, `O`, Directory sheet), interactive help & command palette (`Ctrl+H`, `Space`), terminal plotting (`.`/`g.`), and extended format loaders (JSON, JSONL, INI, fixed-width tables, and process tables via `--ps`) alongside the built-in demo. Performance figures in this roadmap are targets; no startup, memory, or file-loading benchmarks have been published for `tam-data`.

---

## Architectural Principles

1. **Zero Python, Minimal Dependencies:**
   - Written in standard C11/POSIX, linking statically against `libtam` (`tam_str`, `tam_fs`, `tam_path`, `tam_exec`, `tam_ini`, `tam_vt`).
   - No Python interpreter, no GLib, and no heavy runtime dependencies.
2. **First-Class Linux VT & Terminal Compatibility:**
   - Direct `termios` and escape-sequence rendering (building on `libtam` and the terminal engine patterns established in `tam`).
   - Graceful adaptation across bare Linux virtual consoles (`TERM=linux`, 8/16 colors, CP437/UTF-8 box drawing) and graphical terminal emulators (`foot`, 256-color/truecolor, synchronized output).
3. **Fast Startup & Low Memory Footprint (Measured — see [docs/BENCHMARKS.md](docs/BENCHMARKS.md)):**
   - Primary `0–2 ms` PTY first-frame TUI startup target: `v1.0.0` achieves `0.81–1.01 ms` warm median (`~2.4 ms` cold median via `POSIX_FADV_DONTNEED`) at `~2.7 MiB` RSS.
   - Arena-allocated and `mmap`-backed string slices for raw tabular data: `1M`-row CSV (`48.5 MiB`) loads in `141.9 ms` (`202.1 MiB` peak RSS), while `1M`-row JSONL (`1037.6 MiB` RSS, `2.07 s`) and `describe_sheet` (`4.46 s`) are tracked as `v1.0.1` / `v1.1.0` optimization targets.
4. **VisiData Ergonomics & Muscle Memory:**
   - Preserve VisiData's modal keybindings (`g`, `z` prefixes), multi-sheet stack (`S`, `C`, `I`, `F`, `O`), non-destructive exploration, and command discoverability (`Ctrl+H`, `Space`).

---

## Release Versioning & SemVer Plan

`tam-data` adheres to [Semantic Versioning (SemVer 2.0.0)](https://semver.org/). Pre-1.0 minor releases track architectural milestones and feature sets, culminating in `v1.0.0` for full core parity and stable CLI interfaces:

| Version | Phase | Milestone Scope | Status |
| :--- | :--- | :--- | :--- |
| **`v0.1.0`** | **Phase 0** | Foundation, build/sanitizers, VT terminal engine, core data model, demo shell | Completed |
| **`v0.2.0`** | **Phase 1** | Native CSV/TSV parser, viewport rendering, grid navigation, atomic saver (**MVP**) | Completed |
| **`v0.3.0`** | **Phase 2** | Row selection bitset, regex search, filtering, and typed multi-column sorting | Completed |
| **`v0.4.0`** | **Phase 3** | In-place editing, clipboard/yank ring, undo/redo, command replay log | Completed |
| **`v0.5.0`** | **Phase 4** | Frequency tables (`F`), aggregation summaries (`+`), describe sheet (`I`), pivot/melt, joins | Completed |
| **`v0.6.0`** | **Phase 5** | Embedded C expression engine (`td_expr`), computed/derived columns | Completed |
| **`v1.0.0`** | **Phase 6** | Metasheets (`S`/`C`/`O`/Dir), command palette (`Space`/`Ctrl+H`), terminal plotting, extended formats | Completed |
| **`v1.0.1`** | **Perf Patch** | Reproducible benchmark harness, streaming JSONL, single-pass `describe`, CSV pre-sizing | In Progress |
| **`v1.1.0`** | **Perf Minor** | String sort key caching, compact `td_val_t` cell layout, cold-start tuning | Planned |

- **Patch Releases (`x.y.Z`)**: Bug fixes, terminal compatibility adjustments, performance enhancements, and sanitizer fixes without interface changes.
- **Minor Releases (`1.Y.0`)**: Post-`1.0.0` structural performance improvements and backwards-compatible feature additions.
- **Minor Releases (`0.Y.0`)**: Completion of each major roadmap phase leading up to `v1.0.0`.
- **Major Release (`1.0.0`)**: Production-ready general availability with full VisiData core parity, complete metasheet support, and stable CLI/scripting behaviour.

---

## Phase 0: Foundation, Build System & Terminal Engine (v0.1.0)

**Goal:** Establish the repository structure, build/test harness, terminal screen compositor, and core sheet data model.

- [x] **Build & Sanitizer Harness:**
  - `Makefile` integrating `pkg-config libtam`, strict compiler warnings (`-Wall -Wextra -Werror`), and `make test` running under AddressSanitizer (`ASan`) and UndefinedBehaviorSanitizer (`UBSan`).
- [x] **VT & Terminal Engine (`td_term`):**
  - Raw `termios` lifecycle, alternate screen switching, cursor visibility, `SIGWINCH` resize handling, and `SIGTSTP`/`SIGCONT` job control.
  - Double-buffered cell/line diffing renderer for flicker-free redraws on both Linux VTs and Wayland terminals.
  - Multi-key input state machine supporting prefix sequences (`g`, `z`, `gz`), arrow/function keys, and inline prompt editing.
- [x] **Core Data Model (`td_sheet`, `td_col`, `td_val`):**
  - `td_sheet_t`: Sheet metadata, row/column vectors, cursor coordinates, selection bitset, and parent/source lineage.
  - `td_val_t`: Tagged value representation (`raw` string slice, `int64`, `double`, `date`/`timestamp`, `currency`, `null`/`error`).
  - Global sheet stack (`td_stack_t`) for pushing, popping (`q`), and switching sheets.

---

## Phase 1: Native CSV/TSV Loaders & Grid Navigation (MVP / v0.2.0)

**Goal:** Open any CSV or TSV file from disk or `stdin`, navigate it fluidly in a VT or terminal, inspect columns, and save changes back to disk.

- [x] **Streaming & `mmap` CSV/TSV Parser:**
  - RFC 4180 compliant CSV parser (quoted fields, escaped quotes, embedded newlines, configurable delimiters) and fast TSV scanner.
  - Support reading from file paths and piped `stdin` (`cat data.csv | tam-data`).
- [x] **Sheet Viewport & Status Bar Renderer:**
  - Dynamic column width calculation with manual override (`_`), column truncation indicators (`…`), and type sigils in headers (`#` int, `%` float, `$` currency, `@` date, `~` string).
  - Pinned key columns (`!`) anchored on the left during horizontal scrolling.
  - VisiData-style status bar displaying sheet stack depth, sheet name, current row/col, total rows, selection count, pending keystrokes, and status messages.
- [x] **Core Navigation & Column Controls:**
  - Cursor movement: `h`/`j`/`k`/`l`, arrow keys, page up/down (`Ctrl+F`/`Ctrl+B`), home/end (`gh`/`gj`/`gk`/`gl`, `gg`/`G`).
  - Value navigation: Jump to next/previous distinct value in column (`<`, `>`) or blank cell (`{`, `}`).
  - Column manipulation: Hide column (`-`), adjust width (`_`, `g_`), rename column (`^`), reorder columns (`H`/`L`), and set column types (`#`, `%`, `$`, `@`, `~`).
- [x] **Atomic CSV/TSV Saver (`Ctrl+S`):**
  - Direct export of the current sheet (respecting visible columns and row ordering) to `.tsv` or `.csv` via `tam_fs_write_atomic`.

---

## Phase 2: Selection, Search, Filtering & Sorting (v0.3.0)

**Goal:** Enable interactive subsetting, regex searching, row selection, and multi-column sorting.

- [x] **Row Selection Bitset:**
  - Select (`s`), unselect (`u`), and toggle (`t`) current row; select/unselect/toggle all rows (`gs`, `gu`, `gt`).
  - Select/unselect rows matching regex in current column (`|`, `\`) or across all visible columns (`g|`, `g\`).
- [x] **Search Engine (`regex.h`):**
  - Forward (`/`) and backward (`?`) regex/substring search in current column or all visible columns (`g/`, `g?`), with `n`/`N` match stepping.
- [x] **Filtering & Sheet Extraction:**
  - Duplicate selected rows into a new pushed sheet (`"`).
  - Delete current row (`d`) or all selected rows (`gd`).
- [x] **Typed Sorting:**
  - Ascending (`[`) and descending (`]`) sort on current column using typed comparators (`int64`, `double`, `date`, `string`).
  - Multi-column sort (`g[`, `g]`) across pinned key columns (falling back to the current column).

---

## Phase 3: In-Place Editing, Clipboard, Undo/Redo & Command Log (v0.4.0)

**Goal:** Turn `tam-data` from an interactive explorer into a full tabular editor with reproducible command logs.

- [x] **Cell & Bulk Editing:**
  - Inline cell editing (`e`) with per-prompt history (`Up`/`Down`/`Ctrl+P`/`Ctrl+N`) and cursor movement; set value across all selected rows (`ge`); clear cell / selected cells (`zd`, `gzd`).
  - Insert blank row (`a`, `ga`), append blank column (`za`), and duplicate row/column (`g"`, `z"`).
- [x] **Yank, Paste & Deletion:**
  - Internal cross-sheet cell and row yank ring (`y`, `gy`, `zy`, `gzy`, `p`, `P`, `zp`) plus Vim-style delete-to-yank-ring (`d`, `gd`).
- [x] **Undo / Redo (`U` / `R`):**
  - Lossless reversible action log per sheet for cell edits, row insertions/deletions, column appends/hides/renames/reordering/type changes, and sorts.
- [x] **Command Log (`D`) & Replay (`-p`, `-b`):**
  - Record executed navigation and mutation commands into a tabular Command Log sheet (`cmdlog`) that can be viewed, edited, saved as a `.vd` / `.tsv` script, and replayed deterministically from the CLI (`-p`, optionally `-b` for batch mode).

---

## Phase 4: Aggregation, Frequency Tables, Pivot & Joins (v0.5.0)

**Goal:** Recreate VisiData's signature analytical workflows in native C.

- [x] **Frequency Table (`F`, `gF`):**
  - Group by current column (or key columns), computing count, percentage, and terminal histogram bars (`*`).
  - Pressing `Enter` on any frequency bin pushes a filtered sheet of the underlying source rows.
- [x] **Column Aggregators (`+`, `z+`):**
  - Built-in C aggregators: `count`, `distinct`, `nulls`, `sum`, `mean`, `median`, `min`, `max`, `stdev`, `pct90`, `pct95`, `pct99`.
  - One-off aggregation display (`z+`) and persistent footer summaries.
- [x] **Describe Sheet (`I`):**
  - Statistical summary sheet profiling every column's type distribution, missing count, min/max, mean, median, and standard deviation.
- [x] **Pivot (`W`) & Melt (`M`):**
  - Pivot key columns against a categorical column with aggregated values; unpivot (`melt`) wide tables into normalized rows.
- [x] **Sheet Joins (`&`):**
  - Join top two sheets on the stack by key columns (`inner`, `outer`, `full`, `diff`, `append`, `extend`).

---

## Phase 5: Native C Expression Engine & Derived Columns (v0.6.0)

**Goal:** Provide computed columns (`=`) and transformations without a Python interpreter.

- [x] **Embedded C Expression Evaluator (`td_expr`):**
  - AST-evaluated expression engine operating over row contexts:
    - Column references by name (`col_name`, `${col name}`) or index (`$0`, `$1`).
    - Arithmetic (`+`, `-`, `*`, `/`, `%`, `**`), relational (`==`, `!=`, `<`, `<=`, `>`, `>=`), logical (`&&`, `||`, `!`), and ternary (`? :`) operators.
    - Built-in string functions: `len()`, `lower()`, `upper()`, `trim()`, `substr()`, `replace()`, `contains()`, `match()`.
    - Built-in math & date functions: `abs()`, `round()`, `floor()`, `ceil()`, `min()`, `max()`, `log()`, `sqrt()`, `year()`, `month()`, `day()`, `datediff()`.
- [x] **Derived & Transformed Columns:**
  - Add computed formula column (`=`), set selected cells by expression (`g=`).
  - Split column by delimiter (`:`) or regex capture groups (`;`).

---

## Phase 6: Metasheets, Help System, Terminal Plotting & Extended Loaders (v1.0.0)

**Goal:** Complete the VisiData ecosystem experience and integrate deeply with Tamlinux.

- [x] **Metasheets:**
  - **Sheets Sheet (`S` / `gS`):** Inspect, reorder, drop, or join active sheets on the stack.
  - **Columns Sheet (`C` / `gC`):** Edit column names, widths, types, aggregators, and visibility as rows of a metasheet.
  - **Options Sheet (`O`):** View and modify runtime and persistent configuration (stored via `tam_xdg_path` and parsed with `tam_ini`).
  - **Directory Sheet:** Open a directory path in `tam-data` to inspect files, sizes, permissions, and timestamps as a sortable, filterable table, with `Enter` opening the selected file.
- [x] **Interactive Help & Command Palette (`Ctrl+H`, `Space`):**
  - Searchable command palette (`Space`) and context-sensitive help sheet listing all commands, keybindings, and descriptions.
- [x] **VT & Terminal Canvas / Plotting (`.` / `g.`):**
  - Terminal scatter plots and bar charts using Unicode Braille (`U+2800`..`U+28FF`) in graphical/UTF-8 terminals with automatic fallback to ASCII/block characters on bare Linux VTs.
- [x] **Additional Zero-Python Format Loaders:**
  - Fixed-width text tables, JSON / JSON Lines (`.json`, `.jsonl`), INI / key-value files (`tam_ini`), and direct process table output via `tam_exec`.

---

## v1.0.1: Benchmarking & Hotspot Performance Fixes (SemVer `1.0.1` — Patch)

**Goal:** Establish reproducible performance baselines and eliminate the highest-impact load and aggregation bottlenecks without changing data structures or CLI/file interfaces.

- [x] **Reproducible Benchmark Harness (`bench/bench_tam_data.c`, `docs/BENCHMARKS.md`):**
  - Zero-dependency C11 runner (`make bench`, `make bench-quick`) measuring warm/cold PTY first-frame startup, `100k`/`1M`-row CSV/TSV/JSONL load time and isolated `ru_maxrss`, and core analytical operations.
  - Recorded `v1.0.0` baselines: `0.81–1.01 ms` warm PTY startup (`~2.4 ms` cold), `141.9 ms` / `202.1 MiB` RSS for `1M.csv`, `2071.7 ms` / `1037.6 MiB` RSS for `1M.jsonl`, and `4464.2 ms` for `1M` `describe_sheet`.
- [ ] **Streaming JSONL Loader (`src/json.c`):**
  - Replace full-file `tam_fs_read_file()` + `jval_t` AST array buffering with `mmap` + line-by-line parsing into a reusable scratch arena and indexed column lookup.
  - **Target:** Reduce `1M.jsonl` peak RSS from `1037.6 MiB` to `<= 300 MiB` and load time from `2.07 s` to `<= 500 ms`.
- [ ] **Single-Pass `describe_sheet` (`src/agg.c`):**
  - Compute per-column count, nulls, min, max, mean, stdev, and median in a single pass instead of repeated `td_agg_compute_rows()` scans.
  - **Target:** Reduce `1M` × 6-column `describe_sheet` from `4.46 s` to `< 1.0 s`.
- [ ] **CSV Row Pre-Sizing & `madvise` (`src/csv.c`, `src/sheet.c`):**
  - Apply `madvise(MADV_SEQUENTIAL)` on `mmap` inputs and estimate initial `row_cap` from file size / header-line width to eliminate ~15 `realloc()` doubling passes per column on `1M`-row files.
- [ ] **Benchmark Refresh & Release (`v1.0.1`):**
  - Re-run `make bench`, update [docs/BENCHMARKS.md](docs/BENCHMARKS.md) with `v1.0.1` before/after deltas, and verify `make test` (ASan/UBSan) and `make smoke`.

---

## v1.1.0: Structural Memory & Sorting Performance (SemVer `1.1.0` — Minor)

**Goal:** Reduce per-cell memory footprint and accelerate string-heavy analytical operations for multi-million-row datasets.

- [ ] **String Sort Key Caching (`src/sheet.c`):**
  - Pre-extract/cache string slices across `nrows` before `qsort` in `td_sheet_sort_col()` instead of re-evaluating cells inside every comparator call.
  - **Target:** Reduce `1M`-row `sort_col(str)` from `3.01 s` toward `< 1.0 s`.
- [ ] **Compact `td_val_t` Cell Layout (`src/val.h`):**
  - Evaluate shrinking `td_val_t` from 24 bytes to 16 bytes (e.g., packed tag/length or NaN-boxing) to cut cell-array RSS by ~33% (`144 MiB` → `~96 MiB` for 6M cells).
- [ ] **Cold-Start & Render-Path Tuning:**
  - Profile cold-cache PTY first-frame startup (`~2.4 ms` baseline) and viewport rendering to keep even cold TUI startup reliably under `2.0 ms`.
- [ ] **Benchmark Refresh & Release (`v1.1.0`):**
  - Re-run `make bench`, record `v1.1.0` baselines in [docs/BENCHMARKS.md](docs/BENCHMARKS.md), and verify `make test` and `make smoke`.

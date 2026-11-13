# Changelog

All notable changes to `tam-data` are documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-13

### Added
- **Phase 6 — Metasheets, Help, Plotting & Extended Loaders:**
  - Metasheets: Sheets (`S`/`gS`), Columns (`C`/`gC`), Options (`O` with `tam_ini` persistence via `tam_xdg_path`), and Directory browser sheet.
  - Searchable command palette (`Space`) and interactive help sheet (`Ctrl+H`).
  - Terminal scatter plots and bar charts (`.`/`g.`) using Unicode Braille (`U+2800`..`U+28FF`) with automatic ASCII/block fallback for Linux virtual consoles (`TERM=linux`).
  - Extended zero-Python format loaders: JSON (`.json`), JSON Lines (`.jsonl`), INI (`.ini` via `tam_ini`), fixed-width tables (`.fwf`), and process table inspection (`--ps` via `tam_exec`).
  - CLI end-to-end smoke tests (`tests/smoke.sh` / `make smoke`).
- **Phase 5 (`v0.6.0`) — Native C Expression Engine:**
  - Embedded AST expression evaluator (`td_expr`) supporting column references, arithmetic/relational/logical/ternary operators, and built-in string, math, and date functions.
  - Computed columns (`=`), selected-cell expression assignment (`g=`), and delimiter/regex column splitting (`:`/`;`).
- **Phase 4 (`v0.5.0`) — Aggregation, Frequency Tables, Pivot & Joins:**
  - 12 column aggregators (`+`/`z+`) with persistent footer summaries.
  - Frequency tables (`F`/`gF`) with histogram bars and `Enter` drill-down, statistical Describe sheet (`I`), Pivot (`W`), Melt (`M`), and multi-mode sheet joins (`&`).
- **Phase 3 (`v0.4.0`) — Editing, Clipboard, Undo/Redo & Command Log:**
  - In-place cell/row/column editing, duplication, and deletion; cross-sheet yank ring (`y`/`p`/`d`); lossless per-sheet undo/redo (`U`/`R`); command log (`D`) with `.vd` script export and CLI replay (`-p`/`-b`).
- **Phase 2 (`v0.3.0`) — Selection, Search, Filtering & Sorting:**
  - Row selection bitset (`s`/`u`/`t`/`gs`/`gu`/`gt`), POSIX regex selection and search (`|`/`\`/`/`/`?`/`n`/`N`), filtered sheet extraction (`"`), and stable typed single/multi-column sorting (`[`/`]`/`g[`/`g]`).

## [0.2.0] - 2026-11-12

### Added
- **Phase 1 — CSV/TSV Viewer MVP:** `mmap`-backed RFC 4180 CSV/TSV parser, `stdin` support, viewport & status bar renderer, grid/value navigation, column controls, and atomic saving (`Ctrl+S`) via `tam_fs_write_atomic`.
- **Phase 0 — Foundation:** Strict C11 build with `libtam`, ASan/UBSan test harness, `td_term` double-buffered VT renderer, arena allocator, and core sheet/value data model.

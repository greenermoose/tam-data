# tam-data Roadmap

This document outlines the architectural plan and phased roadmap for `tam-data`: a pure-C, zero-Python rewrite of [VisiData](https://www.visidata.org/) built on the Tam C libraries (`libtam`) and designed to run natively on any Tamlinux system—including bare Linux virtual terminals (`TERM=linux`).

---

## Architectural Principles

1. **Zero Python, Minimal Dependencies:**
   - Written in standard C11/POSIX, linking statically against `libtam` (`tam_str`, `tam_fs`, `tam_path`, `tam_exec`, `tam_ini`, `tam_vt`).
   - No Python interpreter, no GLib, and no heavy runtime dependencies.
2. **First-Class Linux VT & Terminal Compatibility:**
   - Direct `termios` and escape-sequence rendering (building on `libtam` and the terminal engine patterns established in `tam`).
   - Graceful adaptation across bare Linux virtual consoles (`TERM=linux`, 8/16 colors, CP437/UTF-8 box drawing) and graphical terminal emulators (`foot`, 256-color/truecolor, synchronized output).
3. **Instantaneous Startup & Low Memory Footprint:**
   - Sub-millisecond cold start (`0–2 ms`).
   - Arena-allocated and `mmap`-backed zero-copy string slices for raw tabular data so multi-megabyte CSV/TSV files open immediately without per-cell heap bloat.
4. **VisiData Ergonomics & Muscle Memory:**
   - Preserve VisiData's modal keybindings (`g`, `z` prefixes), multi-sheet stack (`S`, `C`, `I`, `F`, `O`), non-destructive exploration, and command discoverability (`Ctrl+H`, `Space`).

---

## Phase 0: Foundation, Build System & Terminal Engine

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

## Phase 1: Native CSV/TSV Loaders & Grid Navigation (MVP)

**Goal:** Open any CSV or TSV file from disk or `stdin`, navigate it fluidly in a VT or terminal, inspect columns, and save changes back to disk.

- [ ] **Streaming & `mmap` CSV/TSV Parser:**
  - RFC 4180 compliant CSV parser (quoted fields, escaped quotes, embedded newlines, configurable delimiters) and fast TSV scanner.
  - Support reading from file paths and piped `stdin` (`cat data.csv | tam-data`).
- [ ] **Sheet Viewport & Status Bar Renderer:**
  - Dynamic column width calculation with manual override (`_`), column truncation indicators (`…`), and type sigils in headers (`#` int, `%` float, `$` currency, `@` date, `~` string).
  - Pinned key columns (`!`) anchored on the left during horizontal scrolling.
  - VisiData-style status bar displaying sheet stack depth, sheet name, current row/col, total rows, selection count, pending keystrokes, and status messages.
- [ ] **Core Navigation & Column Controls:**
  - Cursor movement: `h`/`j`/`k`/`l`, arrow keys, page up/down (`Ctrl+F`/`Ctrl+B`), home/end (`gh`/`gj`/`gk`/`gl`, `gg`/`G`).
  - Value navigation: Jump to next/previous distinct value in column (`<`, `>`) or blank cell (`{`, `}`).
  - Column manipulation: Hide column (`-`), adjust width (`_`, `g_`), rename column (`^`), reorder columns (`H`/`L`), and set column types (`#`, `%`, `$`, `@`, `~`).
- [ ] **Atomic CSV/TSV Saver (`Ctrl+S`):**
  - Direct export of the current sheet (respecting visible columns and row ordering) to `.tsv` or `.csv` via `tam_fs_write_atomic`.

---

## Phase 2: Selection, Search, Filtering & Sorting

**Goal:** Enable interactive subsetting, regex searching, row selection, and multi-column sorting.

- [ ] **Row Selection Bitset:**
  - Select (`s`), unselect (`u`), and toggle (`t`) current row; select/unselect/toggle all rows (`gs`, `gu`, `gt`).
  - Select/unselect rows matching regex in current column (`|`, `\`) or across all visible columns (`g|`, `g\`).
- [ ] **Search Engine (`regex.h`):**
  - Forward (`/`) and backward (`?`) regex/substring search in current column, key columns, or all columns (`g/`, `g?`), with `n`/`N` match stepping.
- [ ] **Filtering & Sheet Extraction:**
  - Duplicate selected rows into a new pushed sheet (`"`).
  - Delete current row (`d`) or all selected rows (`gd`).
- [ ] **Typed Sorting:**
  - Ascending (`[`) and descending (`]`) sort on current column using typed comparators (`int64`, `double`, `date`, `string`).
  - Multi-column sort (`g[`, `g]`) across key columns or selected column sets.

---

## Phase 3: In-Place Editing, Clipboard, Undo/Redo & Command Log

**Goal:** Turn `tam-data` from an interactive explorer into a full tabular editor with reproducible command logs.

- [ ] **Cell & Bulk Editing:**
  - Inline cell editing (`e`) with history and cursor movement; set value across all selected rows (`ge`).
  - Insert blank row (`a`, `ga`), append column (`za`), and duplicate row/column.
- [ ] **Yank, Paste & Deletion:**
  - Internal cell and row yank ring (`y`, `gy`, `p`, `P`).
- [ ] **Undo / Redo (`U` / `R`):**
  - Reversible action log per sheet for cell edits, row deletions, column hides, and sorts.
- [ ] **Command Log (`D`) & Replay (`-p`):**
  - Record executed commands into a tabular Command Log sheet (`cmdlog`) that can be viewed, edited, saved as a `.vd` / `.tsv` script, and replayed deterministically from the CLI.

---

## Phase 4: Aggregation, Frequency Tables, Pivot & Joins

**Goal:** Recreate VisiData's signature analytical workflows in native C.

- [ ] **Frequency Table (`F`, `gF`):**
  - Group by current column (or key columns), computing count, percentage, and terminal histogram bars (`*`).
  - Pressing `Enter` on any frequency bin pushes a filtered sheet of the underlying source rows.
- [ ] **Column Aggregators (`+`, `z+`):**
  - Built-in C aggregators: `count`, `distinct`, `nulls`, `sum`, `mean`, `median`, `min`, `max`, `stdev`, `pct90`, `pct95`, `pct99`.
  - One-off aggregation display (`z+`) and persistent footer summaries.
- [ ] **Describe Sheet (`I`):**
  - Statistical summary sheet profiling every column's type distribution, missing count, min/max, mean, median, and standard deviation.
- [ ] **Pivot (`W`) & Melt (`M`):**
  - Pivot key columns against a categorical column with aggregated values; unpivot (`melt`) wide tables into normalized rows.
- [ ] **Sheet Joins (`&`):**
  - Join top two sheets on the stack by key columns (`inner`, `outer`, `full`, `diff`, `append`, `extend`).

---

## Phase 5: Native C Expression Engine & Derived Columns

**Goal:** Provide computed columns (`=`) and transformations without a Python interpreter.

- [ ] **Embedded C Expression Evaluator (`td_expr`):**
  - Bytecode-compiled or AST-evaluated expression engine operating over row contexts:
    - Column references by name (`col_name`, `${col name}`) or index (`$0`, `$1`).
    - Arithmetic (`+`, `-`, `*`, `/`, `%`, `**`), relational (`==`, `!=`, `<`, `<=`, `>`, `>=`), logical (`&&`, `||`, `!`), and ternary (`? :`) operators.
    - Built-in string functions: `len()`, `lower()`, `upper()`, `trim()`, `substr()`, `replace()`, `contains()`, `match()`.
    - Built-in math & date functions: `abs()`, `round()`, `floor()`, `ceil()`, `min()`, `max()`, `log()`, `sqrt()`, `year()`, `month()`, `day()`, `datediff()`.
- [ ] **Derived & Transformed Columns:**
  - Add computed formula column (`=`), set selected cells by expression (`g=`).
  - Split column by delimiter (`:`) or regex capture groups (`;`).

---

## Phase 6: Metasheets, Help System, Terminal Plotting & Extended Loaders

**Goal:** Complete the VisiData ecosystem experience and integrate deeply with Tamlinux.

- [ ] **Metasheets:**
  - **Sheets Sheet (`S` / `gS`):** Inspect, reorder, drop, or join active sheets on the stack.
  - **Columns Sheet (`C` / `gC`):** Edit column names, widths, types, aggregators, and visibility as rows of a metasheet.
  - **Options Sheet (`O`):** View and modify runtime and persistent configuration (stored via `tam_xdg_path` and parsed with `tam_ini`).
  - **Directory Sheet:** Open a directory path in `tam-data` to inspect files, sizes, permissions, and timestamps as a sortable, filterable table, with `Enter` opening the selected file.
- [ ] **Interactive Help & Command Palette (`Ctrl+H`, `Space`):**
  - Searchable command palette (`Space`) and context-sensitive help sheet listing all commands, keybindings, and descriptions.
- [ ] **VT & Terminal Canvas / Plotting (`.` / `g.`):**
  - Terminal scatter plots and bar charts using Unicode Braille (`U+2800`..`U+28FF`) in graphical/UTF-8 terminals with automatic fallback to ASCII/block characters on bare Linux VTs.
- [ ] **Additional Zero-Python Format Loaders:**
  - Fixed-width text tables, JSON / JSON Lines (`.json`, `.jsonl`), INI / key-value files (`tam_ini`), and direct process table output via `tam_exec`.

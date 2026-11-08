# Phase 0 Workplan: Foundation, Build System & Terminal Engine

This workplan defines the concrete engineering tasks, module boundaries, data structures, and verification criteria to implement **Phase 0** of [`ROADMAP.md`](../ROADMAP.md).

---

## 1. Phase 0 Objective

Deliver a self-contained, warning-free, sanitizer-clean C11 foundation for `tamdata` that:
1. Links statically against `libtam` (`>= 0.4.0`) with **zero external shared library dependencies beyond `libc`**.
2. Provides a double-buffered terminal compositor (`td_term`) that runs cleanly on both bare Linux virtual consoles (`TERM=linux`, `console_codes(4)`) and graphical terminal emulators (`foot`, `xterm-256color`, `tmux-256color`).
3. Implements the multi-key input state machine (`td_input`) for VisiData prefix chords (`g`, `z`, `gz`), special keys, and bottom-line prompt editing.
4. Implements the core in-memory tabular data structures (`td_arena_t`, `td_val_t`, `td_col_t`, `td_sheet_t`, `td_stack_t`).
5. Boots into a minimal interactive `tamdata` shell rendering a grid viewport, column headers with type sigils, cursor navigation, and the VisiData status bar, backed by headless unit tests under `ASan` and `UBSan`.

---

## 2. Target Directory & File Layout

```text
~/Code/tamdata/
├── Makefile
├── README.md
├── ROADMAP.md
├── plans/
│   └── P0-WORKPLAN.md
├── src/
│   ├── version.h       # Version constants and build metadata macros
│   ├── arena.h         # Fast bump/page arena allocator for strings and row slices
│   ├── arena.c
│   ├── val.h           # Tagged cell values (td_val_t), type sigils, formatting
│   ├── val.c
│   ├── sheet.h         # Columns (td_col_t), sheets (td_sheet_t), stack (td_stack_t)
│   ├── sheet.c
│   ├── term.h          # Raw termios, VT/ANSI profiles, double-buffered cell grid
│   ├── term.c
│   ├── input.h         # Escape parser, UTF-8 decoder, prefix chords, line prompt
│   ├── input.c
│   └── tamdata.c       # CLI entry point, main event loop, minimal grid & status bar
└── tests/
    ├── test_main.c     # Test runner entry point
    ├── test_arena.c    # Arena allocation, alignment, and reset/free tests
    ├── test_val.c      # Value parsing, type conversion, and sigil formatting tests
    ├── test_sheet.c    # Sheet, column, row indirection, selection bitset, stack tests
    ├── test_term.c     # Headless cell buffer drawing, clipping, and ANSI diff tests
    └── test_input.c    # Escape sequence decoding and prefix chord state machine tests
```

---

## 3. Work Items

### W1: Build System & `libtam` Integration (`Makefile`, `src/version.h`)

- **Compiler & Flags:**
  - `CC ?= cc`
  - `CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -Werror -std=c11`
  - Feature macros: `-D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE` (matching `tam.h`).
- **`libtam` Discovery:**
  - Resolve `libtam` (`>= 0.4.0`) via `pkg-config libtam` or `LIBTAM_PREFIX` (with a clear `libtam-check` preflight rule matching `tam/Makefile`).
  - Default `PREFIX` to `$(HOME)/.local/dev` when Nix/Home Manager is active (`NIX_PROFILES` or `~/.nix-profile`), otherwise `/usr/local`.
- **Zero-Bloat Linkage:**
  - Link `libtam.a` statically; require no `ncurses`, `glib`, or Python libraries.
  - Compiler flag tracking via `build/.flags` so changing `CFLAGS` or `PREFIX` triggers a clean rebuild.
- **Targets:**
  - `make` / `make all` / `make command`: builds `build/tamdata`.
  - `make test`: compiles unit tests and `src/` objects with `-fsanitize=address,undefined -fno-omit-frame-pointer -g` into `build/test_tamdata` and runs the suite.
  - `make install` / `make uninstall` / `make clean`.

---

### W2: Arena Allocator & Tagged Value Representation (`src/arena.*`, `src/val.*`)

- **Arena Allocator (`td_arena_t`):**
  - Chained block allocator (default 64 KiB pages, oversized allocations handled in dedicated blocks) supporting `td_arena_alloc()`, `td_arena_strdup()`, and `td_arena_strndup()`.
  - Enables zero-fragmentation storage of string slices and sheet metadata, freed in $O(\text{blocks})$ when a sheet is closed.
- **String Slice (`td_slice_t`):**
  - `{ const char *data; uint32_t len; }` — supports both arena-interned strings and zero-copy slices pointing directly into `mmap`-backed CSV/TSV buffers ( readied for Phase 1).
- **Tagged Value (`td_val_t`):**
  - Type enumeration (`td_type_t`):
    - `TD_TYPE_ANY` (`' '` sigil — untyped/raw string slice)
    - `TD_TYPE_STR` (`'~'` sigil — explicit string)
    - `TD_TYPE_INT` (`'#'` sigil — `int64_t`)
    - `TD_TYPE_FLOAT` (`'%'` sigil — `double`)
    - `TD_TYPE_CURRENCY` (`'$'` sigil — `double` / fixed-point display)
    - `TD_TYPE_DATE` (`'@'` sigil — `int64_t` UTC epoch seconds/milliseconds)
    - `TD_TYPE_NULL` (empty/missing value)
    - `TD_TYPE_ERR` (`'!'` error indicator)
  - Functions:
    - `const char *td_type_sigil(td_type_t t)`
    - `td_val_t td_val_parse(td_slice_t raw, td_type_t target_type)`
    - `size_t td_val_format(td_val_t v, td_type_t col_type, char *buf, size_t cap)`
    - `int td_val_cmp(td_val_t a, td_val_t b)` (type-aware comparator with null/error ordering).

---

### W3: Core Sheet, Column, Selection & Stack Model (`src/sheet.*`)

- **Column (`td_col_t`):**
  - `char *name`: column title.
  - `td_type_t type`: display and sort type.
  - `int width`: rendered character width (`0` = hidden via `-`, `< 0` = auto-computed up to max cap).
  - `bool is_key`: pinned key column flag (`!`).
  - `td_val_t *cells`: contiguous cell array of length `row_cap` (or column-oriented storage).
- **Sheet (`td_sheet_t`):**
  - `char *name` and `char *source_path`.
  - `td_arena_t arena`: per-sheet memory arena.
  - `td_col_t *cols`, `size_t ncols`, `size_t col_cap`.
  - `size_t nrows`, `size_t row_cap`.
  - `size_t *row_order`: indirection array (`0 .. nrows-1`) mapping visible row index to physical row index so sorting and filtering never copy cell payloads.
  - `uint64_t *sel_bits`: packed bitset of selected physical rows + `size_t nselected` counter (`td_sheet_select_row`, `td_sheet_unselect_row`, `td_sheet_toggle_row`, `td_sheet_is_selected`).
  - Cursor & viewport state: `size_t cursor_row`, `size_t cursor_col`, `size_t top_row`, `size_t left_col`.
  - Viewport layout helper `td_sheet_ensure_Cursor_visible(td_sheet_t *s, int grid_rows, int grid_cols)` accounting for pinned key columns (`is_key`) on the left and hidden columns (`width == 0`).
- **Sheet Stack (`td_stack_t`):**
  - Dynamic array of `td_sheet_t *` sheets (`td_stack_push`, `td_stack_pop`, `td_stack_active`, `td_stack_free`).

---

### W4: VT & Terminal Double-Buffered Compositor (`src/term.*`)

- **Capability & Environment Detection (`td_term_profile_t`):**
  - Inspects `TERM` and `LANG`/`LC_CTYPE`:
    - `TERM=linux` (bare Linux virtual console): 16-color SGR (`30..37`, `40..47`, `1` bold), skips xterm `?1049h` alt-screen and `?2026h` sync output, uses full-screen clear on entry/exit.
    - Modern terminals (`foot*`, `xterm*`, `tmux*`, `screen*`, `alacritty*`, `kitty*`): enables `?1049h` alternate screen, 256-color palette, and `?2026h` synchronized frame updates.
    - Glyph table (`td_glyphs_t`): selects UTF-8 box-drawing/truncation glyphs (`│`, `─`, `…`, `•`) when UTF-8 locale is active, or pure ASCII (`|`, `-`, `~`, `*`) on restricted consoles.
- **Terminal Lifecycle & Crash-Safe Restoration:**
  - `td_term_open()` / `td_term_close()`: puts terminal fd (`/dev/tty` or `STDIN_FILENO`/`STDOUT_FILENO`) into raw `termios` mode, queries window dimensions via `ioctl(TIOCGWINSZ)`.
  - Signal handling (`SIGWINCH`, `SIGTSTP`, `SIGCONT`, `SIGINT`, `SIGTERM`): async-signal-safe flags (`volatile sig_atomic_t`) plus emergency `tcsetattr` restoration so a VT is never left in raw/no-echo mode.
- **Double-Buffered Cell Compositor:**
  - `td_cell_t`: `{ uint32_t ch; uint8_t fg; uint8_t bg; uint8_t attr; }` (`TD_ATTR_BOLD`, `TD_ATTR_REV`, `TD_ATTR_UNDERLINE`, `TD_ATTR_DIM`).
  - `front` and `back` buffers of size `rows * cols`.
  - Drawing primitives: `td_term_clear_back()`, `td_term_put_cell()`, `td_term_put_str()` (UTF-8 aware with column clipping and ellipsis truncation), `td_term_fill_rect()`.
  - `td_term_present()`: diffs `back` against `front`, formats minimal cursor moves and SGR transitions into a single `tam_str_t` frame buffer, writes once, and swaps/copies `back` to `front`.

---

### W5: Multi-Key Input State Machine & Prompt Editor (`src/input.*`)

- **Key & Escape Decoder (`td_key_t`):**
  - Reads from terminal fd using `poll()` with a short timeout (e.g., 25 ms) after a bare `0x1B` (`ESC`) to distinguish an isolated `Esc` press from CSI (`ESC [`) and SS3 (`ESC O`) sequences.
  - Supports both standard ANSI/xterm sequences and Linux VT (`TERM=linux`) keypad/function sequences (`ESC [[A`..`E`, `ESC [1~`..`6~`, `ESC [A`..`D`, `Delete`, `Backspace`, `Home`, `End`, `PgUp`, `PgDn`).
  - Decodes multi-byte UTF-8 input into Unicode codepoints.
- **Prefix Chord State Machine (`td_input_t`):**
  - Tracks pending prefix keys (`g`, `z`, `gz`) and exposes `const char *td_input_pending(const td_input_t *)` for the status bar.
  - Resolves complete chords into canonical command IDs (`td_cmd_t`), e.g.:
    - Movement: `h`/`j`/`k`/`l`, `gh`/`gj`/`gk`/`gl`, `gg`/`G`, `Ctrl+F`/`Ctrl+B`.
    - Column actions: `-` (hide), `!` (toggle key), `_` (auto-width), `^` (rename), `#`/`%`/`$`/`@`/`~` (set type).
    - Sheet actions: `q` (pop sheet), `gq` (quit all).
- **Bottom-Line Prompt Editor (`td_prompt_t`):**
  - Modal single-line editor rendered on the bottom status line (used in Phase 0 for `^` column rename and ready in Phase 1–2 for `/`, `?`, `:`, `e`, `Ctrl+S`).
  - Supports cursor left/right, `Home`/`End` (`Ctrl+A`/`Ctrl+E`), `Backspace`, `Delete`, `Ctrl+U` (kill line), `Ctrl+W` (kill word), `Enter` (accept), and `Esc`/`Ctrl+C` (cancel).

---

### W6: Minimal Interactive Shell & Phase 0 Verification (`src/tamdata.c`, `tests/*`)

- **Interactive Grid & Status Bar (`src/tamdata.c`):**
  - Renders the active `td_sheet_t` on `td_term_t`:
    - Top row: Column headers with bold highlight on `cursor_col`, key separator (`║` or `|`), and type sigils (`#`, `%`, `$`, `@`, `~`).
    - Body rows: Pinned key columns on the left, horizontally scrolled regular columns on the right, distinct styling for cursor row, cursor cell (reverse video), and selected rows (`s`/`t`).
    - Bottom row: VisiData status bar showing `[sheet_index/total] sheet_name` on the left, transient status/prompt message in the center, and `pending_keys | row_count rows (selected_count selected)` on the right.
  - CLI options:
    - `tamdata --version` / `tamdata -v`
    - `tamdata --help` / `tamdata -h`
    - `tamdata` (or `tamdata --demo`): opens a built-in starter/sample sheet so navigation, column resizing/hiding/renaming/typing, row selection, and VT rendering can be exercised interactively before Phase 1's CSV/TSV file parser lands.
- **Sanitizer Unit Test Suite (`tests/*`):**
  - `test_arena`: block growth, alignment, `strdup`/`strndup`, reset, and free under ASan.
  - `test_val`: integer, float, currency, date, and string parsing, formatting, truncation, and typed comparisons.
  - `test_sheet`: adding rows/columns, row indirection permutations, selection bitset operations, key-column pinning, and viewport scrolling math.
  - `test_term`: headless rendering of a sheet into an in-memory `td_term_t` buffer (80×24 Linux VT profile and 120×40 UTF-8 profile) verifying exact header cells, clipping, ellipsis markers, and diff output.
  - `test_input`: feeding raw byte streams (normal keys, UTF-8 characters, Linux VT and xterm arrow/page sequences, bare `ESC`, and `g`/`z` prefix chords) and asserting decoded `td_cmd_t` events and prompt edits.

---

## 4. Implementation Sequence

1. **Step 1 (W1):** Create `Makefile` and `src/version.h`; verify `libtam` static detection and empty test runner build under ASan/UBSan.
2. **Step 2 (W2):** Implement `src/arena.*` and `src/val.*` with `tests/test_arena.c` and `tests/test_val.c`.
3. **Step 3 (W3):** Implement `src/sheet.*` with `tests/test_sheet.c`.
4. **Step 4 (W4):** Implement `src/term.*` with headless buffer tests in `tests/test_term.c`.
5. **Step 5 (W5):** Implement `src/input.*` with byte-stream and prompt tests in `tests/test_input.c`.
6. **Step 6 (W6):** Implement `src/tamdata.c` tying the event loop, grid renderer, and status bar together; run full `make && make test`.

---

## 5. Phase 0 Completion Criteria (Exit Gate to Phase 1)

- [ ] `make` builds `build/tamdata` with zero warnings under `-Wall -Wextra -Wpedantic -Werror -std=c11`.
- [ ] `ldd build/tamdata` shows only standard C runtime libraries (`libc.so`, `ld-linux`), with `libtam.a` statically linked.
- [ ] `make test` passes 100% of unit tests (`arena`, `val`, `sheet`, `term`, `input`) with AddressSanitizer and UndefinedBehaviorSanitizer enabled and zero memory leaks.
- [ ] `build/tamdata --version` and `build/tamdata --help` exit `0` with clean output.
- [ ] Interactive launch (`build/tamdata`) renders the grid, headers, and status bar cleanly in both a graphical terminal (`foot`) and a `TERM=linux` environment, supporting cursor movement (`h`/`j`/`k`/`l`, `gh`/`gj`/`gk`/`gl`, `gg`/`G`), column controls (`!`, `-`, `_`, `^`, `#`/`%`/`$`/`~`), row selection (`s`/`u`/`t`), window resize (`SIGWINCH`), and clean exit (`q`).

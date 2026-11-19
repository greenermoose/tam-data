# Phase 2 Workplan: Selection, Search, Filtering & Sorting

This workplan defines the concrete engineering tasks, module boundaries, data structures, and verification criteria to implement **Phase 2 (`v0.3.0`)** of [`ROADMAP.md`](../ROADMAP.md).

---

## 1. Phase 2 Objective

Build on the Phase 1 CSV/TSV viewer MVP (`v0.2.0`) to turn `tam-data` into an interactive data subsetting, searching, and sorting workbench:
1. Decouple physical row storage (`phys_nrows`) from active visible row indices (`nrows` via `row_order[]`) so sorting and row deletion cooperate cleanly with the `sel_bits` selection bitset.
2. Implement POSIX extended regular expression (`<regex.h>`) row selection (`|`, `\`, `g|`, `g\`) and interactive forward/backward search (`/`, `?`, `g/`, `g?`, `n`, `N`).
3. Implement row deletion (`d`, `gd`) and filtered sheet extraction (`"`), deep-copying extracted slices into an independent child sheet's `td_arena_t` and warning `"no rows selected"` when `nselected == 0`.
4. Implement a standard C11 stable merge sort over `row_order[]` for single-column (`[`, `]`) and multi-column key sorting (`g[`, `g]`, falling back to `cursor_col` when no key columns are pinned, and reversing strict `td_val_cmp()` order on descending so `TD_TYPE_NULL` flips to last).
5. Maintain zero-warning `-std=c11 -Wall -Wextra -Wpedantic -Werror` compilation, static `libtam` linkage, and 100% leak-free `ASan`/`UBSan` test coverage.

---

## 2. Target Files

- `plans/P2-workplan.md` (this workplan)
- `src/sheet.h`, `src/sheet.c` — `phys_nrows` tracking, per-sheet search state, regex selection/search, row deletion, filtered extraction, stable merge sort
- `src/input.h`, `src/input.c` — Phase 2 `td_cmd_t` IDs, key bindings (`|`, `\`, `/`, `?`, `n`, `N`, `"`, `d`, `[`, `]`, `g|`, `g\`, `g/`, `g?`, `gd`, `g[`, `g]`), and 8 prompt kinds
- `src/tam-data.c` — `row_order[cursor_row]` fix for `s`/`u`/`t`, command & prompt dispatch, `td_usage()` updates
- `src/version.h`, `README.md`, `ROADMAP.md` — `0.3.0` version bump and Phase 2 documentation updates
- `tests/test_sheet.c`, `tests/test_input.c` — sanitizer unit tests covering every Phase 2 operation and edge case

---

## 3. Work Items

### W1: Physical vs. Visible Row Separation (`src/sheet.*`, `src/tam-data.c`)

- Add `size_t phys_nrows;` to `td_sheet_t` alongside `size_t nrows;` (`row_order[0 .. nrows - 1]` maps visible row index to physical row index in `0 .. phys_nrows - 1`).
- Update `td_sheet_add_row()` to allocate physical index `phys = s->phys_nrows++` and append `s->row_order[s->nrows++] = phys`.
- Update `td_sheet_get_cell()`, `td_sheet_set_cell()`, `td_row_valid()`, and `td_sheet_is_selected()` to validate physical indices against `s->phys_nrows`.
- Update `td_sheet_select_all()`, `td_sheet_unselect_all()`, and `td_sheet_toggle_all()` to iterate visible rows `0 .. s->nrows - 1` and act on `s->row_order[vr]`.
- Update `TD_CMD_SELECT_ROW`, `TD_CMD_UNSELECT_ROW`, and `TD_CMD_TOGGLE_ROW` in `src/tam-data.c` to pass `s->row_order[s->cursor_row]`.

### W2: POSIX `<regex.h>` Selection & Search Engine (`src/sheet.*`)

- Add per-sheet search state to `td_sheet_t`:
  - `char *last_search;` (heap-allocated pattern for `n`/`N`, freed in `td_sheet_free()`)
  - `bool search_all_cols;`
  - `int search_dir;` (`+1` forward, `-1` backward)
- Format cell values via `td_val_format()` (stack buffer with dynamic fallback for arbitrarily long cells) and match using `regcomp(..., REG_EXTENDED | REG_NOSUB)` + `regexec()`.
- Public API in `src/sheet.h`:
  - `bool td_sheet_select_regex(td_sheet_t *s, const char *pattern, bool all_cols, bool select, size_t *matched_out, char *err, size_t err_cap);`
  - `bool td_sheet_search(td_sheet_t *s, const char *pattern, int dir, bool all_cols, char *err, size_t err_cap);`
  - `bool td_sheet_search_next(td_sheet_t *s, int dir_mult, char *err, size_t err_cap);`

### W3: Row Deletion & Filtered Sheet Extraction (`src/sheet.*`)

- `bool td_sheet_delete_row(td_sheet_t *s, size_t vis_row);` (`d`)
  - Clears `sel_bits` for `phys = s->row_order[vis_row]`, shifts `row_order` left, decrements `s->nrows`, clamps `s->cursor_row`, and invalidates auto-column `cached_width`.
- `size_t td_sheet_delete_selected(td_sheet_t *s);` (`gd`)
  - Compacts `row_order` in place removing all selected rows, clears their `sel_bits`, updates `s->nrows` and `s->nselected`, clamps `s->cursor_row`, and invalidates auto-column `cached_width`.
- `td_sheet_t *td_sheet_dup_selected(const td_sheet_t *s, const char *suffix);` (`"`)
  - Returns `NULL` if `s->nselected == 0` (caller warns `"no rows selected"`).
  - Creates an independent child `td_sheet_t` copying column definitions and selected visible rows in `row_order` sequence, deep-copying string slices into the child sheet's own `td_arena_t` so closing the parent sheet never leaves dangling pointers.

### W4: Stable Typed Sorting (`src/sheet.*`)

- Implement a standard C11 stable merge sort over `s->row_order` with explicit `td_sort_ctx_t` context (no `qsort_r` dependency).
- Compare evaluated cells (`TD_TYPE_ANY` coerced on demand via `td_val_parse()` to `col->type`) using `td_val_cmp()`, inverting the sign for descending order (`TD_TYPE_NULL` sorts first ascending and flips to last descending).
- Public API in `src/sheet.h`:
  - `bool td_sheet_sort_cols(td_sheet_t *s, const size_t *cols, size_t ncols, bool desc);`
  - `bool td_sheet_sort_col(td_sheet_t *s, size_t col, bool desc);` (`[`, `]`)
  - `bool td_sheet_sort_keys(td_sheet_t *s, bool desc);` (`g[`, `g]`, falling back to `s->cursor_col` when no key columns are pinned)

### W5: Input Chords, Prompt Dispatch & UI Wiring (`src/input.*`, `src/tam-data.c`)

- Add Phase 2 `td_cmd_t` enum entries and key decoding (`|`, `\`, `/`, `?`, `n`, `N`, `"`, `d`, `[`, `]`, `g|`, `g\`, `g/`, `g?`, `gd`, `g[`, `g]`).
- Add 8 `td_prompt_kind_t` entries (`TD_PROMPT_SELECT_REGEX`, `TD_PROMPT_UNSELECT_REGEX`, `TD_PROMPT_SELECT_ALL_REGEX`, `TD_PROMPT_UNSELECT_ALL_REGEX`, `TD_PROMPT_SEARCH_FWD`, `TD_PROMPT_SEARCH_BWD`, `TD_PROMPT_SEARCH_ALL_FWD`, `TD_PROMPT_SEARCH_ALL_BWD`) and route them in `td_handle_prompt_result()`.
- Update `td_usage()` help text with Phase 2 keybindings.

### W6: Version, Docs & Sanitizer Verification (`src/version.h`, `README.md`, `ROADMAP.md`, `tests/*`)

- Add thorough unit tests in `tests/test_sheet.c` and `tests/test_input.c` for every Phase 2 feature and edge case.
- Bump `src/version.h` to `0.3.0`, update `README.md` and `ROADMAP.md`.
- Verify `make && make test` under `ASan`/`UBSan` with zero warnings and zero leaks.

---

## 4. Phase 2 Completion Criteria (Exit Gate to Phase 3)

- [ ] `make` builds `build/tam-data` (`0.3.0`) with zero warnings under `-Wall -Wextra -Wpedantic -Werror -std=c11`.
- [ ] `make test` passes 100% of unit tests (`arena`, `val`, `sheet`, `csv`, `term`, `input`) under `ASan` and `UBSan` with zero memory leaks.
- [ ] Row selection (`s`, `u`, `t`, `gs`, `gu`, `gt`, `|`, `\`, `g|`, `g\`) operates accurately after sorting and row deletion.
- [ ] Regex search (`/`, `?`, `g/`, `g?`, `n`, `N`) steps across matching rows/columns with wrap-around and clean invalid-regex handling.
- [ ] Duplicating selected rows (`"`) warns when 0 rows are selected and otherwise pushes an independent sheet onto `td_stack_t`.
- [ ] Deleting (`d`, `gd`) and stable typed sorting (`[`, `]`, `g[`, `g]`) update `row_order` cleanly across all cell types.

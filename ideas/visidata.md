# Ideas from VisiData

## Overview

- **What it is:** A terminal interface and multitool for exploring, arranging,
  and analyzing tabular data (CSV, TSV, JSON, SQLite, Excel, HDF5, and more).
- **Developers:** Created and maintained by Saul Pwanson (`saulpw`) and
  contributors.
- **License:** GNU General Public License v3 (`GPL-3.0`, see `LICENSE.gpl3`).
- **Language:** Python 3.8+ (with `curses` and optional third-party Python
  packages for extended formats).
- **Source Code:** <https://github.com/saulpw/visidata>.

---

## Ideas We Borrow

### 1. Sheet-Stack & Metasheet Paradigm
- Everything is a sheet (`Sheets` `S`, `Columns` `C`, `Options` `O`,
  `Describe` `I`, `Frequency` `F`, `Directory`, `CommandLog` `D`).
- Pushing derived sheets onto a stack makes data exploration non-destructive
  and consistent across every workflow.

### 2. Modal Prefix Keybindings & Muscle Memory
- Single-key navigation (`h`/`j`/`k`/`l`, `[`/`]`, `s`/`u`/`t`, `/`/`?`) paired
  with `g` (global/all) and `z` (zoom/variant) prefixes.
- Preserving VisiData's keymap lets users transfer muscle memory directly to
  `tam-data`.

### 3. Analytical & Transformation Workflows
- Frequency tables (`F`/`gF`) with histogram bars and `Enter`-to-dive
  extraction.
- Statistical `Describe` sheet (`I`), aggregators (`+`/`z+`) with persistent
  footers, pivot (`W`), melt (`M`), and stack joins (`&`).
- Computed columns (`=`/`g=`), regex/delimiter column splitting (`:`/`;`), and
  terminal scatter/bar plots (`.`/`g.`).

### 4. Deterministic Replay & Command Log
- Recording commands to a tabular log (`D`) and `.vd` scripts for `-p`/`-b`
  reproducible batch replay.

---

## Ideas We Adapt

### 1. Embedded Expression Evaluation
- **VisiData:** Uses Python `eval()` directly on row objects, which grants full
  Python power at the cost of requiring a Python runtime.
- **tam-data:** Implements `td_expr`, a compact embedded C expression
  evaluator supporting arithmetic, relational, logical, ternary, string, math,
  and date functions with zero Python dependency.

### 2. Format Loaders
- **VisiData:** Relies on `openpyxl`, `lxml`, `sqlite3`, `h5py`, etc.
- **tam-data:** Implements native C11 `mmap`/streaming loaders for CSV, TSV,
  JSON, JSONL, INI, fixed-width tables, directories, and `--ps` process tables.

### 3. Terminal Rendering
- **VisiData:** Uses Python's `curses` module.
- **tam-data:** Renders directly via `libtam` (`tam_tty`, `tam_key`, `tam_uni`)
  with automatic Unicode Braille / ASCII fallback for `TERM=linux`.

---

## Ideas We Avoid

- **Python Runtime & Plugin Ecosystem:** Requiring Python 3.8+ and `pip`
  packages breaks Tamlinux's sub-megabyte RSS, 0–2 ms cold start, and
  `libc` + `libtam`-only goals.
- **Dynamic Arbitrary-Code Execution in Cells:** Running arbitrary Python in
  expressions/plugins is unnecessary for core tabular workflows and introduces
  runtime bloat and safety complexity.
- **`curses` / `terminfo` Dependency:** Avoided in favor of `libtam`'s direct
  ANSI/VT handling.

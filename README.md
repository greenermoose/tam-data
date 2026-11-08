# tamdata

`tamdata` is a terminal spreadsheet multitool written in C, recreating the interactive data exploration and arrangement capabilities of [VisiData](https://www.visidata.org/) with zero Python dependencies.

## Mission & Purpose

`tamdata` provides a fast, standalone, and lightweight tabular data tool that runs on any Tamlinux system—including minimal recovery environments, headless servers, and VT text-only terminals (Linux virtual consoles / serial lines)—without requiring a Python runtime or external language interpreters. It links only against standard C/POSIX libraries and the Tam C libraries (`libtam`).

## Design Goals

- **Zero Python Runtime Dependencies:** Built purely in C to operate in minimal base environments where Python is not installed or available.
- **Full VisiData Feature Parity:** Systematically recreate VisiData's rich workflow and interactive capabilities:
  - Direct reading and writing of delimited formats (CSV, TSV).
  - Fast sheet navigation, multi-column sorting, row and column filtering, and regex searching.
  - Frequency analysis, summary statistics, pivot tables, and column type inference.
  - Multi-sheet stack navigation (`Shift+S`), undo/redo history, and status bar command discovery (`Ctrl+H`).
- **VT Text-Only Terminal Compatibility:** Native escape-sequence and ANSI/VT terminal rendering capable of running cleanly inside bare virtual consoles, serial terminals, and standard terminal emulators without graphical or heavy terminal dependencies.
- **Tamlinux Ecosystem Integration:** Designed to leverage Tamlinux C infrastructure (`libtam`) for consistent system integration, memory efficiency, and predictable error handling.
- **Speed & Footprint:** Instantaneous startup time and negligible memory overhead even when operating on large tabular files.

## Status

`tamdata` is in early architectural development.

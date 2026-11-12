# tam-data

A terminal spreadsheet multitool written in C, inspired by
[VisiData](https://www.visidata.org/). The aim is interactive tabular data
exploration and editing with a small runtime footprint and first-class Linux
virtual console support, without a Python runtime.

**Early development: the current version, 0.1.0, runs a built-in demo only.**
It does not yet open or save CSV/TSV files, accept piped data, or edit cells.
CSV/TSV loading and saving are the next milestone. The long-term ambition is
VisiData's core workflows; feature parity is not a current capability.

![tam-data displaying its built-in demo sheet](docs/images/demo.png)

The screenshot shows the current demo, including typed columns, a pinned
name column, and the row-selection status bar. See
[capture details](docs/images/README.md).

## What works today

- Grid navigation with arrow keys or `h`/`j`/`k`/`l`, paging, and row/column jumps.
- Row selection, column hiding, renaming, autosizing, pinning, and type changes
  on the demo data.
- Typed values, sheet-stack infrastructure, a prompt editor, and a terminal
  renderer with Linux console and modern terminal profiles.
- Strict C11 compilation and an AddressSanitizer/UndefinedBehaviorSanitizer
  test suite.

See [the roadmap](ROADMAP.md) for planned features and
[the Phase 1 workplan](plans/P1-WORKPLAN.md) for the CSV/TSV milestone.

## Build on Linux

Tamlinux is not required to build the demo. You need a C11 compiler, POSIX
development headers and libc, GNU Make, Git, `pkg-config`, and
[`libtam` >= 0.4.0](https://github.com/greenermoose/libtam).
The tests also need a compiler with ASan/UBSan support, such as GCC or Clang.
Other operating systems have not been verified.

`tam-data` uses the core static `libtam.a` archive. The optional portal,
display, and session libraries are not needed; neither are D-Bus development
files, ncurses, or Python.

From a parent directory containing this checkout, build and install the
library into a development prefix owned by your user:

```sh
git clone https://github.com/greenermoose/libtam.git
make -C libtam core
make -C libtam install-core PREFIX="$HOME/.local/dev"
```

Then build and test this checkout using that prefix explicitly:

```sh
cd tam-data
make LIBTAM_PREFIX="$HOME/.local/dev"
make test LIBTAM_PREFIX="$HOME/.local/dev"
./build/tam-data --demo
```

If `libtam` is already installed where `pkg-config` can find it, plain
`make` and `make test` work. To expose a custom prefix to `pkg-config`:

```sh
export PKG_CONFIG_PATH="$HOME/.local/dev/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --atleast-version=0.4.0 libtam
make
```

Running from the build directory is sufficient. An optional development
installation uses an explicit prefix:

```sh
make install PREFIX="$HOME/.local/dev"
```

The installed executable is `~/.local/dev/bin/tam-data`. `make uninstall`
with the same `PREFIX` removes it. The Makefile's implicit prefix depends
on whether Nix is detected, so specify it when installing.

## Try the demo

Run `./build/tam-data --demo` from an interactive terminal. A Linux virtual
console (`TERM=linux`) is a design target alongside terminal emulators.
The demo contains eight built-in rows; no input file is required.

| Key | Action |
| --- | --- |
| Arrow keys or `h` / `j` / `k` / `l` | Move between cells |
| `Ctrl+F` / `Ctrl+B` | Page down / up |
| `gg` / `G` | First / last row |
| `gh` / `gl` | First / last visible column |
| `s` / `u` / `t` | Select / unselect / toggle the current row |
| `!` | Pin or unpin the current column |
| `_` / `^` / `-` | Autosize / rename / hide the current column |
| `q` | Close the sheet and exit |

`./build/tam-data --help` lists further controls. Changes affect the demo in
memory and are discarded on exit; saving is not implemented yet.

## Performance goals

Fast startup and low memory use are goals, not benchmark results. Startup
latency, peak resident memory, and large-file performance have not been
measured for this project. The planned arena/mmap-backed CSV/TSV path is
not implemented yet. The small demo and passing unit tests do not establish
large-file performance.

## License and development provenance

Copyright (C) 2026 Fred Horch and contributors.

This project's code and documentation are licensed under the GNU General
Public License, version 3 or (at your option) any later version
(`GPL-3.0-or-later`). See [LICENSE](LICENSE).

AI-assisted development, verified commit attribution, and gaps in historical
evidence are recorded in [AI_PROVENANCE.md](AI_PROVENANCE.md).

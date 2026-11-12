# Demo screenshot

[`demo.png`](demo.png) is a capture of the running `tam-data` built-in demo
on 2026-11-12, built from source baseline `79d1d76`. It shows the current
application, not a mockup. Only the terminal window was captured.

The demo ran in foot with a monospace font and no user configuration:

```sh
./build/tam-data --demo
```

The compositor determined the final window dimensions. The image is
1256 × 680 pixels, including the header, all eight built-in rows, and status
bar. There are no edits to the demo data or added UI elements. Recreate it
by building the program, running the command in a terminal, and taking a
screenshot of that window. Python and screenshot tools used for capture
are documentation tooling, not application runtime dependencies.

The capture verifies this terminal rendering only. It does not establish
CSV loading, large-file performance, or physical Linux-console acceptance.

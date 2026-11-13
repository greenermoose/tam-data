# `tam-data` Benchmarks & Performance Baselines

This document records reproducible performance baselines for `tam-data` using the C11 benchmark harness in [`bench/bench_tam_data.c`](../bench/bench_tam_data.c) and tracks optimization targets for `v1.0.1` and `v1.1.0`.

## Running the Harness

```sh
# Fast developer check (startup + 100k-row datasets; informational)
make bench-quick

# Full suite (startup + 100k and 1M-row datasets)
make bench

# Export machine-readable JSON/CSV
./build/bench_tam-data --full --json build/bench/baseline.json --csv build/bench/baseline.csv
```

## Methodology

- **Build:** Release `-O2 -std=c11` (`build/tam-data` and `build/bench_tam-data`, no sanitizers).
- **Startup Latency (`0–2 ms` primary target):**
  - **Primary metric (`pty-first-frame`):** Parent opens a 24×80 PTY via `posix_openpt()`, forks/execs `tam-data` with `TERM=xterm-256color`, measures `CLOCK_MONOTONIC` elapsed time until the first rendered frame bytes arrive on the PTY master fd, then sends `'q'` and reaps the child via `wait4()`.
  - **Secondary metric (`cli`):** `fork()` + `execvp()` + `wait4()` wall time for `--version`, `-b --demo`, and `-b small.csv`.
  - **Warm vs. Cold:** Warm runs execute 3–5 warmup iterations followed by 40 measured runs (`--full`). Cold runs evict clean page-cache pages for both `build/tam-data` and the input file via `posix_fadvise(..., POSIX_FADV_DONTNEED)` before each of 10 iterations (no root required).
- **Large-File Load & Peak RSS:** Each loader runs in an isolated child process so `wait4()` `ru_maxrss` reports exact peak RSS without cross-run contamination. Synthetic datasets (6 columns: `id, group, score, amount, date, note`) are deterministically generated into `build/bench/`.
- **Core Analytical Hotspots:** Measured in-process on loaded 100k-row and 1M-row CSV sheets after column typing (`int64`, `str`, `float`, `currency`, `date`, `str`).

---

## `v1.0.0` Baseline Results

**Environment:** `Linux 7.2.11-arch1-1.1 x86_64`, `tam-data 1.0.0` (`v1.0.0-dirty`).

### Suite 1: Startup Latency (Target: `0–2 ms` PTY First-Frame)

| Case | Mode | Iters | Min (ms) | Median (ms) | P95 (ms) | Max (ms) | Peak RSS (KiB) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| `pty-first-frame --demo` | warm | 40 | 0.76 | **0.81** | 1.23 | 1.58 | 2704 |
| `pty-first-frame --demo` | cold | 10 | 1.98 | **2.38** | 3.82 | 3.82 | 2696 |
| `pty-first-frame small.csv` | warm | 40 | 0.77 | **1.01** | 1.51 | 1.53 | 2712 |
| `pty-first-frame small.csv` | cold | 10 | 2.08 | **2.46** | 2.97 | 2.97 | 2704 |
| `cli --version` | warm | 40 | 0.71 | 0.75 | 1.20 | 1.50 | 2572 |
| `cli --version` | cold | 10 | 1.68 | 1.82 | 2.45 | 2.45 | 2576 |
| `cli -b --demo` | warm | 40 | 0.67 | 0.79 | 1.50 | 1.68 | 2576 |
| `cli -b small.csv` | warm | 40 | 0.74 | 0.78 | 1.18 | 1.47 | 2520 |

**Takeaway:** Warm PTY first-frame startup (`0.81–1.01 ms` median, `1.23–1.51 ms` p95) meets the `0–2 ms` target; cold-cache startup sits right at `~2.4 ms` median.

### Suite 2: Large-File Load & Peak RSS (`100k` & `1M` Rows)

| Dataset | Scale | File (MiB) | Load (ms) | Throughput (MiB/s) | Peak RSS (MiB) | `mmap`/buf (MiB) | Arena (MiB) | Cells (MiB) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `100k.csv` (`mmap`) | 100k | 4.8 | 17.24 | 275.9 | 25.4 | 4.8 | 0.06 | 18.0 |
| `100k_quoted.csv` (`mmap`) | 100k | 6.7 | 21.92 | 304.0 | 28.6 | 6.7 | 1.69 | 18.0 |
| `100k.tsv` (`strict_tsv`) | 100k | 4.8 | 17.85 | 266.5 | 25.4 | 4.8 | 0.06 | 18.0 |
| `100k.jsonl` (`heap+ast`) | 100k | 9.7 | **212.27** | 45.8 | **109.1** | 0.0 | 4.62 | 18.0 |
| `100k.csv` (`fd/stdin`) | 100k | 4.8 | 20.78 | 228.9 | 25.4 | 4.8 | 0.06 | 18.0 |
| `1M.csv` (`mmap`) | 1M | 48.5 | **141.85** | 342.1 | **202.1** | 48.5 | 0.06 | 144.0 |
| `1M.tsv` (`strict_tsv`) | 1M | 48.5 | 142.56 | 340.4 | 202.1 | 48.5 | 0.06 | 144.0 |
| `1M.jsonl` (`heap+ast`) | 1M | 98.1 | **2071.67** | 47.4 | **1037.6** | 0.0 | 45.81 | 144.0 |
| `1M.csv` (`fd/stdin`) | 1M | 48.5 | 157.47 | 308.1 | 202.0 | 48.5 | 0.06 | 144.0 |

### Suite 3: Core Analytical Hotspots (`100k` & `1M` Rows)

| Operation | 100k (ms) | 100k (rows/s) | 1M (ms) | 1M (rows/s) |
| :--- | ---: | ---: | ---: | ---: |
| `expr_eval(score*1.5+id)` | 9.25 | 10,811,581 | 92.92 | 10,762,356 |
| `freq_sheet(group)` | 17.70 | 5,648,771 | 240.51 | 4,157,838 |
| `agg(sum+mean+med+stdev)` | 18.02 | 5,549,180 | 254.47 | 3,929,781 |
| `select_regex(col)` | 23.47 | 4,260,002 | 313.97 | 3,185,048 |
| `sort_col(float)` | 50.90 | 1,964,540 | 734.89 | 1,360,744 |
| `sort_col(str)` | 192.84 | 518,575 | **3014.64** | 331,714 |
| `describe_sheet` | 262.40 | 381,101 | **4464.16** | 224,006 |

---

## Identified Hotspots & Optimization Roadmap

1. **JSONL Streaming Loader (`v1.0.1` — High Impact):**
   - *Observed:* `1M.jsonl` takes `2.07 s` and **`1037.6 MiB` peak RSS** (5.1× CSV RSS, 14.6× CSV load time).
   - *Cause:* `td_json_load_file()` reads the whole file into a heap buffer, `td_json_load_mem()` materializes a `jval_t` AST array for all 1M lines simultaneously, and `jrows_to_sheet()` linearly scans `s->cols` per key per row.
   - *Target:* Stream JSONL line-by-line (reusing a scratch arena per line and indexing column names) to bring `1M.jsonl` peak RSS down toward `~250–300 MiB` and load time under `500 ms`.
2. **`describe_sheet` Single-Pass Optimization (`v1.0.1` — High Impact):**
   - *Observed:* `describe_sheet` takes `4.46 s` on 1M × 6 columns (`~740 ms` per column).
   - *Cause:* Re-scans and re-sorts/parses values repeatedly across multiple per-column aggregators instead of computing summary stats in a single pass.
   - *Target:* Reduce `1M` `describe_sheet` under `1.0 s`.
3. **CSV Row Pre-Sizing & `madvise` (`v1.0.1` — Medium Impact):**
   - *Observed:* `1M.csv` loads in `141.85 ms` (`202.1 MiB` RSS) with `144 MiB` in `td_val_t` arrays (`row_cap = 1,048,576`, `~4.6%` slack after doubling from `TD_SHEET_ROW_CHUNK`).
   - *Target:* `madvise(MADV_SEQUENTIAL)` on `mmap` + estimated initial `row_cap` to eliminate ~15 `realloc()` passes per column.
4. **String Sort & Compact Cell Representation (`v1.1.0`):**
   - *Observed:* `sort_col(str)` takes `3.01 s` on 1M rows (4.1× slower than float sort), and `td_val_t` is 24 bytes/cell (`144 MiB` for 6M cells).
   - *Target:* Pre-normalize/cache sort keys during `td_sheet_sort_col()` and evaluate a compact 16-byte `td_val_t` layout for `v1.1.0`.

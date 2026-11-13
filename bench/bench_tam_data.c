#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "agg.h"
#include "arena.h"
#include "csv.h"
#include "expr.h"
#include "json.h"
#include "sheet.h"
#include "val.h"
#include "version.h"

#define BENCH_DIR       "build/bench"
#define BENCH_DEFAULT_BIN "./build/tam-data"

typedef struct {
    bool full;
    bool regen;
    bool run_startup;
    bool run_load;
    bool run_ops;
    const char *bin_path;
    const char *json_out;
    const char *csv_out;
    uint64_t seed;
} bench_cfg_t;

typedef struct {
    char suite[32];
    char name[64];
    char mode[16];     /* "warm", "cold", "100k", "1M" */
    size_t iters;
    double min_ms;
    double median_ms;
    double p95_ms;
    double max_ms;
    double mean_ms;
    long peak_rss_kib;
    uint64_t file_bytes;
    size_t nrows;
    size_t ncols;
    size_t mmap_bytes;
    size_t arena_bytes;
    size_t cells_bytes;
    double row_cap_slack_pct;
    double rows_per_sec;
    double mib_per_sec;
} bench_rec_t;

static bench_rec_t g_recs[128];
static size_t g_nrecs = 0;

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;

    if (da < db)
        return -1;
    if (da > db)
        return 1;
    return 0;
}

static void summarize_samples(const double *samples, size_t n, bench_rec_t *r)
{
    double *tmp;
    double sum = 0.0;

    if (n == 0)
        return;
    tmp = malloc(n * sizeof(*tmp));
    if (!tmp)
        return;
    memcpy(tmp, samples, n * sizeof(*tmp));
    qsort(tmp, n, sizeof(*tmp), cmp_double);

    for (size_t i = 0; i < n; i++)
        sum += tmp[i];

    r->iters = n;
    r->min_ms = tmp[0];
    r->max_ms = tmp[n - 1];
    r->mean_ms = sum / (double)n;
    if (n % 2 == 1)
        r->median_ms = tmp[n / 2];
    else
        r->median_ms = 0.5 * (tmp[n / 2 - 1] + tmp[n / 2]);

    size_t p95_idx = (size_t)ceil(0.95 * (double)n);
    if (p95_idx == 0)
        p95_idx = 1;
    if (p95_idx > n)
        p95_idx = n;
    r->p95_ms = tmp[p95_idx - 1];
    free(tmp);
}

static void record_add(const bench_rec_t *r)
{
    if (g_nrecs < sizeof(g_recs) / sizeof(g_recs[0]))
        g_recs[g_nrecs++] = *r;
}

static void drop_file_cache(const char *path)
{
    int fd;

    if (!path || !path[0])
        return;
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }
}

static bool ensure_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return true;
    if (mkdir(path, 0755) == 0)
        return true;
    return errno == EEXIST;
}

static bool file_nonempty(const char *path, uint64_t *size_out)
{
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return false;
    if (size_out)
        *size_out = (uint64_t)st.st_size;
    return true;
}

typedef enum {
    GEN_CSV = 0,
    GEN_CSV_QUOTED,
    GEN_TSV,
    GEN_JSONL
} gen_fmt_t;

static bool generate_dataset(const char *path, size_t nrows, gen_fmt_t fmt,
                             uint64_t seed, bool force_regen,
                             uint64_t *bytes_out)
{
    FILE *fp;
    uint64_t rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
    static const char *const notes[] = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"
    };

    if (!force_regen && file_nonempty(path, bytes_out))
        return true;

    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "bench: cannot create %s: %s\n", path, strerror(errno));
        return false;
    }

    static char io_buf[256 * 1024];
    setvbuf(fp, io_buf, _IOFBF, sizeof(io_buf));

    if (fmt == GEN_CSV || fmt == GEN_CSV_QUOTED)
        fputs("id,group,score,amount,date,note\n", fp);
    else if (fmt == GEN_TSV)
        fputs("id\tgroup\tscore\tamount\tdate\tnote\n", fp);

    for (size_t i = 0; i < nrows; i++) {
        uint64_t r1 = xorshift64(&rng);
        uint64_t r2 = xorshift64(&rng);
        unsigned grp = (unsigned)(r1 % 32u);
        double score = (double)(r1 % 100000u) / 100.0;
        double amount = (double)(r2 % 500000u) / 100.0;
        unsigned year = 2020u + (unsigned)(r2 % 7u);
        unsigned month = 1u + (unsigned)((r2 >> 8) % 12u);
        unsigned day = 1u + (unsigned)((r2 >> 16) % 28u);
        const char *note = notes[(r1 >> 16) & 7u];

        switch (fmt) {
        case GEN_CSV:
            fprintf(fp, "%zu,group_%02u,%.2f,%.2f,%04u-%02u-%02u,%s_%u\n",
                    i + 1, grp, score, amount, year, month, day, note,
                    (unsigned)(r2 & 0xFFu));
            break;
        case GEN_CSV_QUOTED:
            fprintf(fp,
                    "%zu,\"group_%02u,tier\",%.2f,\"$%.2f\",%04u-%02u-%02u,\"%s \"\"tag\"\" %u\"\n",
                    i + 1, grp, score, amount, year, month, day, note,
                    (unsigned)(r2 & 0xFFu));
            break;
        case GEN_TSV:
            fprintf(fp, "%zu\tgroup_%02u\t%.2f\t%.2f\t%04u-%02u-%02u\t%s_%u\n",
                    i + 1, grp, score, amount, year, month, day, note,
                    (unsigned)(r2 & 0xFFu));
            break;
        case GEN_JSONL:
            fprintf(fp,
                    "{\"id\":%zu,\"group\":\"group_%02u\",\"score\":%.2f,"
                    "\"amount\":%.2f,\"date\":\"%04u-%02u-%02u\",\"note\":\"%s_%u\"}\n",
                    i + 1, grp, score, amount, year, month, day, note,
                    (unsigned)(r2 & 0xFFu));
            break;
        }
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "bench: write error on %s: %s\n", path, strerror(errno));
        return false;
    }
    return file_nonempty(path, bytes_out);
}

static bool prepare_datasets(const bench_cfg_t *cfg)
{
    uint64_t bytes = 0;

    if (!ensure_dir("build") || !ensure_dir(BENCH_DIR)) {
        fprintf(stderr, "bench: cannot create %s\n", BENCH_DIR);
        return false;
    }

    printf("Preparing synthetic datasets in %s...\n", BENCH_DIR);
    if (!generate_dataset(BENCH_DIR "/small.csv", 100, GEN_CSV, cfg->seed,
                          cfg->regen, &bytes))
        return false;

    if (cfg->run_load || cfg->run_ops) {
        if (!generate_dataset(BENCH_DIR "/100k.csv", 100000, GEN_CSV,
                              cfg->seed, cfg->regen, &bytes) ||
            !generate_dataset(BENCH_DIR "/100k_quoted.csv", 100000,
                              GEN_CSV_QUOTED, cfg->seed, cfg->regen, &bytes) ||
            !generate_dataset(BENCH_DIR "/100k.tsv", 100000, GEN_TSV,
                              cfg->seed, cfg->regen, &bytes) ||
            !generate_dataset(BENCH_DIR "/100k.jsonl", 100000, GEN_JSONL,
                              cfg->seed, cfg->regen, &bytes))
            return false;

        if (cfg->full) {
            if (!generate_dataset(BENCH_DIR "/1M.csv", 1000000, GEN_CSV,
                                  cfg->seed, cfg->regen, &bytes) ||
                !generate_dataset(BENCH_DIR "/1M.tsv", 1000000, GEN_TSV,
                                  cfg->seed, cfg->regen, &bytes) ||
                !generate_dataset(BENCH_DIR "/1M.jsonl", 1000000, GEN_JSONL,
                                  cfg->seed, cfg->regen, &bytes))
                return false;
        }
    }
    return true;
}

static bool run_cli_once(char *const argv[], double *ms_out, long *rss_kib_out)
{
    struct rusage ru;
    double t0 = now_ms();
    pid_t pid = fork();

    if (pid < 0)
        return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (wait4(pid, &status, 0, &ru) < 0)
        return false;
    double t1 = now_ms();

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    *ms_out = t1 - t0;
    *rss_kib_out = ru.ru_maxrss;
    return true;
}

static bool run_pty_first_frame_once(char *const argv[], double *ms_out,
                                     long *rss_kib_out)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    char slave_name[128];
    struct winsize ws = { .ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0 };

    if (master < 0)
        return false;
    if (grantpt(master) != 0 || unlockpt(master) != 0 ||
        ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        close(master);
        return false;
    }
    (void)ioctl(master, TIOCSWINSZ, &ws);

    double t0 = now_ms();
    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return false;
    }
    if (pid == 0) {
        close(master);
        if (setsid() < 0)
            _exit(126);
        int slave = open(slave_name, O_RDWR);
        if (slave < 0)
            _exit(126);
        (void)ioctl(slave, TIOCSCTTY, 0);
        (void)ioctl(slave, TIOCSWINSZ, &ws);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > 2)
            close(slave);
        setenv("TERM", "xterm-256color", 1);
        execvp(argv[0], argv);
        _exit(127);
    }

    /* Wait for first rendered frame bytes on PTY master. */
    struct pollfd pfd = { .fd = master, .events = POLLIN, .revents = 0 };
    double first_frame_ms = -1.0;
    char buf[4096];

    if (poll(&pfd, 1, 5000) > 0 && (pfd.revents & POLLIN)) {
        ssize_t n = read(master, buf, sizeof(buf));
        if (n > 0)
            first_frame_ms = now_ms() - t0;
    }

    /* Ask tam-data to quit cleanly and drain remaining output. */
    (void)write(master, "q", 1);
    for (int i = 0; i < 50; i++) {
        struct pollfd dp = { .fd = master, .events = POLLIN, .revents = 0 };
        if (poll(&dp, 1, 20) <= 0)
            break;
        if (read(master, buf, sizeof(buf)) <= 0)
            break;
    }

    struct rusage ru;
    int status = 0;
    if (wait4(pid, &status, 0, &ru) < 0) {
        close(master);
        return false;
    }
    close(master);

    if (first_frame_ms < 0.0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    *ms_out = first_frame_ms;
    *rss_kib_out = ru.ru_maxrss;
    return true;
}

static void bench_startup_case(const char *name, const char *mode, bool is_pty,
                               bool cold, const char *bin_path,
                               const char *data_file, char *const argv[],
                               size_t warmup, size_t iters)
{
    double *samples = calloc(iters, sizeof(*samples));
    long max_rss = 0;
    bench_rec_t rec;

    if (!samples)
        return;

    if (!cold) {
        for (size_t i = 0; i < warmup; i++) {
            double ms = 0.0;
            long rss = 0;
            if (is_pty)
                (void)run_pty_first_frame_once(argv, &ms, &rss);
            else
                (void)run_cli_once(argv, &ms, &rss);
        }
    }

    size_t ok = 0;
    for (size_t i = 0; i < iters; i++) {
        double ms = 0.0;
        long rss = 0;
        bool pass;

        if (cold) {
            drop_file_cache(bin_path);
            drop_file_cache(data_file);
        }
        if (is_pty)
            pass = run_pty_first_frame_once(argv, &ms, &rss);
        else
            pass = run_cli_once(argv, &ms, &rss);
        if (pass) {
            samples[ok++] = ms;
            if (rss > max_rss)
                max_rss = rss;
        }
    }

    memset(&rec, 0, sizeof(rec));
    snprintf(rec.suite, sizeof(rec.suite), "startup");
    snprintf(rec.name, sizeof(rec.name), "%s", name);
    snprintf(rec.mode, sizeof(rec.mode), "%s", mode);
    summarize_samples(samples, ok, &rec);
    rec.peak_rss_kib = max_rss;
    record_add(&rec);
    free(samples);
}

static void run_startup_suite(const bench_cfg_t *cfg)
{
    const char *bin = cfg->bin_path;
    const char *small = BENCH_DIR "/small.csv";
    size_t warm_iters = cfg->full ? 40 : 20;
    size_t cold_iters = cfg->full ? 10 : 5;

    char *argv_ver[] = { (char *)bin, "--version", NULL };
    char *argv_demo_b[] = { (char *)bin, "-b", "--demo", NULL };
    char *argv_csv_b[] = { (char *)bin, "-b", (char *)small, NULL };
    char *argv_demo_pty[] = { (char *)bin, "--demo", NULL };
    char *argv_csv_pty[] = { (char *)bin, (char *)small, NULL };

    printf("Running startup latency benchmarks...\n");
    bench_startup_case("pty-first-frame --demo", "warm", true, false,
                       bin, NULL, argv_demo_pty, 3, warm_iters);
    bench_startup_case("pty-first-frame --demo", "cold", true, true,
                       bin, NULL, argv_demo_pty, 0, cold_iters);
    bench_startup_case("pty-first-frame small.csv", "warm", true, false,
                       bin, small, argv_csv_pty, 3, warm_iters);
    bench_startup_case("pty-first-frame small.csv", "cold", true, true,
                       bin, small, argv_csv_pty, 0, cold_iters);

    bench_startup_case("cli --version", "warm", false, false,
                       bin, NULL, argv_ver, 5, warm_iters);
    bench_startup_case("cli --version", "cold", false, true,
                       bin, NULL, argv_ver, 0, cold_iters);
    bench_startup_case("cli -b --demo", "warm", false, false,
                       bin, NULL, argv_demo_b, 5, warm_iters);
    bench_startup_case("cli -b small.csv", "warm", false, false,
                       bin, small, argv_csv_b, 5, warm_iters);
}

typedef enum {
    LOAD_CSV = 0,
    LOAD_TSV,
    LOAD_JSONL,
    LOAD_CSV_FD
} load_kind_t;

typedef struct {
    double load_ms;
    size_t nrows;
    size_t ncols;
    size_t row_cap;
    size_t mmap_bytes;
    size_t arena_bytes;
    size_t cells_bytes;
} load_child_info_t;

static void bench_load_isolated(const char *name, const char *mode,
                                const char *path, load_kind_t kind)
{
    int pfd[2];
    uint64_t fbytes = 0;

    if (!file_nonempty(path, &fbytes))
        return;
    if (pipe(pfd) != 0)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return;
    }

    if (pid == 0) {
        close(pfd[0]);
        char err[256] = "";
        td_sheet_t *s = NULL;
        double t0 = now_ms();

        if (kind == LOAD_CSV || kind == LOAD_TSV) {
            td_csv_opts_t opts;
            td_csv_opts_default(&opts);
            opts.delimiter = (kind == LOAD_TSV) ? '\t' : ',';
            opts.strict_tsv = (kind == LOAD_TSV);
            s = td_csv_load_file(path, &opts, err, sizeof(err));
        } else if (kind == LOAD_JSONL) {
            s = td_json_load_file(path, true, err, sizeof(err));
        } else if (kind == LOAD_CSV_FD) {
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                td_csv_opts_t opts;
                td_csv_opts_default(&opts);
                opts.delimiter = ',';
                s = td_csv_load_fd(fd, "stdin", &opts, err, sizeof(err));
                close(fd);
            }
        }
        double t1 = now_ms();

        if (!s) {
            fprintf(stderr, "bench: load failed for %s: %s\n", path, err);
            _exit(1);
        }

        load_child_info_t info;
        memset(&info, 0, sizeof(info));
        info.load_ms = t1 - t0;
        info.nrows = s->nrows;
        info.ncols = s->ncols;
        info.row_cap = s->row_cap;
        info.mmap_bytes = s->mmap_len ? s->mmap_len : s->buf_len;
        info.arena_bytes = s->arena.total;
        info.cells_bytes = s->row_cap * s->ncols * sizeof(td_val_t);

        (void)write(pfd[1], &info, sizeof(info));
        close(pfd[1]);
        td_sheet_free(s);
        _exit(0);
    }

    close(pfd[1]);
    load_child_info_t info;
    memset(&info, 0, sizeof(info));
    ssize_t nread = read(pfd[0], &info, sizeof(info));
    close(pfd[0]);

    struct rusage ru;
    int status = 0;
    if (wait4(pid, &status, 0, &ru) < 0)
        return;
    if (nread != (ssize_t)sizeof(info) || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return;

    bench_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.suite, sizeof(rec.suite), "load");
    snprintf(rec.name, sizeof(rec.name), "%s", name);
    snprintf(rec.mode, sizeof(rec.mode), "%s", mode);
    rec.iters = 1;
    rec.min_ms = info.load_ms;
    rec.median_ms = info.load_ms;
    rec.p95_ms = info.load_ms;
    rec.max_ms = info.load_ms;
    rec.mean_ms = info.load_ms;
    rec.peak_rss_kib = ru.ru_maxrss;
    rec.file_bytes = fbytes;
    rec.nrows = info.nrows;
    rec.ncols = info.ncols;
    rec.mmap_bytes = info.mmap_bytes;
    rec.arena_bytes = info.arena_bytes;
    rec.cells_bytes = info.cells_bytes;
    if (info.row_cap > 0 && info.row_cap >= info.nrows)
        rec.row_cap_slack_pct =
            100.0 * (double)(info.row_cap - info.nrows) / (double)info.row_cap;
    if (info.load_ms > 0.0) {
        double secs = info.load_ms / 1000.0;
        rec.rows_per_sec = (double)info.nrows / secs;
        rec.mib_per_sec = ((double)fbytes / (1024.0 * 1024.0)) / secs;
    }
    record_add(&rec);
}

static void run_load_suite(const bench_cfg_t *cfg)
{
    printf("Running large-file load & peak RSS benchmarks...\n");
    bench_load_isolated("100k.csv (mmap)", "100k", BENCH_DIR "/100k.csv", LOAD_CSV);
    bench_load_isolated("100k_quoted.csv (mmap)", "100k",
                        BENCH_DIR "/100k_quoted.csv", LOAD_CSV);
    bench_load_isolated("100k.tsv (strict_tsv)", "100k",
                        BENCH_DIR "/100k.tsv", LOAD_TSV);
    bench_load_isolated("100k.jsonl (heap+ast)", "100k",
                        BENCH_DIR "/100k.jsonl", LOAD_JSONL);
    bench_load_isolated("100k.csv (fd/stdin)", "100k",
                        BENCH_DIR "/100k.csv", LOAD_CSV_FD);

    if (cfg->full) {
        bench_load_isolated("1M.csv (mmap)", "1M", BENCH_DIR "/1M.csv", LOAD_CSV);
        bench_load_isolated("1M.tsv (strict_tsv)", "1M",
                            BENCH_DIR "/1M.tsv", LOAD_TSV);
        bench_load_isolated("1M.jsonl (heap+ast)", "1M",
                            BENCH_DIR "/1M.jsonl", LOAD_JSONL);
        bench_load_isolated("1M.csv (fd/stdin)", "1M",
                            BENCH_DIR "/1M.csv", LOAD_CSV_FD);
    }
}

static void record_op(const char *name, const char *mode, double ms,
                      size_t nrows, size_t ncols)
{
    bench_rec_t rec;

    memset(&rec, 0, sizeof(rec));
    snprintf(rec.suite, sizeof(rec.suite), "ops");
    snprintf(rec.name, sizeof(rec.name), "%s", name);
    snprintf(rec.mode, sizeof(rec.mode), "%s", mode);
    rec.iters = 1;
    rec.min_ms = ms;
    rec.median_ms = ms;
    rec.p95_ms = ms;
    rec.max_ms = ms;
    rec.mean_ms = ms;
    rec.nrows = nrows;
    rec.ncols = ncols;
    if (ms > 0.0)
        rec.rows_per_sec = (double)nrows / (ms / 1000.0);
    record_add(&rec);
}

static void run_ops_for_file(const char *path, const char *mode)
{
    char err[256] = "";
    td_csv_opts_t opts;
    td_sheet_t *s;
    double t0, t1;

    td_csv_opts_default(&opts);
    opts.delimiter = ',';
    s = td_csv_load_file(path, &opts, err, sizeof(err));
    if (!s) {
        fprintf(stderr, "bench: ops load failed (%s): %s\n", path, err);
        return;
    }

    (void)td_sheet_set_col_type(s, 0, TD_TYPE_INT);
    (void)td_sheet_set_col_type(s, 1, TD_TYPE_STR);
    (void)td_sheet_set_col_type(s, 2, TD_TYPE_FLOAT);
    (void)td_sheet_set_col_type(s, 3, TD_TYPE_CURRENCY);
    (void)td_sheet_set_col_type(s, 4, TD_TYPE_DATE);
    (void)td_sheet_set_col_type(s, 5, TD_TYPE_STR);

    t0 = now_ms();
    (void)td_sheet_sort_col(s, 2, false);
    t1 = now_ms();
    record_op("sort_col(float)", mode, t1 - t0, s->nrows, s->ncols);

    t0 = now_ms();
    (void)td_sheet_sort_col(s, 5, false);
    t1 = now_ms();
    record_op("sort_col(str)", mode, t1 - t0, s->nrows, s->ncols);

    t0 = now_ms();
    (void)td_agg_compute_rows(s, 2, s->row_order, s->nrows, TD_AGG_SUM);
    (void)td_agg_compute_rows(s, 2, s->row_order, s->nrows, TD_AGG_MEAN);
    (void)td_agg_compute_rows(s, 2, s->row_order, s->nrows, TD_AGG_MEDIAN);
    (void)td_agg_compute_rows(s, 2, s->row_order, s->nrows, TD_AGG_STDEV);
    t1 = now_ms();
    record_op("agg(sum+mean+med+stdev)", mode, t1 - t0, s->nrows, s->ncols);

    t0 = now_ms();
    td_sheet_t *desc = td_agg_describe_sheet(s, err, sizeof(err));
    t1 = now_ms();
    record_op("describe_sheet", mode, t1 - t0, s->nrows, s->ncols);
    td_sheet_free(desc);

    size_t grp_col = 1;
    t0 = now_ms();
    td_sheet_t *freq = td_agg_freq_sheet(s, &grp_col, 1, err, sizeof(err));
    t1 = now_ms();
    record_op("freq_sheet(group)", mode, t1 - t0, s->nrows, s->ncols);
    td_sheet_free(freq);

    td_expr_t *ex = td_expr_compile(s, "score * 1.5 + id", err, sizeof(err));
    if (ex) {
        td_arena_t scratch;
        td_arena_init(&scratch, 0);
        volatile double sink = 0.0;
        t0 = now_ms();
        for (size_t r = 0; r < s->nrows; r++) {
            td_val_t v = td_expr_eval(ex, s, r, &scratch, err, sizeof(err));
            if (v.type == TD_TYPE_FLOAT)
                sink += v.as.f;
            if ((r & 1023u) == 1023u)
                td_arena_reset(&scratch);
        }
        t1 = now_ms();
        (void)sink;
        record_op("expr_eval(score*1.5+id)", mode, t1 - t0, s->nrows, s->ncols);
        td_arena_free(&scratch);
        td_expr_free(ex);
    }

    size_t matched = 0;
    s->cursor_col = 1;
    t0 = now_ms();
    (void)td_sheet_select_regex(s, "^group_0[0-3]$", false, true, &matched,
                                err, sizeof(err));
    t1 = now_ms();
    record_op("select_regex(col)", mode, t1 - t0, s->nrows, s->ncols);

    td_sheet_free(s);
}

static void run_ops_suite(const bench_cfg_t *cfg)
{
    printf("Running analytical hotspot benchmarks...\n");
    run_ops_for_file(BENCH_DIR "/100k.csv", "100k");
    if (cfg->full)
        run_ops_for_file(BENCH_DIR "/1M.csv", "1M");
}

static void print_results(void)
{
    struct utsname uts;

    memset(&uts, 0, sizeof(uts));
    (void)uname(&uts);
    printf("\n=== tam-data %s (%s) Benchmark Results [%s %s %s] ===\n",
           td_version(), td_build_id()[0] ? td_build_id() : "dev",
           uts.sysname, uts.release, uts.machine);

    printf("\n-- Suite 1: Startup Latency (target: 0-2 ms PTY first-frame) --\n");
    printf("%-28s %-6s %5s %8s %8s %8s %8s %10s\n",
           "Case", "Mode", "Iters", "Min(ms)", "Med(ms)", "P95(ms)",
           "Max(ms)", "RSS(KiB)");
    for (size_t i = 0; i < g_nrecs; i++) {
        const bench_rec_t *r = &g_recs[i];
        if (strcmp(r->suite, "startup") != 0)
            continue;
        printf("%-28s %-6s %5zu %8.2f %8.2f %8.2f %8.2f %10ld\n",
               r->name, r->mode, r->iters, r->min_ms, r->median_ms,
               r->p95_ms, r->max_ms, r->peak_rss_kib);
    }

    printf("\n-- Suite 2: Large-File Load & Peak RSS --\n");
    printf("%-24s %-5s %7s %9s %8s %8s %9s %9s %9s\n",
           "Dataset", "Scale", "FileMiB", "Load(ms)", "MiB/s",
           "RSS(MiB)", "Map(MiB)", "Arn(MiB)", "Cel(MiB)");
    for (size_t i = 0; i < g_nrecs; i++) {
        const bench_rec_t *r = &g_recs[i];
        if (strcmp(r->suite, "load") != 0)
            continue;
        printf("%-24s %-5s %7.1f %9.2f %8.1f %8.1f %9.1f %9.2f %9.1f\n",
               r->name, r->mode,
               (double)r->file_bytes / (1024.0 * 1024.0),
               r->median_ms, r->mib_per_sec,
               (double)r->peak_rss_kib / 1024.0,
               (double)r->mmap_bytes / (1024.0 * 1024.0),
               (double)r->arena_bytes / (1024.0 * 1024.0),
               (double)r->cells_bytes / (1024.0 * 1024.0));
    }

    printf("\n-- Suite 3: Core Analytical Hotspots --\n");
    printf("%-28s %-6s %10s %10s %12s\n",
           "Operation", "Scale", "Rows", "Time(ms)", "Rows/sec");
    for (size_t i = 0; i < g_nrecs; i++) {
        const bench_rec_t *r = &g_recs[i];
        if (strcmp(r->suite, "ops") != 0)
            continue;
        printf("%-28s %-6s %10zu %10.2f %12.0f\n",
               r->name, r->mode, r->nrows, r->median_ms, r->rows_per_sec);
    }
}

static bool write_json(const char *path)
{
    FILE *fp = fopen(path, "w");
    struct utsname uts;

    if (!fp)
        return false;
    memset(&uts, 0, sizeof(uts));
    (void)uname(&uts);

    fprintf(fp, "{\n  \"version\": \"%s\",\n  \"build_id\": \"%s\",\n"
            "  \"sysname\": \"%s\",\n  \"release\": \"%s\",\n  \"machine\": \"%s\",\n"
            "  \"results\": [\n",
            td_version(), td_build_id(), uts.sysname, uts.release, uts.machine);
    for (size_t i = 0; i < g_nrecs; i++) {
        const bench_rec_t *r = &g_recs[i];
        fprintf(fp,
                "    {\"suite\":\"%s\",\"name\":\"%s\",\"mode\":\"%s\","
                "\"iters\":%zu,\"min_ms\":%.3f,\"median_ms\":%.3f,"
                "\"p95_ms\":%.3f,\"max_ms\":%.3f,\"mean_ms\":%.3f,"
                "\"peak_rss_kib\":%ld,\"file_bytes\":%llu,"
                "\"nrows\":%zu,\"ncols\":%zu,\"mmap_bytes\":%zu,"
                "\"arena_bytes\":%zu,\"cells_bytes\":%zu,"
                "\"row_cap_slack_pct\":%.2f,\"rows_per_sec\":%.1f,"
                "\"mib_per_sec\":%.2f}%s\n",
                r->suite, r->name, r->mode, r->iters, r->min_ms, r->median_ms,
                r->p95_ms, r->max_ms, r->mean_ms, r->peak_rss_kib,
                (unsigned long long)r->file_bytes, r->nrows, r->ncols,
                r->mmap_bytes, r->arena_bytes, r->cells_bytes,
                r->row_cap_slack_pct, r->rows_per_sec, r->mib_per_sec,
                (i + 1 < g_nrecs) ? "," : "");
    }
    fprintf(fp, "  ]\n}\n");
    return fclose(fp) == 0;
}

static bool write_csv(const char *path)
{
    FILE *fp = fopen(path, "w");

    if (!fp)
        return false;
    fprintf(fp, "suite,name,mode,iters,min_ms,median_ms,p95_ms,max_ms,mean_ms,"
                "peak_rss_kib,file_bytes,nrows,ncols,mmap_bytes,arena_bytes,"
                "cells_bytes,row_cap_slack_pct,rows_per_sec,mib_per_sec\n");
    for (size_t i = 0; i < g_nrecs; i++) {
        const bench_rec_t *r = &g_recs[i];
        fprintf(fp, "%s,%s,%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%ld,%llu,%zu,%zu,"
                    "%zu,%zu,%zu,%.2f,%.1f,%.2f\n",
                r->suite, r->name, r->mode, r->iters, r->min_ms, r->median_ms,
                r->p95_ms, r->max_ms, r->mean_ms, r->peak_rss_kib,
                (unsigned long long)r->file_bytes, r->nrows, r->ncols,
                r->mmap_bytes, r->arena_bytes, r->cells_bytes,
                r->row_cap_slack_pct, r->rows_per_sec, r->mib_per_sec);
    }
    return fclose(fp) == 0;
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: bench_tam-data [options]\n"
            "  --quick          Run startup + 100k benchmarks (default)\n"
            "  --full           Run startup + 100k + 1M benchmarks\n"
            "  --regen          Force regeneration of synthetic datasets\n"
            "  --filter SUITE   Run only: startup, load, or ops\n"
            "  --bin PATH       tam-data binary path (default: %s)\n"
            "  --json PATH      Write JSON results\n"
            "  --csv PATH       Write CSV results\n"
            "  --seed N         PRNG seed (default: 0x9E3779B97F4A7C15)\n",
            BENCH_DEFAULT_BIN);
}

int main(int argc, char **argv)
{
    bench_cfg_t cfg = {
        .full = false,
        .regen = false,
        .run_startup = true,
        .run_load = true,
        .run_ops = true,
        .bin_path = BENCH_DEFAULT_BIN,
        .json_out = NULL,
        .csv_out = NULL,
        .seed = 0x9E3779B97F4A7C15ULL
    };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--quick") == 0) {
            cfg.full = false;
        } else if (strcmp(a, "--full") == 0) {
            cfg.full = true;
        } else if (strcmp(a, "--regen") == 0) {
            cfg.regen = true;
        } else if (strcmp(a, "--filter") == 0 && i + 1 < argc) {
            const char *f = argv[++i];
            cfg.run_startup = (strcmp(f, "startup") == 0);
            cfg.run_load = (strcmp(f, "load") == 0);
            cfg.run_ops = (strcmp(f, "ops") == 0);
            if (!cfg.run_startup && !cfg.run_load && !cfg.run_ops) {
                fprintf(stderr, "bench: unknown filter '%s'\n", f);
                return 2;
            }
        } else if (strcmp(a, "--bin") == 0 && i + 1 < argc) {
            cfg.bin_path = argv[++i];
        } else if (strcmp(a, "--json") == 0 && i + 1 < argc) {
            cfg.json_out = argv[++i];
        } else if (strcmp(a, "--csv") == 0 && i + 1 < argc) {
            cfg.csv_out = argv[++i];
        } else if (strcmp(a, "--seed") == 0 && i + 1 < argc) {
            cfg.seed = strtoull(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "bench: unknown argument '%s'\n", a);
            usage(stderr);
            return 2;
        }
    }

    if (cfg.run_startup && access(cfg.bin_path, X_OK) != 0) {
        fprintf(stderr, "bench: binary not executable: %s\n", cfg.bin_path);
        return 1;
    }
    if (!prepare_datasets(&cfg))
        return 1;

    if (cfg.run_startup)
        run_startup_suite(&cfg);
    if (cfg.run_load)
        run_load_suite(&cfg);
    if (cfg.run_ops)
        run_ops_suite(&cfg);

    print_results();

    if (cfg.json_out && !write_json(cfg.json_out)) {
        fprintf(stderr, "bench: failed writing %s\n", cfg.json_out);
        return 1;
    }
    if (cfg.csv_out && !write_csv(cfg.csv_out)) {
        fprintf(stderr, "bench: failed writing %s\n", cfg.csv_out);
        return 1;
    }
    return 0;
}

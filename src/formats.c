#include "formats.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "json.h"
#include "metasheets.h"
#include "tam.h"

static void td_fmt_err(char *err, size_t cap, const char *msg)
{
    if (err && cap > 0)
        snprintf(err, cap, "%s", msg ? msg : "");
}

static bool td_ends_with_ci(const char *s, const char *suffix)
{
    size_t slen;
    size_t suflen;

    if (!s || !suffix)
        return false;
    slen = strlen(s);
    suflen = strlen(suffix);
    if (slen < suflen)
        return false;
    return strcasecmp(s + slen - suflen, suffix) == 0;
}

td_format_t td_format_from_path(const char *path)
{
    if (!path)
        return TD_FMT_AUTO;
    if (td_ends_with_ci(path, ".jsonl") || td_ends_with_ci(path, ".ndjson"))
        return TD_FMT_JSONL;
    if (td_ends_with_ci(path, ".json"))
        return TD_FMT_JSON;
    if (td_ends_with_ci(path, ".ini") || td_ends_with_ci(path, ".conf") ||
        td_ends_with_ci(path, ".cfg"))
        return TD_FMT_INI;
    if (td_ends_with_ci(path, ".fwf"))
        return TD_FMT_FWF;
    if (td_ends_with_ci(path, ".tsv"))
        return TD_FMT_TSV;
    if (td_ends_with_ci(path, ".csv"))
        return TD_FMT_CSV;
    return TD_FMT_AUTO;
}

static td_val_t td_fmt_str_val(td_sheet_t *s, const char *text)
{
    td_val_t v;
    char *copy;

    if (!text)
        return TD_VAL_NULL;
    copy = td_arena_strdup(&s->arena, text);
    if (!copy)
        return TD_VAL_NULL;
    v.type = TD_TYPE_STR;
    v.as.s.data = copy;
    v.as.s.len = (uint32_t)strlen(copy);
    return v;
}

static int td_ini_row_cb(void *user, const char *section, const char *name,
                         const char *value)
{
    td_sheet_t *s = (td_sheet_t *)user;
    size_t r = td_sheet_add_row(s);

    if (r == SIZE_MAX)
        return 1;
    td_sheet_set_cell(s, r, 0, td_fmt_str_val(s, section ? section : ""));
    td_sheet_set_cell(s, r, 1, td_fmt_str_val(s, name ? name : ""));
    td_sheet_set_cell(s, r, 2, td_fmt_str_val(s, value ? value : ""));
    return 0;
}

td_sheet_t *td_ini_load_mem(const char *name, const char *source_path,
                            const char *buf, size_t len,
                            char *err, size_t err_cap)
{
    td_sheet_t *s;
    char *nul_buf;
    int rc;

    if (!buf && len > 0) {
        td_fmt_err(err, err_cap, "invalid INI buffer");
        return NULL;
    }
    s = td_sheet_new(name ? name : "ini", source_path);
    if (!s)
        return NULL;
    if (td_sheet_add_col(s, "section", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "key", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "value", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX) {
        td_sheet_free(s);
        return NULL;
    }
    nul_buf = malloc(len + 1);
    if (!nul_buf) {
        td_sheet_free(s);
        return NULL;
    }
    if (len)
        memcpy(nul_buf, buf, len);
    nul_buf[len] = '\0';

    rc = tam_ini_parse_string(nul_buf, td_ini_row_cb, s);
    free(nul_buf);
    if (rc < 0) {
        td_fmt_err(err, err_cap, "failed to parse INI");
        td_sheet_free(s);
        return NULL;
    }
    td_sheet_set_undo_recording(s, true);
    return s;
}

td_sheet_t *td_ini_load_file(const char *path, char *err, size_t err_cap)
{
    size_t len = 0;
    char *buf;
    td_sheet_t *s;

    if (!path) {
        td_fmt_err(err, err_cap, "missing INI path");
        return NULL;
    }
    buf = tam_fs_read_file(path, &len);
    if (!buf) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    s = td_ini_load_mem(path, path, buf, len, err, err_cap);
    free(buf);
    return s;
}

typedef struct {
    size_t start;
    size_t end;
} td_fwf_span_t;

typedef struct {
    const char *ptr;
    size_t len;
} td_line_slice_t;

static td_val_t td_parse_fwf_cell(td_sheet_t *s, const char *text, size_t len)
{
    char *copy;
    td_val_t v;

    while (len > 0 && (*text == ' ' || *text == '\t')) {
        text++;
        len--;
    }
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
        len--;
    if (len == 0)
        return TD_VAL_NULL;

    copy = td_arena_strndup(&s->arena, text, len);
    if (!copy)
        return TD_VAL_NULL;
    v.type = TD_TYPE_ANY;
    v.as.s.data = copy;
    v.as.s.len = (uint32_t)len;
    return v;
}

td_sheet_t *td_fwf_load_mem(const char *name, const char *source_path,
                            const char *buf, size_t len, bool has_header,
                            char *err, size_t err_cap)
{
    td_line_slice_t *lines = NULL;
    size_t nlines = 0;
    size_t lcap = 0;
    size_t max_len = 0;
    bool *occ = NULL;
    td_fwf_span_t *spans = NULL;
    size_t nspans = 0;
    size_t scap = 0;
    td_sheet_t *s = NULL;
    size_t pos = 0;
    size_t data_start;

    if (!buf && len > 0) {
        td_fmt_err(err, err_cap, "invalid fixed-width buffer");
        return NULL;
    }

    while (pos < len) {
        size_t lstart = pos;
        size_t lend;
        bool nonblank = false;

        while (pos < len && buf[pos] != '\n')
            pos++;
        lend = pos;
        if (pos < len && buf[pos] == '\n')
            pos++;
        if (lend > lstart && buf[lend - 1] == '\r')
            lend--;
        for (size_t k = lstart; k < lend; k++) {
            if (buf[k] != ' ' && buf[k] != '\t') {
                nonblank = true;
                break;
            }
        }
        if (!nonblank)
            continue;
        if (nlines == lcap) {
            size_t ncap = lcap ? lcap * 2 : 32;
            td_line_slice_t *nl = realloc(lines, ncap * sizeof(*nl));

            if (!nl) {
                free(lines);
                return NULL;
            }
            lines = nl;
            lcap = ncap;
        }
        lines[nlines].ptr = buf + lstart;
        lines[nlines].len = lend - lstart;
        if (lines[nlines].len > max_len)
            max_len = lines[nlines].len;
        nlines++;
    }

    s = td_sheet_new(name ? name : "fwf", source_path);
    if (!s) {
        free(lines);
        return NULL;
    }
    if (nlines == 0 || max_len == 0) {
        free(lines);
        return s;
    }

    occ = calloc(max_len, sizeof(bool));
    if (!occ) {
        free(lines);
        td_sheet_free(s);
        return NULL;
    }
    for (size_t i = 0; i < nlines; i++) {
        for (size_t x = 0; x < lines[i].len; x++) {
            if (lines[i].ptr[x] != ' ' && lines[i].ptr[x] != '\t')
                occ[x] = true;
        }
    }

    for (size_t x = 0; x < max_len;) {
        size_t start;

        while (x < max_len && !occ[x])
            x++;
        if (x >= max_len)
            break;
        start = x;
        while (x < max_len && occ[x])
            x++;
        if (nspans == scap) {
            size_t ncap = scap ? scap * 2 : 16;
            td_fwf_span_t *ns = realloc(spans, ncap * sizeof(*ns));

            if (!ns) {
                free(occ);
                free(spans);
                free(lines);
                td_sheet_free(s);
                return NULL;
            }
            spans = ns;
            scap = ncap;
        }
        spans[nspans].start = start;
        spans[nspans].end = x;
        nspans++;
    }
    free(occ);

    for (size_t c = 0; c < nspans; c++) {
        char cname[64];

        if (has_header && lines[0].len > spans[c].start) {
            size_t avail = lines[0].len < spans[c].end
                               ? lines[0].len - spans[c].start
                               : spans[c].end - spans[c].start;
            const char *hp = lines[0].ptr + spans[c].start;

            while (avail > 0 && (*hp == ' ' || *hp == '\t')) {
                hp++;
                avail--;
            }
            while (avail > 0 && (hp[avail - 1] == ' ' || hp[avail - 1] == '\t'))
                avail--;
            if (avail > 0) {
                size_t copy_n = avail < sizeof(cname) - 1 ? avail : sizeof(cname) - 1;
                memcpy(cname, hp, copy_n);
                cname[copy_n] = '\0';
            } else {
                snprintf(cname, sizeof(cname), "col%zu", c + 1);
            }
        } else {
            snprintf(cname, sizeof(cname), "col%zu", c + 1);
        }
        if (td_sheet_add_col(s, cname, TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP) == SIZE_MAX) {
            free(spans);
            free(lines);
            td_sheet_free(s);
            return NULL;
        }
    }

    data_start = has_header ? 1 : 0;
    for (size_t r = data_start; r < nlines; r++) {
        size_t ridx = td_sheet_add_row(s);

        if (ridx == SIZE_MAX) {
            free(spans);
            free(lines);
            td_sheet_free(s);
            return NULL;
        }
        for (size_t c = 0; c < nspans; c++) {
            td_val_t v = TD_VAL_NULL;

            if (lines[r].len > spans[c].start) {
                size_t end = lines[r].len < spans[c].end ? lines[r].len : spans[c].end;

                v = td_parse_fwf_cell(s, lines[r].ptr + spans[c].start,
                                      end - spans[c].start);
            }
            td_sheet_set_cell(s, ridx, c, v);
        }
    }

    free(spans);
    free(lines);
    td_sheet_set_undo_recording(s, true);
    return s;
}

td_sheet_t *td_fwf_load_file(const char *path, bool has_header,
                             char *err, size_t err_cap)
{
    size_t len = 0;
    char *buf;
    td_sheet_t *s;

    if (!path) {
        td_fmt_err(err, err_cap, "missing FWF path");
        return NULL;
    }
    buf = tam_fs_read_file(path, &len);
    if (!buf) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    s = td_fwf_load_mem(path, path, buf, len, has_header, err, err_cap);
    free(buf);
    return s;
}

td_sheet_t *td_ps_load(char *err, size_t err_cap)
{
    static const char *const argv[] = {
        "ps", "-eo", "pid,ppid,user,stat,%cpu,%mem,comm", NULL
    };
    tam_exec_result_t res = tam_exec(argv, 5000);
    td_sheet_t *s;

    if (res.exit_code != 0 || !res.stdout_buf) {
        if (err && err_cap > 0) {
            snprintf(err, err_cap, "ps failed (exit %d)%s%s", res.exit_code,
                     res.timed_out ? " [timeout]" : "",
                     (res.stderr_buf && res.stderr_buf[0]) ? ": " : "");
            if (res.stderr_buf && res.stderr_buf[0]) {
                size_t cur = strlen(err);
                snprintf(err + cur, err_cap - cur, "%s", res.stderr_buf);
            }
        }
        tam_exec_result_free(&res);
        return NULL;
    }

    s = td_fwf_load_mem("ps", NULL, res.stdout_buf, res.stdout_len, true,
                        err, err_cap);
    tam_exec_result_free(&res);
    if (s) {
        if (s->ncols >= 2) {
            s->cols[0].type = TD_TYPE_INT;
            s->cols[1].type = TD_TYPE_INT;
        }
        if (s->ncols >= 6) {
            s->cols[4].type = TD_TYPE_FLOAT;
            s->cols[5].type = TD_TYPE_FLOAT;
        }
    }
    return s;
}

td_sheet_t *td_load_path(const char *path, const td_csv_opts_t *csv_opts,
                         td_format_t forced_fmt, char *err, size_t err_cap)
{
    struct stat st;
    td_format_t fmt;
    td_csv_opts_t local_csv;

    if (!path || !path[0]) {
        td_fmt_err(err, err_cap, "missing path");
        return NULL;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return td_meta_dir_build(path, err, err_cap);

    fmt = forced_fmt != TD_FMT_AUTO ? forced_fmt : td_format_from_path(path);
    switch (fmt) {
    case TD_FMT_JSON:
        return td_json_load_file(path, false, err, err_cap);
    case TD_FMT_JSONL:
        return td_json_load_file(path, true, err, err_cap);
    case TD_FMT_INI:
        return td_ini_load_file(path, err, err_cap);
    case TD_FMT_FWF:
        return td_fwf_load_file(path, csv_opts ? csv_opts->has_header : true,
                                err, err_cap);
    case TD_FMT_TSV:
        if (csv_opts)
            local_csv = *csv_opts;
        else
            td_csv_opts_default(&local_csv);
        local_csv.delimiter = '\t';
        return td_csv_load_file(path, &local_csv, err, err_cap);
    case TD_FMT_CSV:
        if (csv_opts)
            local_csv = *csv_opts;
        else
            td_csv_opts_default(&local_csv);
        local_csv.delimiter = ',';
        local_csv.strict_tsv = false;
        return td_csv_load_file(path, &local_csv, err, err_cap);
    case TD_FMT_AUTO:
    default:
        return td_csv_load_file(path, csv_opts, err, err_cap);
    }
}

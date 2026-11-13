#include "csv.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TD_CSV_READ_INIT 65536u
#define TD_CSV_SNIFF_MAX 4096u

static void td_csv_set_err(char *err, size_t err_cap, const char *fmt, ...)
{
    va_list ap;

    if (!err || err_cap == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, err_cap, fmt, ap);
    va_end(ap);
}

void td_csv_opts_default(td_csv_opts_t *opts)
{
    if (!opts)
        return;
    opts->delimiter = '\0';
    opts->has_header = true;
    opts->strict_tsv = false;
}

static bool td_csv_has_ext(const char *path, const char *ext)
{
    size_t plen;
    size_t elen;

    if (!path || !ext)
        return false;
    plen = strlen(path);
    elen = strlen(ext);
    if (plen < elen)
        return false;
    for (size_t i = 0; i < elen; i++) {
        unsigned char a = (unsigned char)path[plen - elen + i];
        unsigned char b = (unsigned char)ext[i];

        if (tolower(a) != tolower(b))
            return false;
    }
    return true;
}

char td_csv_detect_delimiter(const char *path, const char *buf, size_t len)
{
    size_t tabs = 0;
    size_t commas = 0;
    size_t limit;
    bool in_quotes = false;

    if (td_csv_has_ext(path, ".tsv") || td_csv_has_ext(path, ".tab"))
        return '\t';
    if (td_csv_has_ext(path, ".csv"))
        return ',';

    if (!buf || len == 0)
        return ',';

    /* Strip UTF-8 BOM when sniffing the first line. */
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        buf += 3;
        len -= 3;
    }

    limit = len < TD_CSV_SNIFF_MAX ? len : TD_CSV_SNIFF_MAX;
    for (size_t i = 0; i < limit; i++) {
        char c = buf[i];

        if (c == '"') {
            if (in_quotes && i + 1 < limit && buf[i + 1] == '"') {
                i++;
                continue;
            }
            in_quotes = !in_quotes;
        } else if (!in_quotes) {
            if (c == '\n' || c == '\r')
                break;
            if (c == '\t')
                tabs++;
            else if (c == ',')
                commas++;
        }
    }

    if (tabs > 0 && tabs >= commas)
        return '\t';
    return ',';
}

static size_t td_csv_add_synthetic_col(td_sheet_t *s, size_t idx)
{
    char name[32];

    snprintf(name, sizeof(name), "col%zu", idx + 1);
    return td_sheet_add_col(s, name, TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP);
}

static size_t td_csv_add_header_col(td_sheet_t *s, const char *data, size_t len)
{
    char small[256];
    char *heap = NULL;
    const char *name;
    size_t idx;

    if (len == 0 || !data)
        return td_sheet_add_col(s, "", TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP);

    if (len < sizeof(small)) {
        memcpy(small, data, len);
        small[len] = '\0';
        name = small;
    } else {
        heap = malloc(len + 1);
        if (!heap)
            return SIZE_MAX;
        memcpy(heap, data, len);
        heap[len] = '\0';
        name = heap;
    }

    idx = td_sheet_add_col(s, name, TD_TYPE_ANY, -TD_AUTO_WIDTH_CAP);
    free(heap);
    return idx;
}

static bool td_csv_unescape_quoted(td_sheet_t *s, const char *src, size_t src_len,
                                   size_t out_len, td_slice_t *out)
{
    char *dst;
    size_t j = 0;

    if (out_len == 0) {
        out->data = NULL;
        out->len = 0;
        return true;
    }
    if (out_len > UINT32_MAX)
        return false;

    dst = td_arena_alloc(&s->arena, out_len);
    if (!dst)
        return false;

    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];

        if (c == '"' && i + 1 < src_len && src[i + 1] == '"') {
            dst[j++] = '"';
            i++;
        } else if (c == '\r') {
            dst[j++] = '\n';
            if (i + 1 < src_len && src[i + 1] == '\n')
                i++;
        } else {
            dst[j++] = c;
        }
    }

    out->data = dst;
    out->len = (uint32_t)out_len;
    return true;
}

static bool td_csv_parse_field(td_sheet_t *s, const char *buf, size_t len,
                               size_t *pos, size_t *line, char delimiter,
                               bool strict_tsv, td_slice_t *out,
                               char *err, size_t err_cap)
{
    size_t p = *pos;

    if (!strict_tsv && p < len && buf[p] == '"') {
        size_t start;
        size_t out_len = 0;
        bool needs_unescape = false;
        bool closed = false;
        size_t end_content = p;
        size_t field_line = *line;

        p++;
        start = p;
        while (p < len) {
            char c = buf[p];

            if (c == '"') {
                if (p + 1 < len && buf[p + 1] == '"') {
                    needs_unescape = true;
                    out_len++;
                    p += 2;
                } else {
                    end_content = p;
                    p++;
                    closed = true;
                    break;
                }
            } else if (c == '\r') {
                needs_unescape = true;
                out_len++;
                p++;
                if (p < len && buf[p] == '\n')
                    p++;
                (*line)++;
            } else {
                if (c == '\n')
                    (*line)++;
                out_len++;
                p++;
            }
        }

        if (!closed) {
            td_csv_set_err(err, err_cap,
                           "unterminated quoted field on line %zu", field_line);
            return false;
        }
        if (p < len && buf[p] != delimiter && buf[p] != '\r' && buf[p] != '\n') {
            td_csv_set_err(err, err_cap,
                           "unexpected character after closing quote on line %zu",
                           *line);
            return false;
        }

        if (out_len == 0) {
            out->data = NULL;
            out->len = 0;
        } else if (!needs_unescape) {
            if (end_content - start > UINT32_MAX) {
                td_csv_set_err(err, err_cap, "field exceeds maximum length");
                return false;
            }
            out->data = buf + start;
            out->len = (uint32_t)(end_content - start);
        } else if (!td_csv_unescape_quoted(s, buf + start, end_content - start,
                                           out_len, out)) {
            td_csv_set_err(err, err_cap, "out of memory parsing field");
            return false;
        }
        *pos = p;
        return true;
    }

    size_t start = p;
    while (p < len && buf[p] != delimiter && buf[p] != '\r' && buf[p] != '\n')
        p++;

    if (p - start > UINT32_MAX) {
        td_csv_set_err(err, err_cap, "field exceeds maximum length");
        return false;
    }
    if (p == start) {
        out->data = NULL;
        out->len = 0;
    } else {
        out->data = buf + start;
        out->len = (uint32_t)(p - start);
    }
    *pos = p;
    return true;
}

static bool td_csv_commit_field(td_sheet_t *s, size_t record_idx,
                                size_t col_idx, size_t row_idx,
                                bool has_header, td_slice_t field,
                                char *err, size_t err_cap)
{
    td_val_t v;

    if (record_idx == 0 && has_header) {
        if (td_csv_add_header_col(s, field.data, field.len) == SIZE_MAX) {
            td_csv_set_err(err, err_cap, "out of memory adding header column");
            return false;
        }
        return true;
    }

    while (col_idx >= s->ncols) {
        if (td_csv_add_synthetic_col(s, s->ncols) == SIZE_MAX) {
            td_csv_set_err(err, err_cap, "out of memory adding column");
            return false;
        }
    }

    if (field.len == 0 || !field.data)
        return true;

    v.type = TD_TYPE_ANY;
    v.as.s = field;
    if (!td_sheet_set_cell(s, row_idx, col_idx, v)) {
        td_csv_set_err(err, err_cap, "failed to store cell");
        return false;
    }
    return true;
}

td_sheet_t *td_csv_load_mem(const char *name, const char *source_path,
                            const char *buf, size_t len,
                            const td_csv_opts_t *opts,
                            char *err, size_t err_cap)
{
    td_csv_opts_t cfg;
    td_sheet_t *s;
    char delim;
    bool strict_tsv;
    size_t pos = 0;
    size_t line = 1;
    size_t record_idx = 0;

    if (err && err_cap > 0)
        err[0] = '\0';

    if (len > 0 && !buf) {
        td_csv_set_err(err, err_cap, "invalid input buffer");
        return NULL;
    }

    td_csv_opts_default(&cfg);
    if (opts)
        cfg = *opts;

    /* Strip UTF-8 BOM if present at byte 0. */
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        buf += 3;
        len -= 3;
    }

    delim = cfg.delimiter;
    if (delim == '\0') {
        const char *hint = (source_path && source_path[0]) ? source_path : name;
        delim = td_csv_detect_delimiter(hint, buf, len);
    }
    strict_tsv = cfg.strict_tsv;

    s = td_sheet_new(name ? name : "data", source_path ? source_path : "");
    if (!s) {
        td_csv_set_err(err, err_cap, "out of memory creating sheet");
        return NULL;
    }

    while (pos < len) {
        size_t col_idx = 0;
        size_t row_idx = 0;

        if (!(record_idx == 0 && cfg.has_header)) {
            row_idx = td_sheet_add_row(s);
            if (row_idx == SIZE_MAX) {
                td_csv_set_err(err, err_cap, "out of memory adding row");
                td_sheet_free(s);
                return NULL;
            }
        }

        for (;;) {
            td_slice_t field = { NULL, 0 };

            if (!td_csv_parse_field(s, buf, len, &pos, &line, delim,
                                    strict_tsv, &field, err, err_cap)) {
                td_sheet_free(s);
                return NULL;
            }

            if (!td_csv_commit_field(s, record_idx, col_idx, row_idx,
                                     cfg.has_header, field, err, err_cap)) {
                td_sheet_free(s);
                return NULL;
            }
            col_idx++;

            if (pos < len && buf[pos] == delim) {
                pos++;
                continue;
            }
            if (pos < len && buf[pos] == '\r') {
                pos++;
                if (pos < len && buf[pos] == '\n')
                    pos++;
                line++;
            } else if (pos < len && buf[pos] == '\n') {
                pos++;
                line++;
            }
            break;
        }

        record_idx++;
    }

    return s;
}

static td_sheet_t *td_csv_load_fd_with_path(int fd, const char *name,
                                            const char *source_path,
                                            const td_csv_opts_t *opts,
                                            char *err, size_t err_cap)
{
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    td_sheet_t *s;

    if (err && err_cap > 0)
        err[0] = '\0';

    if (fd < 0) {
        td_csv_set_err(err, err_cap, "invalid file descriptor");
        return NULL;
    }

    for (;;) {
        ssize_t n;

        if (len == cap) {
            size_t next_cap = cap ? cap * 2 : TD_CSV_READ_INIT;
            char *tmp;

            if (cap && next_cap < cap) {
                free(buf);
                td_csv_set_err(err, err_cap, "input stream too large");
                return NULL;
            }
            tmp = realloc(buf, next_cap);
            if (!tmp) {
                free(buf);
                td_csv_set_err(err, err_cap, "out of memory reading stream");
                return NULL;
            }
            buf = tmp;
            cap = next_cap;
        }

        n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            td_csv_set_err(err, err_cap, "read failed: %s", strerror(errno));
            free(buf);
            return NULL;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }

    if (len == 0) {
        free(buf);
        return td_csv_load_mem(name ? name : "stdin",
                               source_path ? source_path : "",
                               "", 0, opts, err, err_cap);
    }

    if (cap - len > TD_CSV_READ_INIT) {
        char *shrunk = realloc(buf, len);
        if (shrunk) {
            buf = shrunk;
            cap = len;
        }
    }

    s = td_csv_load_mem(name ? name : "stdin",
                        source_path ? source_path : "",
                        buf, len, opts, err, err_cap);
    if (!s) {
        free(buf);
        return NULL;
    }
    s->buf_base = buf;
    s->buf_len = len;
    return s;
}

td_sheet_t *td_csv_load_fd(int fd, const char *name,
                           const td_csv_opts_t *opts,
                           char *err, size_t err_cap)
{
    return td_csv_load_fd_with_path(fd, name ? name : "stdin", "", opts,
                                    err, err_cap);
}

static const char *td_csv_basename(const char *path)
{
    const char *slash;

    if (!path || !path[0])
        return "data";
    slash = strrchr(path, '/');
    if (slash && slash[1])
        return slash + 1;
    return path;
}

td_sheet_t *td_csv_load_file(const char *path, const td_csv_opts_t *opts,
                             char *err, size_t err_cap)
{
    int fd;
    struct stat st;
    const char *name;
    td_sheet_t *s;

    if (err && err_cap > 0)
        err[0] = '\0';

    if (!path || !path[0]) {
        td_csv_set_err(err, err_cap, "empty file path");
        return NULL;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        td_csv_set_err(err, err_cap, "cannot open %s: %s", path,
                       strerror(errno));
        return NULL;
    }

    if (fstat(fd, &st) < 0) {
        td_csv_set_err(err, err_cap, "cannot stat %s: %s", path,
                       strerror(errno));
        close(fd);
        return NULL;
    }

    if (S_ISDIR(st.st_mode)) {
        td_csv_set_err(err, err_cap, "%s: is a directory", path);
        close(fd);
        return NULL;
    }

    name = td_csv_basename(path);

    if (!S_ISREG(st.st_mode)) {
        s = td_csv_load_fd_with_path(fd, name, path, opts, err, err_cap);
        close(fd);
        return s;
    }

    if (st.st_size == 0) {
        close(fd);
        return td_csv_load_mem(name, path, "", 0, opts, err, err_cap);
    }

    if ((uint64_t)st.st_size > SIZE_MAX) {
        td_csv_set_err(err, err_cap, "%s: file too large to map", path);
        close(fd);
        return NULL;
    }

    size_t map_len = (size_t)st.st_size;
    void *base = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        td_csv_set_err(err, err_cap, "cannot mmap %s: %s", path,
                       strerror(errno));
        return NULL;
    }

    s = td_csv_load_mem(name, path, (const char *)base, map_len, opts,
                        err, err_cap);
    if (!s) {
        munmap(base, map_len);
        return NULL;
    }
    s->mmap_base = base;
    s->mmap_len = map_len;
    return s;
}

static tam_str_t td_csv_append_field(tam_str_t out, const char *data,
                                     size_t len, char delimiter)
{
    if (!out || len == 0 || !data)
        return out;

    if (delimiter == '\t') {
        size_t start = 0;

        for (size_t i = 0; i < len; i++) {
            if (data[i] == '\t' || data[i] == '\r' || data[i] == '\n') {
                if (i > start)
                    out = tam_str_cat_len(out, data + start, i - start);
                out = tam_str_cat_len(out, " ", 1);
                if (!out)
                    return NULL;
                start = i + 1;
            }
        }
        if (len > start)
            out = tam_str_cat_len(out, data + start, len - start);
        return out;
    }

    bool needs_quote = false;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote)
        return tam_str_cat_len(out, data, len);

    out = tam_str_cat_len(out, "\"", 1);
    if (!out)
        return NULL;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '"') {
            if (i > start)
                out = tam_str_cat_len(out, data + start, i - start);
            out = tam_str_cat_len(out, "\"\"", 2);
            if (!out)
                return NULL;
            start = i + 1;
        }
    }
    if (len > start) {
        out = tam_str_cat_len(out, data + start, len - start);
        if (!out)
            return NULL;
    }
    return tam_str_cat_len(out, "\"", 1);
}

static tam_str_t td_csv_append_cell(tam_str_t out, td_val_t v, td_type_t type,
                                    char delimiter)
{
    char stack_buf[512];
    size_t n = td_val_format(v, type, stack_buf, sizeof(stack_buf));

    if (n == 0)
        return out;
    if (n < sizeof(stack_buf))
        return td_csv_append_field(out, stack_buf, n, delimiter);

    char *dyn = malloc(n + 1);
    if (!dyn) {
        tam_str_free(out);
        return NULL;
    }
    (void)td_val_format(v, type, dyn, n + 1);
    out = td_csv_append_field(out, dyn, n, delimiter);
    free(dyn);
    return out;
}

tam_str_t td_csv_format_sheet(const td_sheet_t *s, char delimiter)
{
    size_t small_cols[64];
    size_t *vis_cols = small_cols;
    size_t nvis = 0;
    tam_str_t out;

    if (!s)
        return NULL;
    if (delimiter == '\0')
        delimiter = ',';

    if (s->ncols > sizeof(small_cols) / sizeof(small_cols[0])) {
        vis_cols = malloc(s->ncols * sizeof(*vis_cols));
        if (!vis_cols)
            return NULL;
    }

    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t c = 0; c < s->ncols; c++) {
            if (s->cols[c].width == 0)
                continue;
            if ((pass == 0 && s->cols[c].is_key) ||
                (pass == 1 && !s->cols[c].is_key))
                vis_cols[nvis++] = c;
        }
    }

    out = tam_str_new("");
    if (!out || nvis == 0) {
        if (vis_cols != small_cols)
            free(vis_cols);
        return out;
    }

    for (size_t i = 0; i < nvis; i++) {
        const char *name = s->cols[vis_cols[i]].name;

        if (i > 0) {
            out = tam_str_cat_len(out, &delimiter, 1);
            if (!out)
                goto done;
        }
        out = td_csv_append_field(out, name, name ? strlen(name) : 0,
                                  delimiter);
        if (!out)
            goto done;
    }
    out = tam_str_cat_len(out, "\n", 1);
    if (!out)
        goto done;

    for (size_t vr = 0; vr < s->nrows; vr++) {
        size_t phys = s->row_order ? s->row_order[vr] : vr;

        for (size_t i = 0; i < nvis; i++) {
            size_t col = vis_cols[i];

            if (i > 0) {
                out = tam_str_cat_len(out, &delimiter, 1);
                if (!out)
                    goto done;
            }
            out = td_csv_append_cell(out, s->cols[col].cells[phys],
                                     s->cols[col].type, delimiter);
            if (!out)
                goto done;
        }
        out = tam_str_cat_len(out, "\n", 1);
        if (!out)
            goto done;
    }

done:
    if (vis_cols != small_cols)
        free(vis_cols);
    return out;
}

bool td_csv_save_sheet(td_sheet_t *s, const char *path, char delimiter,
                       char *err, size_t err_cap)
{
    tam_str_t buf;
    bool ok;
    int saved_errno;
    char *copy;

    if (err && err_cap > 0)
        err[0] = '\0';

    if (!s || !path || !path[0]) {
        td_csv_set_err(err, err_cap, "invalid save path");
        return false;
    }

    if (delimiter == '\0')
        delimiter = td_csv_has_ext(path, ".csv") ? ',' : '\t';

    buf = td_csv_format_sheet(s, delimiter);
    if (!buf) {
        td_csv_set_err(err, err_cap, "out of memory formatting sheet");
        return false;
    }

    errno = 0;
    ok = tam_fs_write_atomic(path, buf, tam_str_len(buf), 0644);
    saved_errno = errno;
    tam_str_free(buf);

    if (!ok) {
        td_csv_set_err(err, err_cap, "cannot save %s: %s", path,
                       saved_errno ? strerror(saved_errno) : "write failed");
        return false;
    }

    copy = td_arena_strdup(&s->arena, path);
    if (copy)
        s->source_path = copy;
    return true;
}

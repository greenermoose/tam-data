#include "metasheets.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "agg.h"
#include "cmdlog.h"
#include "formats.h"
#include "tam.h"

typedef struct {
    uint64_t *sheet_ids;
    size_t count;
} td_sheets_meta_t;

typedef struct {
    uint64_t sheet_id;
    size_t col_idx;
} td_col_ref_t;

typedef struct {
    td_col_ref_t *refs;
    size_t count;
} td_cols_meta_t;

typedef struct {
    td_cmd_t *cmds;
    size_t count;
} td_help_meta_t;

static void td_meta_err(char *err, size_t cap, const char *msg)
{
    if (err && cap > 0)
        snprintf(err, cap, "%s", msg ? msg : "");
}

static void td_sheets_meta_free(void *ptr)
{
    td_sheets_meta_t *m = (td_sheets_meta_t *)ptr;

    if (!m)
        return;
    free(m->sheet_ids);
    free(m);
}

static void td_cols_meta_free(void *ptr)
{
    td_cols_meta_t *m = (td_cols_meta_t *)ptr;

    if (!m)
        return;
    free(m->refs);
    free(m);
}

static void td_help_meta_free(void *ptr)
{
    td_help_meta_t *m = (td_help_meta_t *)ptr;

    if (!m)
        return;
    free(m->cmds);
    free(m);
}

static td_val_t td_meta_str(td_sheet_t *s, const char *text)
{
    char *copy;
    td_val_t v;

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

static td_val_t td_meta_int(int64_t val)
{
    td_val_t v;

    v.type = TD_TYPE_INT;
    v.as.i = val;
    return v;
}

static const char *td_sheet_kind_name(uint8_t kind)
{
    switch ((td_sheet_kind_t)kind) {
    case TD_SHEET_DATA:     return "data";
    case TD_SHEET_FREQ:     return "freq";
    case TD_SHEET_DESCRIBE: return "describe";
    case TD_SHEET_PIVOT:    return "pivot";
    case TD_SHEET_SHEETS:   return "sheets";
    case TD_SHEET_COLS:     return "columns";
    case TD_SHEET_OPTIONS:  return "options";
    case TD_SHEET_DIR:      return "dir";
    case TD_SHEET_HELP:     return "help";
    case TD_SHEET_PLOT:     return "plot";
    default:                return "unknown";
    }
}

td_sheet_t *td_meta_sheets_build(const td_stack_t *stack, bool all_sheets)
{
    td_sheet_t *s;
    td_sheets_meta_t *meta;
    size_t n;

    (void)all_sheets;
    if (!stack)
        return NULL;

    s = td_sheet_new("sheets", NULL);
    if (!s)
        return NULL;
    s->kind = TD_SHEET_SHEETS;

    if (td_sheet_add_col(s, "name", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "kind", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "nrows", TD_TYPE_INT, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "ncols", TD_TYPE_INT, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "nselected", TD_TYPE_INT, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "source", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX) {
        td_sheet_free(s);
        return NULL;
    }

    n = stack->len;
    meta = calloc(1, sizeof(*meta));
    if (!meta) {
        td_sheet_free(s);
        return NULL;
    }
    if (n > 0) {
        meta->sheet_ids = calloc(n, sizeof(uint64_t));
        if (!meta->sheet_ids) {
            free(meta);
            td_sheet_free(s);
            return NULL;
        }
    }
    meta->count = n;
    s->meta = meta;
    s->meta_free = td_sheets_meta_free;

    for (size_t i = stack->len; i-- > 0;) {
        const td_sheet_t *sh = stack->sheets[i];
        size_t r = td_sheet_add_row(s);

        if (r == SIZE_MAX) {
            td_sheet_free(s);
            return NULL;
        }
        meta->sheet_ids[r] = sh->id;
        td_sheet_set_cell(s, r, 0, td_meta_str(s, sh->name ? sh->name : ""));
        td_sheet_set_cell(s, r, 1, td_meta_str(s, td_sheet_kind_name(sh->kind)));
        td_sheet_set_cell(s, r, 2, td_meta_int((int64_t)sh->nrows));
        td_sheet_set_cell(s, r, 3, td_meta_int((int64_t)sh->ncols));
        td_sheet_set_cell(s, r, 4, td_meta_int((int64_t)sh->nselected));
        td_sheet_set_cell(s, r, 5, td_meta_str(s, sh->source_path ? sh->source_path : ""));
    }

    return s;
}

static bool td_parse_type_name(const char *s, td_type_t *out)
{
    if (!s || !out)
        return false;
    if (strcmp(s, "any") == 0 || strcmp(s, "") == 0) {
        *out = TD_TYPE_ANY;
        return true;
    }
    if (strcmp(s, "str") == 0 || strcmp(s, "string") == 0 || strcmp(s, "~") == 0) {
        *out = TD_TYPE_STR;
        return true;
    }
    if (strcmp(s, "int") == 0 || strcmp(s, "#") == 0) {
        *out = TD_TYPE_INT;
        return true;
    }
    if (strcmp(s, "float") == 0 || strcmp(s, "%") == 0) {
        *out = TD_TYPE_FLOAT;
        return true;
    }
    if (strcmp(s, "currency") == 0 || strcmp(s, "$") == 0) {
        *out = TD_TYPE_CURRENCY;
        return true;
    }
    if (strcmp(s, "date") == 0 || strcmp(s, "@") == 0) {
        *out = TD_TYPE_DATE;
        return true;
    }
    return false;
}

static void td_format_col_aggs(const td_col_t *c, char *buf, size_t cap)
{
    size_t pos = 0;

    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    if (!c)
        return;
    for (uint8_t i = 0; i < c->naggs; i++) {
        const char *name = td_agg_name((td_agg_kind_t)c->aggs[i]);
        int n;

        if (!name)
            continue;
        n = snprintf(buf + pos, cap - pos, "%s%s", i > 0 ? " " : "", name);
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }
}

td_sheet_t *td_meta_cols_build(const td_stack_t *stack, bool all_sheets)
{
    td_sheet_t *s;
    td_cols_meta_t *meta;
    size_t total_cols = 0;
    size_t start_idx;
    size_t idx = 0;

    if (!stack || stack->len == 0)
        return NULL;

    s = td_sheet_new(all_sheets ? "all-columns" : "columns", NULL);
    if (!s)
        return NULL;
    s->kind = TD_SHEET_COLS;
    s->source_id = stack->sheets[stack->len - 1]->id;

    if (td_sheet_add_col(s, "sheet", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "name", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "type", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "width", TD_TYPE_INT, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "key", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "aggs", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX) {
        td_sheet_free(s);
        return NULL;
    }

    start_idx = all_sheets ? 0 : stack->len - 1;
    for (size_t i = start_idx; i < stack->len; i++)
        total_cols += stack->sheets[i]->ncols;

    meta = calloc(1, sizeof(*meta));
    if (!meta) {
        td_sheet_free(s);
        return NULL;
    }
    if (total_cols > 0) {
        meta->refs = calloc(total_cols, sizeof(td_col_ref_t));
        if (!meta->refs) {
            free(meta);
            td_sheet_free(s);
            return NULL;
        }
    }
    meta->count = total_cols;
    s->meta = meta;
    s->meta_free = td_cols_meta_free;

    for (size_t i = stack->len; i-- > start_idx;) {
        const td_sheet_t *sh = stack->sheets[i];

        for (size_t c = 0; c < sh->ncols; c++) {
            const td_col_t *col = &sh->cols[c];
            size_t r = td_sheet_add_row(s);
            char aggs[256];

            if (r == SIZE_MAX) {
                td_sheet_free(s);
                return NULL;
            }
            meta->refs[idx].sheet_id = sh->id;
            meta->refs[idx].col_idx = c;
            idx++;

            td_format_col_aggs(col, aggs, sizeof(aggs));
            td_sheet_set_cell(s, r, 0, td_meta_str(s, sh->name ? sh->name : ""));
            td_sheet_set_cell(s, r, 1, td_meta_str(s, col->name));
            td_sheet_set_cell(s, r, 2, td_meta_str(s, td_type_name(col->type)));
            td_sheet_set_cell(s, r, 3, td_meta_int((int64_t)td_sheet_col_width(sh, c)));
            td_sheet_set_cell(s, r, 4, td_meta_str(s, col->is_key ? "true" : "false"));
            td_sheet_set_cell(s, r, 5, td_meta_str(s, aggs));
        }
    }

    td_sheet_set_undo_recording(s, true);
    return s;
}

bool td_meta_cols_apply_edit(td_sheet_t *cols_sheet, td_stack_t *stack,
                             size_t vis_row, size_t col,
                             char *err, size_t err_cap)
{
    td_cols_meta_t *meta;
    size_t phys;
    td_col_ref_t ref;
    td_sheet_t *target;
    char buf[256];
    td_val_t v;

    if (!cols_sheet || cols_sheet->kind != TD_SHEET_COLS || !stack ||
        vis_row >= cols_sheet->nrows) {
        return false;
    }
    meta = (td_cols_meta_t *)cols_sheet->meta;
    phys = cols_sheet->row_order[vis_row];
    if (!meta || phys >= meta->count)
        return false;

    ref = meta->refs[phys];
    target = td_stack_find_by_id(stack, ref.sheet_id);
    if (!target || ref.col_idx >= target->ncols) {
        td_meta_err(err, err_cap, "target sheet/column no longer exists");
        return false;
    }

    v = td_sheet_get_cell(cols_sheet, phys, col);
    td_val_format(v, cols_sheet->cols[col].type, buf, sizeof(buf));

    switch (col) {
    case 1: /* name */
        if (!td_sheet_rename_col(target, ref.col_idx, buf)) {
            td_meta_err(err, err_cap, "invalid column name");
            return false;
        }
        return true;
    case 2: { /* type */
        td_type_t t;

        if (!td_parse_type_name(buf, &t)) {
            td_meta_err(err, err_cap, "invalid type (any/str/int/float/currency/date)");
            return false;
        }
        return td_sheet_set_col_type(target, ref.col_idx, t);
    }
    case 3: { /* width */
        char *end = NULL;
        long w = strtol(buf, &end, 10);

        if (!buf[0] || !end || *end != '\0' || w < 0 || w > 500) {
            td_meta_err(err, err_cap, "width must be 0..500");
            return false;
        }
        return td_sheet_set_col_width(target, ref.col_idx, (int)w);
    }
    case 4: { /* key */
        bool want_key = strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0 ||
                        strcmp(buf, "yes") == 0;

        if (target->cols[ref.col_idx].is_key != want_key)
            td_sheet_toggle_key_col(target, ref.col_idx);
        return true;
    }
    case 5: { /* aggs */
        uint8_t aggs[TD_MAX_COL_AGGS];
        uint8_t naggs = 0;
        char copy[256];
        char *tok;
        char *save = NULL;

        snprintf(copy, sizeof(copy), "%s", buf);
        for (tok = strtok_r(copy, " ,;", &save); tok;
             tok = strtok_r(NULL, " ,;", &save)) {
            td_agg_kind_t ak;

            if (!td_agg_parse(tok, &ak)) {
                if (err && err_cap > 0)
                    snprintf(err, err_cap, "unknown aggregator: %s", tok);
                return false;
            }
            if (naggs < TD_MAX_COL_AGGS)
                aggs[naggs++] = (uint8_t)ak;
        }
        return td_sheet_set_col_aggs(target, ref.col_idx, aggs, naggs,
                                     "set aggs from columns sheet");
    }
    default:
        td_meta_err(err, err_cap, "column field is read-only");
        return false;
    }
}

td_sheet_t *td_meta_options_build(td_options_t *opts)
{
    td_sheet_t *s;

    if (!opts)
        return NULL;
    s = td_sheet_new("options", opts->config_path);
    if (!s)
        return NULL;
    s->kind = TD_SHEET_OPTIONS;

    if (td_sheet_add_col(s, "option", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "value", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "default", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "description", TD_TYPE_STR, -64) == SIZE_MAX) {
        td_sheet_free(s);
        return NULL;
    }

    for (size_t i = 0; i < td_options_count(); i++) {
        size_t r = td_sheet_add_row(s);
        char vbuf[64];

        if (r == SIZE_MAX) {
            td_sheet_free(s);
            return NULL;
        }
        td_options_format_value(opts, i, vbuf, sizeof(vbuf));
        td_sheet_set_cell(s, r, 0, td_meta_str(s, td_options_name(i)));
        td_sheet_set_cell(s, r, 1, td_meta_str(s, vbuf));
        td_sheet_set_cell(s, r, 2, td_meta_str(s, td_options_default_str(i)));
        td_sheet_set_cell(s, r, 3, td_meta_str(s, td_options_desc(i)));
    }

    return s;
}

bool td_meta_options_apply_edit(td_sheet_t *opt_sheet, td_options_t *opts,
                                size_t vis_row, size_t col,
                                char *err, size_t err_cap)
{
    size_t phys;
    char name_buf[128];
    char val_buf[256];
    td_val_t nv;
    td_val_t vv;

    if (!opt_sheet || opt_sheet->kind != TD_SHEET_OPTIONS || !opts ||
        vis_row >= opt_sheet->nrows) {
        return false;
    }
    if (col != 1) {
        td_meta_err(err, err_cap, "only the 'value' column is editable");
        return false;
    }
    phys = opt_sheet->row_order[vis_row];
    nv = td_sheet_get_cell(opt_sheet, phys, 0);
    vv = td_sheet_get_cell(opt_sheet, phys, 1);
    td_val_format(nv, TD_TYPE_STR, name_buf, sizeof(name_buf));
    td_val_format(vv, TD_TYPE_STR, val_buf, sizeof(val_buf));

    if (!td_options_set_by_name(opts, name_buf, val_buf, err, err_cap))
        return false;
    return td_options_save(opts, NULL, err, err_cap);
}

typedef struct {
    char *name;
    char *full_path;
    const char *kind;
    int64_t size;
    char mode[12];
    int64_t mtime;
    bool is_dir;
} td_dir_item_t;

static int td_dir_item_cmp(const void *a, const void *b)
{
    const td_dir_item_t *da = (const td_dir_item_t *)a;
    const td_dir_item_t *db = (const td_dir_item_t *)b;

    if (da->is_dir != db->is_dir)
        return da->is_dir ? -1 : 1;
    return strcmp(da->name, db->name);
}

static void td_format_mode(mode_t m, char out[12])
{
    out[0] = S_ISDIR(m) ? 'd' : (S_ISLNK(m) ? 'l' : '-');
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

td_sheet_t *td_meta_dir_build(const char *dirpath, char *err, size_t err_cap)
{
    DIR *dp;
    struct dirent *de;
    td_dir_item_t *items = NULL;
    size_t n = 0;
    size_t cap = 0;
    td_sheet_t *s;

    if (!dirpath || !dirpath[0]) {
        td_meta_err(err, err_cap, "missing directory path");
        return NULL;
    }
    dp = opendir(dirpath);
    if (!dp) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "cannot open %s: %s", dirpath, strerror(errno));
        return NULL;
    }

    while ((de = readdir(dp)) != NULL) {
        char *full;
        struct stat st;
        td_dir_item_t item;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        full = tam_path_join(dirpath, de->d_name);
        if (!full)
            continue;
        memset(&item, 0, sizeof(item));
        item.name = strdup(de->d_name);
        item.full_path = full;
        if (!item.name) {
            free(full);
            continue;
        }
        if (lstat(full, &st) == 0) {
            item.is_dir = S_ISDIR(st.st_mode);
            item.kind = S_ISDIR(st.st_mode)
                            ? "dir"
                            : (S_ISLNK(st.st_mode) ? "link" : "file");
            item.size = (int64_t)st.st_size;
            item.mtime = (int64_t)st.st_mtime;
            td_format_mode(st.st_mode, item.mode);
        } else {
            item.kind = "unknown";
            snprintf(item.mode, sizeof(item.mode), "----------");
        }

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            td_dir_item_t *ni = realloc(items, ncap * sizeof(*ni));

            if (!ni) {
                free(item.name);
                free(item.full_path);
                break;
            }
            items = ni;
            cap = ncap;
        }
        items[n++] = item;
    }
    closedir(dp);

    if (n > 1)
        qsort(items, n, sizeof(*items), td_dir_item_cmp);

    s = td_sheet_new(dirpath, dirpath);
    if (!s) {
        for (size_t i = 0; i < n; i++) {
            free(items[i].name);
            free(items[i].full_path);
        }
        free(items);
        return NULL;
    }
    s->kind = TD_SHEET_DIR;

    if (td_sheet_add_col(s, "name", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "type", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "size", TD_TYPE_INT, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "mode", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "mtime", TD_TYPE_DATE, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "path", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX) {
        for (size_t i = 0; i < n; i++) {
            free(items[i].name);
            free(items[i].full_path);
        }
        free(items);
        td_sheet_free(s);
        return NULL;
    }

    for (size_t i = 0; i < n; i++) {
        size_t r = td_sheet_add_row(s);
        struct tm tm;
        char tbuf[32] = "";

        if (r != SIZE_MAX) {
            time_t mt = (time_t)items[i].mtime;

            if (localtime_r(&mt, &tm))
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
            td_sheet_set_cell(s, r, 0, td_meta_str(s, items[i].name));
            td_sheet_set_cell(s, r, 1, td_meta_str(s, items[i].kind));
            td_sheet_set_cell(s, r, 2, td_meta_int(items[i].size));
            td_sheet_set_cell(s, r, 3, td_meta_str(s, items[i].mode));
            td_sheet_set_cell(s, r, 4, td_meta_str(s, tbuf));
            td_sheet_set_cell(s, r, 5, td_meta_str(s, items[i].full_path));
        }
        free(items[i].name);
        free(items[i].full_path);
    }
    free(items);
    return s;
}

td_sheet_t *td_meta_help_build(void)
{
    size_t n = td_cmd_count();
    td_sheet_t *s = td_sheet_new("help", NULL);
    td_help_meta_t *meta;

    if (!s)
        return NULL;
    s->kind = TD_SHEET_HELP;

    if (td_sheet_add_col(s, "key", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "command", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "input", TD_TYPE_STR, -TD_AUTO_WIDTH_CAP) == SIZE_MAX ||
        td_sheet_add_col(s, "description", TD_TYPE_STR, -64) == SIZE_MAX) {
        td_sheet_free(s);
        return NULL;
    }

    meta = calloc(1, sizeof(*meta));
    if (!meta) {
        td_sheet_free(s);
        return NULL;
    }
    if (n > 0) {
        meta->cmds = calloc(n, sizeof(td_cmd_t));
        if (!meta->cmds) {
            free(meta);
            td_sheet_free(s);
            return NULL;
        }
    }
    meta->count = n;
    s->meta = meta;
    s->meta_free = td_help_meta_free;

    for (size_t i = 0; i < n; i++) {
        const td_cmd_meta_t *cm = td_cmd_at(i);
        size_t r = td_sheet_add_row(s);

        if (r == SIZE_MAX) {
            td_sheet_free(s);
            return NULL;
        }
        meta->cmds[r] = cm->cmd;
        td_sheet_set_cell(s, r, 0, td_meta_str(s, cm->keystrokes));
        td_sheet_set_cell(s, r, 1, td_meta_str(s, cm->longname));
        td_sheet_set_cell(s, r, 2, td_meta_str(s, cm->takes_input ? "yes" : ""));
        td_sheet_set_cell(s, r, 3, td_meta_str(s, cm->description ? cm->description : ""));
    }

    return s;
}

bool td_meta_sheets_drop_row(td_sheet_t *sheets_sheet, td_stack_t *stack,
                             size_t vis_row, char *err, size_t err_cap)
{
    td_sheets_meta_t *meta;
    size_t phys;
    uint64_t target_id;

    if (!sheets_sheet || sheets_sheet->kind != TD_SHEET_SHEETS || !stack ||
        vis_row >= sheets_sheet->nrows) {
        return false;
    }
    meta = (td_sheets_meta_t *)sheets_sheet->meta;
    phys = sheets_sheet->row_order[vis_row];
    if (!meta || phys >= meta->count)
        return false;

    target_id = meta->sheet_ids[phys];
    if (target_id == sheets_sheet->id) {
        td_meta_err(err, err_cap, "cannot drop active sheets metasheet");
        return false;
    }
    if (stack->len <= 2) {
        td_meta_err(err, err_cap, "cannot drop the last underlying sheet");
        return false;
    }
    if (!td_stack_remove_by_id(stack, target_id)) {
        td_meta_err(err, err_cap, "sheet not found");
        return false;
    }
    td_sheet_delete_row(sheets_sheet, vis_row);
    return true;
}

td_sheet_t *td_meta_dive_row(const td_sheet_t *s, td_stack_t *stack,
                             const td_csv_opts_t *csv_opts,
                             int *cmd_out, char *err, size_t err_cap)
{
    size_t phys;

    if (cmd_out)
        *cmd_out = 0;
    if (!s || s->nrows == 0 || s->cursor_row >= s->nrows) {
        td_meta_err(err, err_cap, "no row selected");
        return NULL;
    }
    phys = s->row_order[s->cursor_row];

    if (s->kind == TD_SHEET_SHEETS) {
        const td_sheets_meta_t *meta = (const td_sheets_meta_t *)s->meta;
        uint64_t id;

        if (!meta || phys >= meta->count || !stack) {
            td_meta_err(err, err_cap, "invalid sheets metadata");
            return NULL;
        }
        id = meta->sheet_ids[phys];
        if (id != s->id) {
            td_sheet_t *popped = td_stack_pop(stack);

            td_stack_switch_to(stack, id);
            td_sheet_free(popped);
        }
        return NULL;
    }

    if (s->kind == TD_SHEET_DIR) {
        td_val_t pv = td_sheet_get_cell(s, phys, 5);
        char path[1024];

        td_val_format(pv, TD_TYPE_STR, path, sizeof(path));
        if (!path[0]) {
            td_meta_err(err, err_cap, "missing path");
            return NULL;
        }
        return td_load_path(path, csv_opts, TD_FMT_AUTO, err, err_cap);
    }

    if (s->kind == TD_SHEET_HELP) {
        const td_help_meta_t *meta = (const td_help_meta_t *)s->meta;

        if (!meta || phys >= meta->count) {
            td_meta_err(err, err_cap, "invalid help metadata");
            return NULL;
        }
        if (cmd_out)
            *cmd_out = (int)meta->cmds[phys];
        return NULL;
    }

    return NULL;
}

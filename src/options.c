#include "options.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sheet.h"
#include "tam.h"

typedef struct {
    const char *name;
    const char *def_str;
    const char *desc;
} td_option_meta_t;

static const td_option_meta_t td_opt_table[] = {
    {
        "default_delimiter",
        "auto",
        "Default CSV/TSV delimiter (auto, comma, tab, or single char)"
    },
    {
        "csv_has_header",
        "true",
        "Treat first row of CSV/TSV files as column headers"
    },
    {
        "auto_width_cap",
        "40",
        "Maximum column auto-width in characters (3..200)"
    },
    {
        "braille_plot",
        "true",
        "Use Unicode Braille (U+2800..U+28FF) for plots when UTF-8 is active"
    },
    {
        "record_cmdlog",
        "true",
        "Record executed commands in the Command Log (D)"
    },
    {
        "confirm_quit",
        "false",
        "Require confirmation before quitting all sheets (gq)"
    }
};

static void td_opt_err(char *err, size_t err_cap, const char *msg)
{
    if (err && err_cap > 0)
        snprintf(err, err_cap, "%s", msg ? msg : "");
}

void td_options_init(td_options_t *opts)
{
    if (!opts)
        return;
    memset(opts, 0, sizeof(*opts));
    opts->default_delimiter = '\0';
    opts->csv_has_header = true;
    opts->auto_width_cap = TD_AUTO_WIDTH_CAP;
    opts->braille_plot = true;
    opts->record_cmdlog = true;
    opts->confirm_quit = false;
}

void td_options_free(td_options_t *opts)
{
    if (!opts)
        return;
    free(opts->config_path);
    opts->config_path = NULL;
}

size_t td_options_count(void)
{
    return sizeof(td_opt_table) / sizeof(td_opt_table[0]);
}

const char *td_options_name(size_t idx)
{
    if (idx >= td_options_count())
        return NULL;
    return td_opt_table[idx].name;
}

const char *td_options_default_str(size_t idx)
{
    if (idx >= td_options_count())
        return NULL;
    return td_opt_table[idx].def_str;
}

const char *td_options_desc(size_t idx)
{
    if (idx >= td_options_count())
        return NULL;
    return td_opt_table[idx].desc;
}

bool td_options_format_value(const td_options_t *opts, size_t idx,
                             char *buf, size_t cap)
{
    if (!opts || !buf || cap == 0 || idx >= td_options_count())
        return false;

    switch (idx) {
    case 0:
        if (opts->default_delimiter == '\0')
            snprintf(buf, cap, "auto");
        else if (opts->default_delimiter == '\t')
            snprintf(buf, cap, "tab");
        else if (opts->default_delimiter == ',')
            snprintf(buf, cap, "comma");
        else
            snprintf(buf, cap, "%c", opts->default_delimiter);
        return true;
    case 1:
        snprintf(buf, cap, "%s", opts->csv_has_header ? "true" : "false");
        return true;
    case 2:
        snprintf(buf, cap, "%d", opts->auto_width_cap);
        return true;
    case 3:
        snprintf(buf, cap, "%s", opts->braille_plot ? "true" : "false");
        return true;
    case 4:
        snprintf(buf, cap, "%s", opts->record_cmdlog ? "true" : "false");
        return true;
    case 5:
        snprintf(buf, cap, "%s", opts->confirm_quit ? "true" : "false");
        return true;
    default:
        return false;
    }
}

static bool td_parse_bool(const char *val, bool *out)
{
    if (!val || !out)
        return false;
    if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
        strcmp(val, "yes") == 0 || strcmp(val, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0 ||
        strcmp(val, "no") == 0 || strcmp(val, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool td_options_set_by_name(td_options_t *opts, const char *name,
                            const char *value, char *err, size_t err_cap)
{
    if (!opts || !name || !value) {
        td_opt_err(err, err_cap, "invalid option");
        return false;
    }

    if (strcmp(name, "default_delimiter") == 0) {
        if (strcmp(value, "auto") == 0 || value[0] == '\0') {
            opts->default_delimiter = '\0';
            return true;
        }
        if (strcmp(value, "tab") == 0 || strcmp(value, "\\t") == 0) {
            opts->default_delimiter = '\t';
            return true;
        }
        if (strcmp(value, "comma") == 0) {
            opts->default_delimiter = ',';
            return true;
        }
        if (strlen(value) == 1 && (unsigned char)value[0] >= 0x20) {
            opts->default_delimiter = value[0];
            return true;
        }
        td_opt_err(err, err_cap, "invalid delimiter (use auto, tab, comma, or 1 char)");
        return false;
    }
    if (strcmp(name, "csv_has_header") == 0) {
        if (!td_parse_bool(value, &opts->csv_has_header)) {
            td_opt_err(err, err_cap, "expected true or false");
            return false;
        }
        return true;
    }
    if (strcmp(name, "auto_width_cap") == 0) {
        char *end = NULL;
        long v = strtol(value, &end, 10);

        if (!value[0] || !end || *end != '\0' || v < 3 || v > 200) {
            td_opt_err(err, err_cap, "auto_width_cap must be 3..200");
            return false;
        }
        opts->auto_width_cap = (int)v;
        return true;
    }
    if (strcmp(name, "braille_plot") == 0) {
        if (!td_parse_bool(value, &opts->braille_plot)) {
            td_opt_err(err, err_cap, "expected true or false");
            return false;
        }
        return true;
    }
    if (strcmp(name, "record_cmdlog") == 0) {
        if (!td_parse_bool(value, &opts->record_cmdlog)) {
            td_opt_err(err, err_cap, "expected true or false");
            return false;
        }
        return true;
    }
    if (strcmp(name, "confirm_quit") == 0) {
        if (!td_parse_bool(value, &opts->confirm_quit)) {
            td_opt_err(err, err_cap, "expected true or false");
            return false;
        }
        return true;
    }

    if (err && err_cap > 0)
        snprintf(err, err_cap, "unknown option: %s", name);
    return false;
}

typedef struct {
    td_options_t *opts;
    char *err;
    size_t err_cap;
    bool ok;
} td_ini_load_ctx_t;

static int td_options_ini_cb(void *user, const char *section, const char *name,
                             const char *value)
{
    td_ini_load_ctx_t *ctx = (td_ini_load_ctx_t *)user;

    if (section && section[0] != '\0' && strcmp(section, "tam-data") != 0 &&
        strcmp(section, "general") != 0) {
        return 0;
    }
    if (!td_options_set_by_name(ctx->opts, name, value ? value : "",
                                ctx->err, ctx->err_cap)) {
        ctx->ok = false;
        return 1;
    }
    return 0;
}

static char *td_resolve_config_path(const td_options_t *opts,
                                    const char *override_path, bool create)
{
    const char *p = override_path ? override_path : (opts ? opts->config_path : NULL);

    if (p && p[0])
        return strdup(p);
    return tam_xdg_path(TAM_XDG_CONFIG, "tam-data", "config.ini", create);
}

bool td_options_load(td_options_t *opts, const char *override_path,
                     char *err, size_t err_cap)
{
    char *path;
    td_ini_load_ctx_t ctx;
    int rc;

    if (!opts)
        return false;
    path = td_resolve_config_path(opts, override_path, false);
    if (!path)
        return true; /* no XDG_CONFIG_HOME or HOME: use defaults */

    free(opts->config_path);
    opts->config_path = path;

    if (!tam_fs_file_exists(path))
        return true;

    ctx.opts = opts;
    ctx.err = err;
    ctx.err_cap = err_cap;
    ctx.ok = true;
    rc = tam_ini_parse_file(path, td_options_ini_cb, &ctx);
    if (rc < 0) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "failed to read config: %s", path);
        return false;
    }
    return ctx.ok;
}

bool td_options_save(const td_options_t *opts, const char *override_path,
                     char *err, size_t err_cap)
{
    char *path;
    tam_str_t out;
    bool ok;

    if (!opts)
        return false;
    path = td_resolve_config_path(opts, override_path, true);
    if (!path) {
        td_opt_err(err, err_cap, "cannot resolve config path");
        return false;
    }

    out = tam_str_new("[tam-data]\n");
    if (!out) {
        free(path);
        td_opt_err(err, err_cap, "out of memory");
        return false;
    }
    for (size_t i = 0; i < td_options_count(); i++) {
        char vbuf[64];

        if (!td_options_format_value(opts, i, vbuf, sizeof(vbuf)))
            continue;
        out = tam_str_cat_printf(out, "%s = %s\n", td_options_name(i), vbuf);
        if (!out) {
            free(path);
            td_opt_err(err, err_cap, "out of memory");
            return false;
        }
    }

    ok = tam_fs_write_atomic(path, out, tam_str_len(out), 0644);
    if (!ok && err && err_cap > 0) {
        snprintf(err, err_cap, "failed to save %s: %s", path, strerror(errno));
    }
    tam_str_free(out);
    free(path);
    return ok;
}

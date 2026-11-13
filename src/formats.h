#ifndef TD_FORMATS_H
#define TD_FORMATS_H

#include <stdbool.h>
#include <stddef.h>

#include "csv.h"
#include "sheet.h"

typedef enum {
    TD_FMT_AUTO = 0,
    TD_FMT_CSV,
    TD_FMT_TSV,
    TD_FMT_JSON,
    TD_FMT_JSONL,
    TD_FMT_INI,
    TD_FMT_FWF
} td_format_t;

td_format_t td_format_from_path(const char *path);

td_sheet_t *td_ini_load_mem(const char *name, const char *source_path,
                            const char *buf, size_t len,
                            char *err, size_t err_cap);
td_sheet_t *td_ini_load_file(const char *path, char *err, size_t err_cap);

td_sheet_t *td_fwf_load_mem(const char *name, const char *source_path,
                            const char *buf, size_t len, bool has_header,
                            char *err, size_t err_cap);
td_sheet_t *td_fwf_load_file(const char *path, bool has_header,
                             char *err, size_t err_cap);

td_sheet_t *td_ps_load(char *err, size_t err_cap);

td_sheet_t *td_load_path(const char *path, const td_csv_opts_t *csv_opts,
                         td_format_t forced_fmt, char *err, size_t err_cap);

#endif /* TD_FORMATS_H */

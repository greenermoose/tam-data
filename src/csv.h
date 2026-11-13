#ifndef TD_CSV_H
#define TD_CSV_H

#include <stdbool.h>
#include <stddef.h>

#include "sheet.h"
#include "tam.h"

typedef struct {
    char delimiter;   /* ',', '\t', custom char, or '\0' to auto-detect */
    bool has_header;  /* true: first record names columns; false: col1..colN */
    bool strict_tsv;  /* true: fast unquoted scanner for tab-delimited text */
} td_csv_opts_t;

void td_csv_opts_default(td_csv_opts_t *opts);
char td_csv_detect_delimiter(const char *path, const char *buf, size_t len);

td_sheet_t *td_csv_load_mem(const char *name, const char *source_path,
                            const char *buf, size_t len,
                            const td_csv_opts_t *opts,
                            char *err, size_t err_cap);
td_sheet_t *td_csv_load_fd(int fd, const char *name,
                           const td_csv_opts_t *opts,
                           char *err, size_t err_cap);
td_sheet_t *td_csv_load_file(const char *path, const td_csv_opts_t *opts,
                             char *err, size_t err_cap);

tam_str_t td_csv_format_sheet(const td_sheet_t *s, char delimiter);
bool td_csv_save_sheet(td_sheet_t *s, const char *path, char delimiter,
                       char *err, size_t err_cap);

#endif /* TD_CSV_H */

#ifndef TD_JSON_H
#define TD_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "sheet.h"
#include "tam.h"

td_sheet_t *td_json_load_mem(const char *name, const char *source_path,
                             const char *buf, size_t len, bool jsonl,
                             char *err, size_t err_cap);
td_sheet_t *td_json_load_file(const char *path, bool jsonl,
                              char *err, size_t err_cap);

tam_str_t td_json_format_sheet(const td_sheet_t *s, bool jsonl);
bool td_json_save_sheet(td_sheet_t *s, const char *path, bool jsonl,
                        char *err, size_t err_cap);

#endif /* TD_JSON_H */

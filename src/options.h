#ifndef TD_OPTIONS_H
#define TD_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char default_delimiter;  /* '\0' for auto, or ',', '\t', etc. */
    bool csv_has_header;
    int auto_width_cap;
    bool braille_plot;
    bool record_cmdlog;
    bool confirm_quit;
    char *config_path;       /* resolved or overridden config file path */
} td_options_t;

void td_options_init(td_options_t *opts);
void td_options_free(td_options_t *opts);

bool td_options_load(td_options_t *opts, const char *override_path,
                     char *err, size_t err_cap);
bool td_options_save(const td_options_t *opts, const char *override_path,
                     char *err, size_t err_cap);

size_t td_options_count(void);
const char *td_options_name(size_t idx);
const char *td_options_default_str(size_t idx);
const char *td_options_desc(size_t idx);
bool td_options_format_value(const td_options_t *opts, size_t idx,
                             char *buf, size_t cap);
bool td_options_set_by_name(td_options_t *opts, const char *name,
                            const char *value, char *err, size_t err_cap);

#endif /* TD_OPTIONS_H */

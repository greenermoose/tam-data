#ifndef TD_TERM_H
#define TD_TERM_H

#include "tam.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#define TD_COLOR_DEFAULT 255u

enum {
    TD_ATTR_NONE      = 0,
    TD_ATTR_BOLD      = 1u << 0,
    TD_ATTR_REV       = 1u << 1,
    TD_ATTR_UNDERLINE = 1u << 2,
    TD_ATTR_DIM       = 1u << 3
};

typedef struct {
    uint32_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
} td_cell_t;

typedef struct {
    const char *name;
    bool utf8;
    bool alt_screen;
    bool sync_output;
    bool color256;
    int colors;
} td_term_profile_t;

typedef struct {
    const char *vline;
    const char *hline;
    const char *cross;
    const char *ellipsis;
    const char *dot;
    const char *keysep;
} td_glyphs_t;

typedef struct {
    int fd_in;
    int fd_out;
    bool opened;
    bool raw_active;
    bool alternate;
    int rows;
    int cols;
    td_term_profile_t profile;
    td_glyphs_t glyphs;
    td_cell_t *front;
    td_cell_t *back;
    tam_str_t frame;
    struct termios saved;
    struct termios raw;
    bool force_full;
} td_term_t;

typedef enum {
    TD_TERM_EVENT_NONE = 0,
    TD_TERM_EVENT_RESIZE,
    TD_TERM_EVENT_SUSPEND,
    TD_TERM_EVENT_CONTINUE,
    TD_TERM_EVENT_QUIT
} td_term_event_t;

void td_term_profile_detect(td_term_profile_t *p, const char *term,
                            const char *locale);
void td_term_glyphs(td_glyphs_t *g, bool utf8);

int td_term_init(td_term_t *t, int rows, int cols, const char *term,
                 const char *locale);
int td_term_open(td_term_t *t, const char *term, const char *locale);
void td_term_close(td_term_t *t);
int td_term_resize(td_term_t *t, int rows, int cols);
void td_term_suspend(td_term_t *t);
void td_term_resume(td_term_t *t);

void td_term_clear_back(td_term_t *t);
void td_term_put_cell(td_term_t *t, int row, int col, uint32_t ch, uint8_t fg,
                      uint8_t bg, uint8_t attr);
int td_term_put_str(td_term_t *t, int row, int col, int maxw, const char *s,
                    uint8_t fg, uint8_t bg, uint8_t attr);
void td_term_fill_rect(td_term_t *t, int row, int col, int height, int width,
                       uint32_t ch, uint8_t fg, uint8_t bg, uint8_t attr);
void td_term_present(td_term_t *t);

td_cell_t td_term_front_cell(const td_term_t *t, int row, int col);
td_cell_t td_term_back_cell(const td_term_t *t, int row, int col);
const char *td_term_frame(const td_term_t *t);
size_t td_term_frame_len(const td_term_t *t);

td_term_event_t td_term_next_event(td_term_t *t);

#endif /* TD_TERM_H */

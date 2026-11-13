#ifndef TD_INPUT_H
#define TD_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TD_KEY_NONE = 0,
    TD_KEY_CHAR,
    TD_KEY_ENTER,
    TD_KEY_TAB,
    TD_KEY_BACKTAB,
    TD_KEY_BACKSPACE,
    TD_KEY_ESC,
    TD_KEY_DELETE,
    TD_KEY_INSERT,
    TD_KEY_UP,
    TD_KEY_DOWN,
    TD_KEY_LEFT,
    TD_KEY_RIGHT,
    TD_KEY_HOME,
    TD_KEY_END,
    TD_KEY_PGUP,
    TD_KEY_PGDN,
    TD_KEY_F1,
    TD_KEY_F2,
    TD_KEY_F3,
    TD_KEY_F4,
    TD_KEY_F5,
    TD_KEY_F6,
    TD_KEY_F7,
    TD_KEY_F8,
    TD_KEY_F9,
    TD_KEY_F10,
    TD_KEY_F11,
    TD_KEY_F12
} td_key_kind_t;

typedef struct {
    td_key_kind_t kind;
    uint32_t ch;
} td_key_t;

typedef struct {
    int fd;
    bool mem;
    const unsigned char *data;
    size_t len;
    size_t pos;
    unsigned char buf[64];
    size_t buf_pos;
    size_t buf_len;
    int pushback;
    int esc_timeout_ms;
} td_key_reader_t;

void td_key_reader_mem(td_key_reader_t *r, const void *data, size_t len);
void td_key_reader_fd(td_key_reader_t *r, int fd);
td_key_t td_key_read(td_key_reader_t *r);

typedef enum {
    TD_CMD_NONE = 0,
    TD_CMD_LEFT,
    TD_CMD_RIGHT,
    TD_CMD_UP,
    TD_CMD_DOWN,
    TD_CMD_FIRST_COL,
    TD_CMD_LAST_COL,
    TD_CMD_FIRST_ROW,
    TD_CMD_LAST_ROW,
    TD_CMD_PAGE_DOWN,
    TD_CMD_PAGE_UP,
    TD_CMD_NEXT_VALUE,
    TD_CMD_PREV_VALUE,
    TD_CMD_NEXT_NULL,
    TD_CMD_PREV_NULL,
    TD_CMD_MOVE_COL_LEFT,
    TD_CMD_MOVE_COL_RIGHT,
    TD_CMD_HIDE_COL,
    TD_CMD_TOGGLE_KEY_COL,
    TD_CMD_AUTOSIZE_COL,
    TD_CMD_AUTOSIZE_ALL_COLS,
    TD_CMD_SET_COL_WIDTH,
    TD_CMD_UNHIDE_ALL_COLS,
    TD_CMD_RENAME_COL,
    TD_CMD_SAVE_SHEET,
    TD_CMD_TYPE_ANY,
    TD_CMD_TYPE_STR,
    TD_CMD_TYPE_INT,
    TD_CMD_TYPE_FLOAT,
    TD_CMD_TYPE_CURRENCY,
    TD_CMD_TYPE_DATE,
    TD_CMD_POP_SHEET,
    TD_CMD_QUIT_ALL,
    TD_CMD_SELECT_ROW,
    TD_CMD_UNSELECT_ROW,
    TD_CMD_TOGGLE_ROW,
    TD_CMD_SELECT_ALL,
    TD_CMD_UNSELECT_ALL,
    TD_CMD_TOGGLE_ALL,
    TD_CMD_REDRAW
} td_cmd_t;

typedef struct {
    int prefix; /* 0 none, 1 g, 2 z, 3 gz */
} td_input_t;

void td_input_init(td_input_t *in);
td_cmd_t td_input_feed(td_input_t *in, td_key_t key);
const char *td_input_pending(const td_input_t *in);
void td_input_clear(td_input_t *in);

typedef enum {
    TD_PROMPT_IGNORE = 0,
    TD_PROMPT_CHANGED,
    TD_PROMPT_ACCEPT,
    TD_PROMPT_CANCEL
} td_prompt_result_t;

typedef enum {
    TD_PROMPT_NONE = 0,
    TD_PROMPT_RENAME_COL,
    TD_PROMPT_COL_WIDTH,
    TD_PROMPT_SAVE_SHEET
} td_prompt_kind_t;

typedef struct {
    bool active;
    td_prompt_kind_t kind;
    char *label;
    char *buf;
    size_t len;
    size_t cap;
    size_t cursor;
} td_prompt_t;

void td_prompt_init(td_prompt_t *p);
void td_prompt_free(td_prompt_t *p);
void td_prompt_start(td_prompt_t *p, const char *label, const char *initial);
void td_prompt_start_kind(td_prompt_t *p, td_prompt_kind_t kind,
                          const char *label, const char *initial);
void td_prompt_stop(td_prompt_t *p);
td_prompt_kind_t td_prompt_kind(const td_prompt_t *p);
const char *td_prompt_label(const td_prompt_t *p);
const char *td_prompt_text(const td_prompt_t *p);
size_t td_prompt_cursor(const td_prompt_t *p);
td_prompt_result_t td_prompt_key(td_prompt_t *p, td_key_t key);

#endif /* TD_INPUT_H */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "input.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utf8.h"

void td_key_reader_mem(td_key_reader_t *r, const void *data, size_t len)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->mem = true;
    r->data = data;
    r->len = len;
    r->pushback = -1;
    r->esc_timeout_ms = 0;
}

void td_key_reader_fd(td_key_reader_t *r, int fd)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->mem = false;
    r->pushback = -1;
    r->esc_timeout_ms = 25;
}

static int td_reader_get(td_key_reader_t *r, int timeout_ms)
{
    int b;

    if (r->pushback >= 0) {
        b = r->pushback;
        r->pushback = -1;
        return b;
    }
    if (r->mem) {
        if (r->pos >= r->len)
            return -1;
        return r->data[r->pos++];
    }
    if (r->fd < 0)
        return -1;

    if (r->buf_pos < r->buf_len)
        return r->buf[r->buf_pos++];

    struct pollfd pfd = { .fd = r->fd, .events = POLLIN, .revents = 0 };
    int rc = poll(&pfd, 1, timeout_ms);

    if (rc <= 0)
        return -1;
    ssize_t n = read(r->fd, r->buf, sizeof(r->buf));
    if (n <= 0)
        return -1;
    r->buf_pos = 0;
    r->buf_len = (size_t)n;
    return r->buf[r->buf_pos++];
}

static void td_reader_unget(td_key_reader_t *r, int b)
{
    if (b >= 0)
        r->pushback = b;
}

static td_key_t td_key_make(td_key_kind_t kind)
{
    td_key_t k = { kind, 0 };

    return k;
}

static td_key_t td_key_char(uint32_t ch)
{
    td_key_t k = { TD_KEY_CHAR, ch };

    return k;
}

static td_key_t td_key_from_function(int n)
{
    if (n >= 1 && n <= 12)
        return td_key_make((td_key_kind_t)(TD_KEY_F1 + n - 1));
    return td_key_make(TD_KEY_NONE);
}

static int td_parse_first_int(const char *params, size_t len)
{
    int value = 0;
    size_t i = 0;

    while (i < len && (params[i] < '0' || params[i] > '9'))
        i++;
    while (i < len && params[i] >= '0' && params[i] <= '9') {
        if (value < 10000)
            value = value * 10 + (params[i] - '0');
        i++;
    }
    return value;
}

static td_key_t td_key_from_csi(char final, const char *params, size_t len)
{
    switch (final) {
    case 'A': return td_key_make(TD_KEY_UP);
    case 'B': return td_key_make(TD_KEY_DOWN);
    case 'C': return td_key_make(TD_KEY_RIGHT);
    case 'D': return td_key_make(TD_KEY_LEFT);
    case 'H': return td_key_make(TD_KEY_HOME);
    case 'F': return td_key_make(TD_KEY_END);
    case 'Z': return td_key_make(TD_KEY_BACKTAB);
    case 'P': return td_key_make(TD_KEY_F1);
    case 'Q': return td_key_make(TD_KEY_F2);
    case 'R': return td_key_make(TD_KEY_F3);
    case 'S': return td_key_make(TD_KEY_F4);
    case '~': {
        int n = td_parse_first_int(params, len);

        switch (n) {
        case 1:
        case 7:  return td_key_make(TD_KEY_HOME);
        case 2:  return td_key_make(TD_KEY_INSERT);
        case 3:  return td_key_make(TD_KEY_DELETE);
        case 4:
        case 8:  return td_key_make(TD_KEY_END);
        case 5:  return td_key_make(TD_KEY_PGUP);
        case 6:  return td_key_make(TD_KEY_PGDN);
        case 11: return td_key_make(TD_KEY_F1);
        case 12: return td_key_make(TD_KEY_F2);
        case 13: return td_key_make(TD_KEY_F3);
        case 14: return td_key_make(TD_KEY_F4);
        case 15: return td_key_make(TD_KEY_F5);
        case 17: return td_key_make(TD_KEY_F6);
        case 18: return td_key_make(TD_KEY_F7);
        case 19: return td_key_make(TD_KEY_F8);
        case 20: return td_key_make(TD_KEY_F9);
        case 21: return td_key_make(TD_KEY_F10);
        case 23: return td_key_make(TD_KEY_F11);
        case 24: return td_key_make(TD_KEY_F12);
        default: return td_key_make(TD_KEY_NONE);
        }
    }
    default:
        return td_key_make(TD_KEY_NONE);
    }
}

static td_key_t td_parse_csi(td_key_reader_t *r)
{
    char params[32];
    size_t len = 0;

    for (;;) {
        int b = td_reader_get(r, r->esc_timeout_ms);

        if (b < 0)
            return td_key_make(TD_KEY_NONE);
        if (b >= 0x40 && b <= 0x7e) {
            if (b == '[') {
                int f = td_reader_get(r, r->esc_timeout_ms);

                if (f >= 'A' && f <= 'E')
                    return td_key_from_function(f - 'A' + 1);
                return td_key_make(TD_KEY_NONE);
            }
            return td_key_from_csi((char)b, params, len);
        }
        if (len + 1 < sizeof(params))
            params[len++] = (char)b;
    }
}

static td_key_t td_parse_ss3(td_key_reader_t *r)
{
    int b = td_reader_get(r, r->esc_timeout_ms);

    switch (b) {
    case 'A': return td_key_make(TD_KEY_UP);
    case 'B': return td_key_make(TD_KEY_DOWN);
    case 'C': return td_key_make(TD_KEY_RIGHT);
    case 'D': return td_key_make(TD_KEY_LEFT);
    case 'H': return td_key_make(TD_KEY_HOME);
    case 'F': return td_key_make(TD_KEY_END);
    case 'P': return td_key_make(TD_KEY_F1);
    case 'Q': return td_key_make(TD_KEY_F2);
    case 'R': return td_key_make(TD_KEY_F3);
    case 'S': return td_key_make(TD_KEY_F4);
    default:  return td_key_make(TD_KEY_NONE);
    }
}

static td_key_t td_parse_utf8(td_key_reader_t *r, int first)
{
    unsigned char bytes[4];
    int extra;
    size_t n = 1;

    bytes[0] = (unsigned char)first;
    if ((first & 0xE0) == 0xC0)
        extra = 1;
    else if ((first & 0xF0) == 0xE0)
        extra = 2;
    else if ((first & 0xF8) == 0xF0)
        extra = 3;
    else
        return td_key_char(0xFFFD);

    for (int i = 0; i < extra; i++) {
        int b = td_reader_get(r, r->esc_timeout_ms);

        if (b < 0)
            return td_key_char(0xFFFD);
        bytes[n++] = (unsigned char)b;
    }

    const char *p = (const char *)bytes;
    const char *end = p + n;
    uint32_t cp = td_utf8_decode(&p, end);

    if (p != end)
        cp = 0xFFFD;
    return td_key_char(cp);
}

td_key_t td_key_read(td_key_reader_t *r)
{
    int b;

    if (!r)
        return td_key_make(TD_KEY_NONE);

    b = td_reader_get(r, -1);
    if (b < 0)
        return td_key_make(TD_KEY_NONE);

    if (b == 0x1b) {
        int next = td_reader_get(r, r->esc_timeout_ms);

        if (next < 0)
            return td_key_make(TD_KEY_ESC);
        if (next == '[')
            return td_parse_csi(r);
        if (next == 'O')
            return td_parse_ss3(r);
        td_reader_unget(r, next);
        return td_key_make(TD_KEY_ESC);
    }
    if (b == '\r' || b == '\n')
        return td_key_make(TD_KEY_ENTER);
    if (b == '\t')
        return td_key_make(TD_KEY_TAB);
    if (b == 127 || b == 8)
        return td_key_make(TD_KEY_BACKSPACE);
    if (b < 0x20)
        return td_key_char((uint32_t)b);
    if (b < 0x80)
        return td_key_char((uint32_t)b);
    return td_parse_utf8(r, b);
}

void td_input_init(td_input_t *in)
{
    if (in)
        in->prefix = 0;
}

void td_input_clear(td_input_t *in)
{
    if (in)
        in->prefix = 0;
}

const char *td_input_pending(const td_input_t *in)
{
    if (!in || in->prefix == 0)
        return "";
    if (in->prefix == 1)
        return "g";
    if (in->prefix == 2)
        return "z";
    return "gz";
}

static td_cmd_t td_input_single(td_key_t key)
{
    if (key.kind == TD_KEY_UP)
        return TD_CMD_UP;
    if (key.kind == TD_KEY_DOWN)
        return TD_CMD_DOWN;
    if (key.kind == TD_KEY_LEFT)
        return TD_CMD_LEFT;
    if (key.kind == TD_KEY_RIGHT)
        return TD_CMD_RIGHT;
    if (key.kind == TD_KEY_HOME)
        return TD_CMD_FIRST_COL;
    if (key.kind == TD_KEY_END)
        return TD_CMD_LAST_COL;
    if (key.kind == TD_KEY_PGUP)
        return TD_CMD_PAGE_UP;
    if (key.kind == TD_KEY_PGDN)
        return TD_CMD_PAGE_DOWN;
    if (key.kind == TD_KEY_ENTER)
        return TD_CMD_DIVE_ROW;
    if (key.kind == TD_KEY_F1 || key.kind == TD_KEY_BACKSPACE)
        return TD_CMD_SHOW_HELP;
    if (key.kind != TD_KEY_CHAR)
        return TD_CMD_NONE;

    switch (key.ch) {
    case ' ': return TD_CMD_COMMAND_PALETTE;
    case 'h': return TD_CMD_LEFT;
    case 'j': return TD_CMD_DOWN;
    case 'k': return TD_CMD_UP;
    case 'l': return TD_CMD_RIGHT;
    case 'H': return TD_CMD_MOVE_COL_LEFT;
    case 'L': return TD_CMD_MOVE_COL_RIGHT;
    case 'G': return TD_CMD_LAST_ROW;
    case '<': return TD_CMD_PREV_VALUE;
    case '>': return TD_CMD_NEXT_VALUE;
    case '{': return TD_CMD_PREV_NULL;
    case '}': return TD_CMD_NEXT_NULL;
    case 'q': return TD_CMD_POP_SHEET;
    case 's': return TD_CMD_SELECT_ROW;
    case 'u': return TD_CMD_UNSELECT_ROW;
    case 't': return TD_CMD_TOGGLE_ROW;
    case '|': return TD_CMD_SELECT_REGEX;
    case '\\': return TD_CMD_UNSELECT_REGEX;
    case '/': return TD_CMD_SEARCH_FWD;
    case '?': return TD_CMD_SEARCH_BWD;
    case 'n': return TD_CMD_SEARCH_NEXT;
    case 'N': return TD_CMD_SEARCH_PREV;
    case '"': return TD_CMD_DUP_SELECTED;
    case 'd': return TD_CMD_DELETE_ROW;
    case 'e': return TD_CMD_EDIT_CELL;
    case 'a': return TD_CMD_ADD_ROW;
    case 'y': return TD_CMD_YANK_ROW;
    case 'p': return TD_CMD_PASTE_AFTER;
    case 'P': return TD_CMD_PASTE_BEFORE;
    case 'U': return TD_CMD_UNDO;
    case 'R': return TD_CMD_REDO;
    case 'D': return TD_CMD_SHOW_CMDLOG;
    case 'S': return TD_CMD_SHOW_SHEETS;
    case 'C': return TD_CMD_SHOW_COLS;
    case 'O': return TD_CMD_SHOW_OPTIONS;
    case 'F': return TD_CMD_FREQ_COL;
    case 'I': return TD_CMD_DESCRIBE;
    case 'W': return TD_CMD_PIVOT;
    case 'M': return TD_CMD_MELT;
    case '.': return TD_CMD_PLOT_COL;
    case '+': return TD_CMD_AGGREGATOR;
    case '&': return TD_CMD_JOIN;
    case '=': return TD_CMD_ADD_COL_EXPR;
    case ':': return TD_CMD_SPLIT_COL_DELIM;
    case ';': return TD_CMD_SPLIT_COL_REGEX;
    case '[': return TD_CMD_SORT_ASC;
    case ']': return TD_CMD_SORT_DESC;
    case '-': return TD_CMD_HIDE_COL;
    case '!': return TD_CMD_TOGGLE_KEY_COL;
    case '_': return TD_CMD_AUTOSIZE_COL;
    case '^': return TD_CMD_RENAME_COL;
    case '#': return TD_CMD_TYPE_INT;
    case '%': return TD_CMD_TYPE_FLOAT;
    case '$': return TD_CMD_TYPE_CURRENCY;
    case '@': return TD_CMD_TYPE_DATE;
    case '~': return TD_CMD_TYPE_STR;
    case 2:   return TD_CMD_PAGE_UP;    /* Ctrl+B */
    case 6:   return TD_CMD_PAGE_DOWN;  /* Ctrl+F */
    case 12:  return TD_CMD_REDRAW;     /* Ctrl+L */
    case 19:  return TD_CMD_SAVE_SHEET; /* Ctrl+S */
    default:  return TD_CMD_NONE;
    }
}

td_cmd_t td_input_feed(td_input_t *in, td_key_t key)
{
    if (!in)
        return TD_CMD_NONE;

    if (key.kind == TD_KEY_ESC ||
        (key.kind == TD_KEY_CHAR && key.ch == 3)) {
        in->prefix = 0;
        return TD_CMD_NONE;
    }

    if (in->prefix == 0) {
        if (key.kind == TD_KEY_CHAR && key.ch == 'g') {
            in->prefix = 1;
            return TD_CMD_NONE;
        }
        if (key.kind == TD_KEY_CHAR && key.ch == 'z') {
            in->prefix = 2;
            return TD_CMD_NONE;
        }
        return td_input_single(key);
    }

    if (in->prefix == 1) {
        in->prefix = 0;
        if (key.kind != TD_KEY_CHAR)
            return TD_CMD_NONE;
        switch (key.ch) {
        case 'h': return TD_CMD_FIRST_COL;
        case 'j': return TD_CMD_LAST_ROW;
        case 'k': return TD_CMD_FIRST_ROW;
        case 'l': return TD_CMD_LAST_COL;
        case 'g': return TD_CMD_FIRST_ROW;
        case 'q': return TD_CMD_QUIT_ALL;
        case 's': return TD_CMD_SELECT_ALL;
        case 'u': return TD_CMD_UNSELECT_ALL;
        case 't': return TD_CMD_TOGGLE_ALL;
        case '|': return TD_CMD_SELECT_ALL_REGEX;
        case '\\': return TD_CMD_UNSELECT_ALL_REGEX;
        case '/': return TD_CMD_SEARCH_ALL_FWD;
        case '?': return TD_CMD_SEARCH_ALL_BWD;
        case 'd': return TD_CMD_DELETE_SELECTED;
        case 'e': return TD_CMD_EDIT_SELECTED;
        case '=': return TD_CMD_SET_COL_EXPR;
        case 'a': return TD_CMD_ADD_ROWS;
        case 'y': return TD_CMD_YANK_SELECTED;
        case '"': return TD_CMD_DUP_ROW;
        case '[': return TD_CMD_SORT_KEYS_ASC;
        case ']': return TD_CMD_SORT_KEYS_DESC;
        case '_': return TD_CMD_AUTOSIZE_ALL_COLS;
        case 'v': return TD_CMD_UNHIDE_ALL_COLS;
        case 'F': return TD_CMD_FREQ_KEYS;
        case 'S': return TD_CMD_SHOW_ALL_SHEETS;
        case 'C': return TD_CMD_SHOW_ALL_COLS;
        case '.': return TD_CMD_PLOT_ALL;
        case 'z':
            in->prefix = 3;
            return TD_CMD_NONE;
        default:  return TD_CMD_NONE;
        }
    }

    if (in->prefix == 2) {
        in->prefix = 0;
        if (key.kind != TD_KEY_CHAR)
            return TD_CMD_NONE;
        switch (key.ch) {
        case '_': return TD_CMD_SET_COL_WIDTH;
        case '~': return TD_CMD_TYPE_ANY;
        case 'a': return TD_CMD_ADD_COL;
        case 'd': return TD_CMD_CLEAR_CELL;
        case 'y': return TD_CMD_YANK_CELL;
        case 'p': return TD_CMD_PASTE_CELL;
        case '"': return TD_CMD_DUP_COL;
        case '+': return TD_CMD_AGGREGATOR_ONE;
        default:  return TD_CMD_NONE;
        }
    }

    in->prefix = 0;
    if (key.kind != TD_KEY_CHAR)
        return TD_CMD_NONE;
    switch (key.ch) {
    case 'y': return TD_CMD_YANK_SELECTED_CELLS;
    case 'd': return TD_CMD_CLEAR_SELECTED;
    default:  return TD_CMD_NONE;
    }
}

static bool td_prompt_reserve(td_prompt_t *p, size_t extra)
{
    size_t need = p->len + extra + 1;
    size_t cap;
    char *buf;

    if (need <= p->cap)
        return true;
    cap = p->cap ? p->cap : 32;
    while (cap < need)
        cap *= 2;
    buf = realloc(p->buf, cap);
    if (!buf)
        return false;
    p->buf = buf;
    p->cap = cap;
    return true;
}

static char *td_strdup_heap(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    char *copy = malloc(len + 1);

    if (!copy)
        return NULL;
    if (len)
        memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

static void td_prompt_hist_push(td_prompt_t *p)
{
    td_prompt_hist_t *h;
    const char *text = p->buf ? p->buf : "";
    char *copy;

    if (!text[0] || (size_t)p->kind >= TD_PROMPT_KIND_COUNT)
        return;
    h = &p->hist[p->kind];
    if (h->len > 0 && strcmp(h->items[h->len - 1], text) == 0)
        return;
    copy = td_strdup_heap(text);
    if (!copy)
        return;
    if (h->len == TD_PROMPT_HIST_MAX) {
        free(h->items[0]);
        memmove(&h->items[0], &h->items[1],
                (TD_PROMPT_HIST_MAX - 1) * sizeof(h->items[0]));
        h->len--;
    }
    h->items[h->len++] = copy;
}

static bool td_prompt_set_text(td_prompt_t *p, const char *text)
{
    size_t n = text ? strlen(text) : 0;

    if (!td_prompt_reserve(p, n))
        return false;
    if (n)
        memcpy(p->buf, text, n);
    p->buf[n] = '\0';
    p->len = n;
    p->cursor = n;
    return true;
}

void td_prompt_init(td_prompt_t *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
}

void td_prompt_free(td_prompt_t *p)
{
    if (!p)
        return;
    free(p->label);
    free(p->buf);
    free(p->draft);
    for (size_t k = 0; k < TD_PROMPT_KIND_COUNT; k++) {
        for (size_t i = 0; i < p->hist[k].len; i++)
            free(p->hist[k].items[i]);
    }
    memset(p, 0, sizeof(*p));
}

void td_prompt_start_kind(td_prompt_t *p, td_prompt_kind_t kind,
                          const char *label, const char *initial)
{
    size_t n;

    if (!p)
        return;
    free(p->label);
    p->label = NULL;
    if (label)
        p->label = td_strdup_heap(label);
    free(p->draft);
    p->draft = NULL;
    p->kind = (size_t)kind < TD_PROMPT_KIND_COUNT ? kind : TD_PROMPT_NONE;
    p->hist_pos = p->hist[p->kind].len;
    p->len = 0;
    p->cursor = 0;
    if (p->buf)
        p->buf[0] = '\0';
    if (initial) {
        n = strlen(initial);
        if (td_prompt_reserve(p, n)) {
            memcpy(p->buf, initial, n + 1);
            p->len = n;
            p->cursor = n;
        }
    }
    p->active = true;
}

void td_prompt_start(td_prompt_t *p, const char *label, const char *initial)
{
    td_prompt_start_kind(p, TD_PROMPT_NONE, label, initial);
}

void td_prompt_stop(td_prompt_t *p)
{
    if (p) {
        p->active = false;
        p->kind = TD_PROMPT_NONE;
        free(p->draft);
        p->draft = NULL;
    }
}

td_prompt_kind_t td_prompt_kind(const td_prompt_t *p)
{
    return p ? p->kind : TD_PROMPT_NONE;
}

const char *td_prompt_label(const td_prompt_t *p)
{
    return (p && p->label) ? p->label : "";
}

const char *td_prompt_text(const td_prompt_t *p)
{
    return (p && p->buf) ? p->buf : "";
}

size_t td_prompt_cursor(const td_prompt_t *p)
{
    return p ? p->cursor : 0;
}

static size_t td_prev_boundary(const char *s, size_t pos)
{
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static size_t td_next_boundary(const char *s, size_t len, size_t pos)
{
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

static bool td_prompt_insert(td_prompt_t *p, uint32_t cp)
{
    char enc[4];
    int n = td_utf8_encode(cp, enc);

    if (!td_prompt_reserve(p, (size_t)n))
        return false;
    memmove(p->buf + p->cursor + (size_t)n, p->buf + p->cursor,
            p->len - p->cursor + 1);
    memcpy(p->buf + p->cursor, enc, (size_t)n);
    p->len += (size_t)n;
    p->cursor += (size_t)n;
    return true;
}

static void td_prompt_delete_range(td_prompt_t *p, size_t begin, size_t end)
{
    if (begin >= end || end > p->len)
        return;
    memmove(p->buf + begin, p->buf + end, p->len - end + 1);
    p->len -= end - begin;
    p->cursor = begin;
}

td_prompt_result_t td_prompt_key(td_prompt_t *p, td_key_t key)
{
    if (!p || !p->active)
        return TD_PROMPT_IGNORE;

    if (key.kind == TD_KEY_ENTER) {
        td_prompt_hist_push(p);
        return TD_PROMPT_ACCEPT;
    }
    if (key.kind == TD_KEY_ESC ||
        (key.kind == TD_KEY_CHAR && key.ch == 3))
        return TD_PROMPT_CANCEL;

    if (key.kind == TD_KEY_UP ||
        (key.kind == TD_KEY_CHAR && key.ch == 16)) { /* Ctrl+P */
        td_prompt_hist_t *h = &p->hist[p->kind];

        if (h->len == 0 || p->hist_pos == 0)
            return TD_PROMPT_IGNORE;
        if (p->hist_pos == h->len) {
            free(p->draft);
            p->draft = td_strdup_heap(p->buf ? p->buf : "");
        }
        p->hist_pos--;
        (void)td_prompt_set_text(p, h->items[p->hist_pos]);
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_DOWN ||
        (key.kind == TD_KEY_CHAR && key.ch == 14)) { /* Ctrl+N */
        td_prompt_hist_t *h = &p->hist[p->kind];

        if (p->hist_pos >= h->len)
            return TD_PROMPT_IGNORE;
        p->hist_pos++;
        if (p->hist_pos == h->len) {
            (void)td_prompt_set_text(p, p->draft ? p->draft : "");
            free(p->draft);
            p->draft = NULL;
        } else {
            (void)td_prompt_set_text(p, h->items[p->hist_pos]);
        }
        return TD_PROMPT_CHANGED;
    }

    if (key.kind == TD_KEY_LEFT ||
        (key.kind == TD_KEY_CHAR && key.ch == 1)) {
        if (key.kind == TD_KEY_LEFT)
            p->cursor = td_prev_boundary(p->buf, p->cursor);
        else
            p->cursor = 0;
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_RIGHT ||
        (key.kind == TD_KEY_CHAR && key.ch == 5)) {
        if (key.kind == TD_KEY_RIGHT)
            p->cursor = td_next_boundary(p->buf, p->len, p->cursor);
        else
            p->cursor = p->len;
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_HOME) {
        p->cursor = 0;
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_END) {
        p->cursor = p->len;
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_BACKSPACE) {
        size_t begin = td_prev_boundary(p->buf, p->cursor);

        if (begin != p->cursor)
            td_prompt_delete_range(p, begin, p->cursor);
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_DELETE) {
        size_t end = td_next_boundary(p->buf, p->len, p->cursor);

        if (end != p->cursor)
            td_prompt_delete_range(p, p->cursor, end);
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_CHAR && key.ch == 21) { /* Ctrl+U */
        td_prompt_delete_range(p, 0, p->len);
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_CHAR && key.ch == 11) { /* Ctrl+K */
        td_prompt_delete_range(p, p->cursor, p->len);
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_CHAR && key.ch == 23) { /* Ctrl+W */
        size_t begin = p->cursor;

        while (begin > 0 && p->buf[begin - 1] == ' ')
            begin--;
        while (begin > 0 && p->buf[begin - 1] != ' ')
            begin = td_prev_boundary(p->buf, begin);
        if (begin != p->cursor)
            td_prompt_delete_range(p, begin, p->cursor);
        return TD_PROMPT_CHANGED;
    }
    if (key.kind == TD_KEY_CHAR && key.ch >= 32 && key.ch != 127) {
        (void)td_prompt_insert(p, key.ch);
        return TD_PROMPT_CHANGED;
    }
    return TD_PROMPT_IGNORE;
}

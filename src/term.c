#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "term.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "utf8.h"

static td_term_t *g_active;
static volatile sig_atomic_t g_resize_pending;
static volatile sig_atomic_t g_quit_pending;
static volatile sig_atomic_t g_suspend_pending;
static volatile sig_atomic_t g_continue_pending;

static int g_restore_fd = -1;
static struct termios g_restore_termios;
static volatile sig_atomic_t g_restore_active;
static volatile sig_atomic_t g_restore_alternate;

static void td_write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
}

/* Async-signal-safe: only tcsetattr, write and _exit. */
static void td_emergency_restore(void)
{
    static const char reset[] = "\x1b[0m\x1b[?25h";
    static const char leave_alt[] = "\x1b[?1049l";

    if (!g_restore_active)
        return;
    if (g_restore_fd >= 0)
        (void)tcsetattr(g_restore_fd, TCSAFLUSH, &g_restore_termios);
    if (g_restore_fd >= 0) {
        td_write_all(g_restore_fd, reset, sizeof(reset) - 1);
        if (g_restore_alternate)
            td_write_all(g_restore_fd, leave_alt, sizeof(leave_alt) - 1);
    }
    g_restore_active = 0;
}

static void td_signal_handler(int sig)
{
    switch (sig) {
    case SIGWINCH:
        g_resize_pending = 1;
        break;
    case SIGINT:
    case SIGTERM:
    case SIGHUP:
        g_quit_pending = 1;
        td_emergency_restore();
        break;
    case SIGTSTP:
        g_suspend_pending = 1;
        break;
    case SIGCONT:
        g_continue_pending = 1;
        break;
    default:
        td_emergency_restore();
        _exit(128 + sig);
    }
}

static void td_install_signals(void)
{
    static const int signals[] = {
        SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGTSTP, SIGCONT,
        SIGSEGV, SIGABRT, SIGBUS, SIGFPE
    };
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = td_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* let poll()/read() return EINTR */
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        (void)sigaction(signals[i], &sa, NULL);
}

static void td_restore_signals(void)
{
    static const int signals[] = {
        SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGTSTP, SIGCONT,
        SIGSEGV, SIGABRT, SIGBUS, SIGFPE
    };

    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        (void)signal(signals[i], SIG_DFL);
}

static bool td_has_utf8(const char *locale)
{
    char buf[64];
    size_t n = 0;

    if (!locale)
        return false;
    while (*locale && n + 1 < sizeof(buf)) {
        buf[n++] = (char)tolower((unsigned char)*locale++);
    }
    buf[n] = '\0';
    return strstr(buf, "utf-8") != NULL || strstr(buf, "utf8") != NULL;
}

static const char *td_env_locale(void)
{
    const char *s = getenv("LC_ALL");

    if (!s || !*s)
        s = getenv("LC_CTYPE");
    if (!s || !*s)
        s = getenv("LANG");
    return s;
}

static bool td_starts_with(const char *s, const char *prefix)
{
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

void td_term_profile_detect(td_term_profile_t *p, const char *term,
                            const char *locale)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->name = term ? term : "";
    p->utf8 = td_has_utf8(locale);

    if (td_starts_with(term, "linux")) {
        p->alt_screen = false;
        p->sync_output = false;
        p->color256 = false;
        p->colors = 16;
        return;
    }
    if (td_starts_with(term, "dumb")) {
        p->alt_screen = false;
        p->sync_output = false;
        p->color256 = false;
        p->colors = 8;
        p->utf8 = false;
        return;
    }

    p->alt_screen = true;
    p->sync_output = true;
    p->color256 = true;
    p->colors = 256;
}

void td_term_glyphs(td_glyphs_t *g, bool utf8)
{
    if (!g)
        return;
    if (utf8) {
        g->vline = "\u2502";
        g->hline = "\u2500";
        g->cross = "\u253c";
        g->ellipsis = "\u2026";
        g->dot = "\u2022";
        g->keysep = "\u2551";
    } else {
        g->vline = "|";
        g->hline = "-";
        g->cross = "+";
        g->ellipsis = "~";
        g->dot = "*";
        g->keysep = "|";
    }
}

static td_cell_t td_blank_cell(void)
{
    td_cell_t c;

    c.ch = ' ';
    c.fg = TD_COLOR_DEFAULT;
    c.bg = TD_COLOR_DEFAULT;
    c.attr = TD_ATTR_NONE;
    return c;
}

static td_cell_t td_unknown_cell(void)
{
    td_cell_t c = td_blank_cell();

    c.ch = 0;
    return c;
}

static bool td_alloc_buffers(td_term_t *t, int rows, int cols)
{
    size_t count;
    td_cell_t *front;
    td_cell_t *back;

    if (rows < 1)
        rows = 1;
    if (cols < 1)
        cols = 1;
    count = (size_t)rows * (size_t)cols;
    if (count / (size_t)rows != (size_t)cols)
        return false;

    front = malloc(count * sizeof(*front));
    back = malloc(count * sizeof(*back));
    if (!front || !back) {
        free(front);
        free(back);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        front[i] = td_unknown_cell();
        back[i] = td_blank_cell();
    }

    free(t->front);
    free(t->back);
    t->front = front;
    t->back = back;
    t->rows = rows;
    t->cols = cols;
    t->force_full = true;
    return true;
}

int td_term_init(td_term_t *t, int rows, int cols, const char *term,
                 const char *locale)
{
    if (!t)
        return -1;
    memset(t, 0, sizeof(*t));
    t->fd_in = -1;
    t->fd_out = -1;
    td_term_profile_detect(&t->profile, term, locale);
    td_term_glyphs(&t->glyphs, t->profile.utf8);
    t->frame = tam_str_new("");
    if (!t->frame)
        return -1;
    if (!td_alloc_buffers(t, rows, cols)) {
        tam_str_free(t->frame);
        t->frame = NULL;
        return -1;
    }
    return 0;
}

static int td_tty_size(int fd, int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
    *rows = 24;
    *cols = 80;
    return -1;
}

int td_term_open(td_term_t *t, const char *term, const char *locale)
{
    int fd;
    int rows, cols;
    bool own_fd = false;

    if (!t)
        return -1;

    if (!locale)
        locale = td_env_locale();
    if (!term)
        term = getenv("TERM");

    fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        own_fd = true;
    } else {
        fd = STDIN_FILENO;
    }
    if (!isatty(fd)) {
        if (own_fd)
            close(fd);
        return -1;
    }

    if (td_term_init(t, 24, 80, term, locale) != 0) {
        if (own_fd)
            close(fd);
        return -1;
    }
    t->fd_in = fd;
    t->fd_out = own_fd ? fd : STDOUT_FILENO;

    if (tcgetattr(fd, &t->saved) != 0) {
        if (own_fd)
            close(fd);
        td_term_close(t);
        return -1;
    }
    t->raw = t->saved;
    cfmakeraw(&t->raw);
    t->raw.c_cc[VMIN] = 1;
    t->raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSAFLUSH, &t->raw) != 0) {
        if (own_fd)
            close(fd);
        td_term_close(t);
        return -1;
    }
    t->raw_active = true;

    (void)td_tty_size(fd, &rows, &cols);
    if (!td_alloc_buffers(t, rows, cols)) {
        (void)tcsetattr(fd, TCSAFLUSH, &t->saved);
        t->raw_active = false;
        if (own_fd)
            close(fd);
        td_term_close(t);
        return -1;
    }

    t->opened = true;
    t->alternate = t->profile.alt_screen;
    g_restore_fd = fd;
    g_restore_termios = t->saved;
    g_restore_alternate = t->alternate ? 1 : 0;
    g_restore_active = 1;
    g_active = t;
    g_resize_pending = 0;
    g_quit_pending = 0;
    g_suspend_pending = 0;
    g_continue_pending = 0;
    td_install_signals();

    if (t->alternate)
        td_write_all(t->fd_out, "\x1b[?1049h", 8);
    else
        td_write_all(t->fd_out, "\x1b[2J\x1b[H", 7);
    td_write_all(t->fd_out, "\x1b[?25l", 6);
    return 0;
}

void td_term_close(td_term_t *t)
{
    if (!t)
        return;

    if (t->opened) {
        static const char reset[] = "\x1b[0m\x1b[?25h";

        td_write_all(t->fd_out, reset, sizeof(reset) - 1);
        if (t->alternate)
            td_write_all(t->fd_out, "\x1b[?1049l", 8);
        else
            td_write_all(t->fd_out, "\x1b[2J\x1b[H", 7);
    }
    if (t->raw_active && t->fd_in >= 0)
        (void)tcsetattr(t->fd_in, TCSAFLUSH, &t->saved);
    t->raw_active = false;

    if (g_active == t) {
        g_active = NULL;
        g_restore_active = 0;
        td_restore_signals();
    }

    if (t->opened && t->fd_in >= 0 && t->fd_in != STDIN_FILENO &&
        t->fd_in != STDOUT_FILENO)
        close(t->fd_in);
    t->fd_in = -1;
    t->fd_out = -1;
    t->opened = false;

    free(t->front);
    free(t->back);
    t->front = NULL;
    t->back = NULL;
    if (t->frame) {
        tam_str_free(t->frame);
        t->frame = NULL;
    }
}

int td_term_resize(td_term_t *t, int rows, int cols)
{
    if (!t)
        return -1;
    return td_alloc_buffers(t, rows, cols) ? 0 : -1;
}

static void td_term_enter_screen(td_term_t *t)
{
    if (t->alternate)
        td_write_all(t->fd_out, "\x1b[?1049h", 8);
    else
        td_write_all(t->fd_out, "\x1b[2J\x1b[H", 7);
    td_write_all(t->fd_out, "\x1b[?25l", 6);
    t->force_full = true;
}

void td_term_suspend(td_term_t *t)
{
    static const char reset[] = "\x1b[0m\x1b[?25h";
    static const char leave_alt[] = "\x1b[?1049l";

    if (!t || !t->opened)
        return;
    td_write_all(t->fd_out, reset, sizeof(reset) - 1);
    if (t->alternate)
        td_write_all(t->fd_out, leave_alt, sizeof(leave_alt) - 1);
    if (t->raw_active)
        (void)tcsetattr(t->fd_in, TCSAFLUSH, &t->saved);
    t->raw_active = false;
    g_restore_active = 0;
}

void td_term_resume(td_term_t *t)
{
    if (!t || !t->opened)
        return;
    (void)tcsetattr(t->fd_in, TCSAFLUSH, &t->raw);
    t->raw_active = true;
    g_restore_fd = t->fd_in;
    g_restore_termios = t->saved;
    g_restore_alternate = t->alternate ? 1 : 0;
    g_restore_active = 1;
    td_term_enter_screen(t);
}

void td_term_clear_back(td_term_t *t)
{
    td_cell_t blank;

    if (!t || !t->back)
        return;
    blank = td_blank_cell();
    for (size_t i = 0; i < (size_t)t->rows * (size_t)t->cols; i++)
        t->back[i] = blank;
}

static uint32_t td_display_ch(uint32_t ch)
{
    if (ch < 32 || ch == 127)
        return ' ';
    return ch ? ch : ' ';
}

void td_term_put_cell(td_term_t *t, int row, int col, uint32_t ch, uint8_t fg,
                      uint8_t bg, uint8_t attr)
{
    td_cell_t *c;

    if (!t || !t->back || row < 0 || col < 0 || row >= t->rows ||
        col >= t->cols)
        return;
    c = &t->back[(size_t)row * (size_t)t->cols + (size_t)col];
    c->ch = td_display_ch(ch);
    c->fg = fg;
    c->bg = bg;
    c->attr = attr;
}

static uint32_t td_glyph_codepoint(const char *s)
{
    const char *p = s;
    const char *end;

    if (!s || !*s)
        return '?';
    end = s + strlen(s);
    return td_utf8_decode(&p, end);
}

int td_term_put_str(td_term_t *t, int row, int col, int maxw, const char *s,
                    uint8_t fg, uint8_t bg, uint8_t attr)
{
    const char *p;
    const char *end;
    size_t total;
    int limit;
    int written = 0;

    if (!t || !s || row < 0 || row >= t->rows)
        return 0;
    if (col < 0) {
        maxw += col;
        col = 0;
    }
    if (col >= t->cols || maxw <= 0)
        return 0;
    if (maxw > t->cols - col)
        maxw = t->cols - col;

    total = td_utf8_width(s);
    p = s;
    end = s + strlen(s);
    limit = total > (size_t)maxw ? maxw - 1 : maxw;

    while (p < end && written < limit) {
        uint32_t cp = td_utf8_decode(&p, end);

        td_term_put_cell(t, row, col + written, cp, fg, bg, attr);
        written++;
    }
    if (total > (size_t)maxw && maxw > 0) {
        td_term_put_cell(t, row, col + written,
                         td_glyph_codepoint(t->glyphs.ellipsis), fg, bg,
                         attr);
        written++;
    }
    return written;
}

void td_term_fill_rect(td_term_t *t, int row, int col, int height, int width,
                       uint32_t ch, uint8_t fg, uint8_t bg, uint8_t attr)
{
    for (int r = 0; r < height; r++) {
        for (int c = 0; c < width; c++)
            td_term_put_cell(t, row + r, col + c, ch, fg, bg, attr);
    }
}

static int td_map_color(const td_term_profile_t *p, uint8_t color)
{
    if (color == TD_COLOR_DEFAULT)
        return -1;
    if (p->colors <= 16 && color >= 16)
        return 7;
    return color;
}

static void td_append_sgr(tam_str_t *frame, const td_term_profile_t *p,
                          uint8_t fg, uint8_t bg, uint8_t attr)
{
    char params[64];
    size_t n = 0;
    int color;

    params[n++] = '0';

#define TD_SGR_APPEND(...)                                                    \
    do {                                                                      \
        int _w = snprintf(params + n, sizeof(params) - n, __VA_ARGS__);       \
        if (_w > 0 && (size_t)_w < sizeof(params) - n)                        \
            n += (size_t)_w;                                                  \
    } while (0)

    if (attr & TD_ATTR_BOLD)
        TD_SGR_APPEND(";1");
    if (attr & TD_ATTR_DIM)
        TD_SGR_APPEND(";2");
    if (attr & TD_ATTR_UNDERLINE)
        TD_SGR_APPEND(";4");
    if (attr & TD_ATTR_REV)
        TD_SGR_APPEND(";7");

    color = td_map_color(p, fg);
    if (color < 0)
        TD_SGR_APPEND(";39");
    else if (color < 8)
        TD_SGR_APPEND(";%d", 30 + color);
    else if (color < 16)
        TD_SGR_APPEND(";%d", 90 + color - 8);
    else
        TD_SGR_APPEND(";38;5;%d", color);

    color = td_map_color(p, bg);
    if (color < 0)
        TD_SGR_APPEND(";49");
    else if (color < 8)
        TD_SGR_APPEND(";%d", 40 + color);
    else if (color < 16)
        TD_SGR_APPEND(";%d", 100 + color - 8);
    else
        TD_SGR_APPEND(";48;5;%d", color);

#undef TD_SGR_APPEND

    *frame = tam_str_cat_printf(*frame, "\x1b[%sm", params);
}

static bool td_cell_equal(const td_cell_t *a, const td_cell_t *b)
{
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg &&
           a->attr == b->attr;
}

void td_term_present(td_term_t *t)
{
    int cursor_row = -1;
    int cursor_col = -1;
    int last_fg = -2;
    int last_bg = -2;
    int last_attr = -2;
    bool changed = false;
    tam_str_t frame;

    if (!t || !t->back || !t->front || !t->frame)
        return;

    tam_str_clear(t->frame);
    frame = t->frame;
    if (t->opened && t->profile.sync_output)
        frame = tam_str_cat(frame, "\x1b[?2026h");

    for (int r = 0; r < t->rows; r++) {
        for (int c = 0; c < t->cols; c++) {
            size_t idx = (size_t)r * (size_t)t->cols + (size_t)c;
            td_cell_t *back = &t->back[idx];
            td_cell_t *front = &t->front[idx];
            char enc[4];
            int n;

            if (!t->force_full && td_cell_equal(back, front))
                continue;
            changed = true;

            if (r != cursor_row || c != cursor_col)
                frame = tam_str_cat_printf(frame, "\x1b[%d;%dH", r + 1,
                                           c + 1);

            if (last_fg != back->fg || last_bg != back->bg ||
                last_attr != back->attr) {
                td_append_sgr(&frame, &t->profile, back->fg, back->bg,
                              back->attr);
                last_fg = back->fg;
                last_bg = back->bg;
                last_attr = back->attr;
            }

            n = td_utf8_encode(td_display_ch(back->ch), enc);
            frame = tam_str_cat_len(frame, enc, (size_t)n);
            cursor_row = r;
            cursor_col = c + 1;
        }
    }

    if (t->opened && t->profile.sync_output)
        frame = tam_str_cat(frame, "\x1b[?2026l");
    t->frame = frame;

    if (t->opened && changed)
        td_write_all(t->fd_out, t->frame, tam_str_len(t->frame));

    memcpy(t->front, t->back,
           (size_t)t->rows * (size_t)t->cols * sizeof(*t->front));
    t->force_full = false;
}

td_cell_t td_term_front_cell(const td_term_t *t, int row, int col)
{
    td_cell_t blank = td_blank_cell();

    if (!t || !t->front || row < 0 || col < 0 || row >= t->rows ||
        col >= t->cols)
        return blank;
    return t->front[(size_t)row * (size_t)t->cols + (size_t)col];
}

td_cell_t td_term_back_cell(const td_term_t *t, int row, int col)
{
    td_cell_t blank = td_blank_cell();

    if (!t || !t->back || row < 0 || col < 0 || row >= t->rows ||
        col >= t->cols)
        return blank;
    return t->back[(size_t)row * (size_t)t->cols + (size_t)col];
}

const char *td_term_frame(const td_term_t *t)
{
    return (t && t->frame) ? t->frame : "";
}

size_t td_term_frame_len(const td_term_t *t)
{
    return (t && t->frame) ? tam_str_len(t->frame) : 0;
}

td_term_event_t td_term_next_event(td_term_t *t)
{
    if (!t)
        return TD_TERM_EVENT_NONE;
    if (g_quit_pending) {
        g_quit_pending = 0;
        return TD_TERM_EVENT_QUIT;
    }
    if (g_resize_pending) {
        int rows, cols;

        g_resize_pending = 0;
        if (t->opened && t->fd_in >= 0) {
            (void)td_tty_size(t->fd_in, &rows, &cols);
            (void)td_term_resize(t, rows, cols);
        }
        return TD_TERM_EVENT_RESIZE;
    }
    if (g_suspend_pending) {
        g_suspend_pending = 0;
        td_term_suspend(t);
        return TD_TERM_EVENT_SUSPEND;
    }
    if (g_continue_pending) {
        g_continue_pending = 0;
        td_term_resume(t);
        return TD_TERM_EVENT_CONTINUE;
    }
    return TD_TERM_EVENT_NONE;
}

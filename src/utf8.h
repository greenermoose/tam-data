#ifndef TD_UTF8_H
#define TD_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decode one UTF-8 codepoint from [*sp, end). Invalid or truncated input
 * advances by one byte and yields U+FFFD, so callers always make progress. */
static inline uint32_t td_utf8_decode(const char **sp, const char *end)
{
    const unsigned char *s = (const unsigned char *)*sp;
    const unsigned char *e = (const unsigned char *)end;
    uint32_t cp;
    int extra;

    if (s >= e)
        return 0;

    if (s[0] < 0x80) {
        *sp += 1;
        return s[0];
    }

    if ((s[0] & 0xE0) == 0xC0) {
        cp = s[0] & 0x1Fu;
        extra = 1;
        if (cp < 2)
            goto bad;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = s[0] & 0x0Fu;
        extra = 2;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = s[0] & 0x07u;
        extra = 3;
        if (cp > 4)
            goto bad;
    } else {
        goto bad;
    }

    if ((size_t)(e - s) < (size_t)extra + 1)
        goto bad;

    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80)
            goto bad;
        cp = (cp << 6) | (uint32_t)(s[i] & 0x3Fu);
    }

    if ((extra == 1 && cp < 0x80) ||
        (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) ||
        cp > 0x10FFFF)
        goto bad;

    *sp += extra + 1;
    return cp;

bad:
    *sp += 1;
    return 0xFFFD;
}

/* Encode one codepoint. Returns the number of bytes (1..4), or 1 with '?'
 * for values that cannot be represented. */
static inline int td_utf8_encode(uint32_t cp, char out[4])
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = '?';

    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Number of codepoints in a NUL-terminated string. */
static inline size_t td_utf8_width(const char *s)
{
    const char *p = s;
    const char *end = s + (s ? __builtin_strlen(s) : 0);
    size_t n = 0;

    while (p < end) {
        (void)td_utf8_decode(&p, end);
        n++;
    }
    return n;
}

#endif /* TD_UTF8_H */

#ifndef UTIL_H
#define UTIL_H

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

/* 1-31 chars of letters, digits, '_' or '-'; first must be a letter or digit */
static inline bool util_valid_name(const char *s)
{
    size_t len = strlen(s);
    if (len < 1 || len > 31 || !isalnum((unsigned char)s[0])) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

/* Canonical floor: "1".."999" or "B1".."B99", no leading zeros.
 * *val is the orderable value (basements negative, there is no 0). */
static inline bool util_floor(const char *s, int *val)
{
    bool b = (s[0] == 'B');
    const char *d = b ? s + 1 : s;
    size_t len = strlen(d);
    if (len < 1 || len > (b ? 2u : 3u) || d[0] == '0') return false;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)d[i])) return false;
        n = n * 10 + (d[i] - '0');
    }
    *val = b ? -n : n;
    return true;
}

#endif

/*
 * kernel/string.c — Freestanding memory utility implementations.
 *
 * These are intentionally simple and correct rather than fast.  The
 * compiler is permitted to emit calls to these functions for aggregate
 * copies and struct initialisation, so they must not themselves be
 * compiled to a call (they won't be, since they are defined here).
 */

#include "string.h"

void *memset(void *dest, int c, size_t n)
{
    unsigned char *p = dest;
    while (n--)
        *p++ = (unsigned char)c;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char       *d = dest;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char       *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++)) {}
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

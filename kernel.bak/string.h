/*
 * kernel/string.h — Freestanding string and memory utilities.
 *
 * Provides the standard memory functions that the compiler may emit
 * implicit calls to (memset, memcpy) as well as memmove and memcmp.
 */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void  *memset (void *dest, int c, size_t n);
void  *memcpy (void *dest, const void *src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp (const void *a, const void *b, size_t n);

size_t strlen (const char *s);
int    strcmp (const char *a, const char *b);
char  *strcpy (char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);

#endif /* STRING_H */

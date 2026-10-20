/* SPDX-License-Identifier: GPL-2.0-only */
/* Freestanding <string.h> for the i1n1.efi build only (no host libc). */
#ifndef I1N1_FS_STRING_H
#define I1N1_FS_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* Freestanding <stdio.h>: only the bounded formatters are provided. */
#ifndef I1N1_FS_STDIO_H
#define I1N1_FS_STDIO_H
#include <stdarg.h>
#include <stddef.h>
int snprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));
#endif

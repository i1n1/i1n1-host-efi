/* SPDX-License-Identifier: GPL-2.0-only */
/* Freestanding <stdlib.h>: bump allocator over a firmware-provided pool. */
#ifndef I1N1_FS_STDLIB_H
#define I1N1_FS_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void free(void *p);
/* Not libc: hands the allocator its pool. Must precede the first malloc. */
void i1n1_fs_heap_init(void *base, size_t bytes);
size_t i1n1_fs_heap_used(void);
#endif

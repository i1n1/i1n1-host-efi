/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal freestanding libc for the i1n1.efi build (never linked into the
 * host unit tests). Only what the product modules actually call:
 * the mem and str families, malloc/calloc/free over one firmware pool, and a
 * bounded snprintf/vsnprintf (d i u x X c s p %, flags 0 -, width,
 * .precision for s, length hh h l ll z).
 *
 * The allocator is a bump allocator: free() is a no-op. That is deliberate
 * and sufficient because the DBT cache allocates once at init; nothing in the
 * EFI image allocates in a loop. Exhaustion returns NULL (callers already
 * treat that as a deterministic failure).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "freestanding/stdio.h"
#include "freestanding/stdlib.h"
#include "freestanding/string.h"

/* clang for COFF x86_64 emits __chkstk for frames larger than a page. The UEFI
 * stack is flat firmware memory without guard pages, so there is nothing to
 * probe: returning leaves RAX/RSP untouched, which is the contract. */
__asm__(".text\n.globl __chkstk\n__chkstk:\n\tret\n");

void *memcpy(void *dst, const void *src, size_t n) {
  void *ret = dst;
  __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
  return ret;
}

void *memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  if (d == s || n == 0) return dst;
  if (d < s || d >= s + n) return memcpy(dst, src, n);
  /* overlapping with dst above src: copy backwards from the last byte */
  {
    uint8_t *dl = d + n - 1;
    const uint8_t *sl = s + n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld"
                     : "+D"(dl), "+S"(sl), "+c"(n)
                     :
                     : "memory");
  }
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  void *ret = dst;
  __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
  return ret;
}

int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a;
  const uint8_t *y = (const uint8_t *)b;
  size_t i;
  for (i = 0; i < n; ++i)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

size_t strnlen(const char *s, size_t n) {
  size_t i = 0;
  while (i < n && s[i]) ++i;
  return i;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  size_t i;
  for (i = 0; i < n; ++i) {
    if (a[i] != b[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
    if (a[i] == 0) return 0;
  }
  return 0;
}

/* ---- allocator --------------------------------------------------------- */

static uint8_t *g_heap;
static size_t g_heap_size;
static size_t g_heap_used;

void i1n1_fs_heap_init(void *base, size_t bytes) {
  g_heap = (uint8_t *)base;
  g_heap_size = bytes;
  g_heap_used = 0;
}

size_t i1n1_fs_heap_used(void) { return g_heap_used; }

void *malloc(size_t n) {
  size_t aligned;
  void *p;
  if (n == 0) n = 1;
  if (n > (size_t)-1 - 63u) return NULL;
  aligned = (n + 63u) & ~(size_t)63u;
  if (!g_heap || aligned > g_heap_size - g_heap_used) return NULL;
  p = g_heap + g_heap_used;
  g_heap_used += aligned;
  return p;
}

void *calloc(size_t count, size_t size) {
  void *p;
  if (size != 0 && count > (size_t)-1 / size) return NULL;
  p = malloc(count * size);
  if (p) memset(p, 0, count * size);
  return p;
}

void free(void *p) { (void)p; }

/* ---- formatter --------------------------------------------------------- */

struct out {
  char *buf;
  size_t cap;
  size_t len; /* characters that WOULD have been written */
};

static void put_ch(struct out *o, char c) {
  if (o->cap && o->len + 1 < o->cap) o->buf[o->len] = c;
  o->len++;
}

static void put_num(struct out *o, uint64_t v, unsigned base, int upper,
                    int neg, int zero, int left, int width) {
  char tmp[24];
  unsigned n = 0, i;
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  int total;
  if (v == 0) tmp[n++] = '0';
  while (v) {
    tmp[n++] = digits[v % base];
    v /= base;
  }
  total = (int)n + (neg ? 1 : 0);
  if (!left && !zero)
    for (; total < width; ++total) put_ch(o, ' ');
  if (neg) put_ch(o, '-');
  if (!left && zero)
    for (; total < width; ++total) put_ch(o, '0');
  for (i = n; i > 0; --i) put_ch(o, tmp[i - 1]);
  if (left)
    for (; total < width; ++total) put_ch(o, ' ');
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
  struct out o;
  o.buf = buf;
  o.cap = cap;
  o.len = 0;
  for (; *fmt; ++fmt) {
    int zero = 0, left = 0, width = 0, prec = -1, lng = 0;
    char c;
    if (*fmt != '%') {
      put_ch(&o, *fmt);
      continue;
    }
    ++fmt;
    for (;; ++fmt) {
      if (*fmt == '0') zero = 1;
      else if (*fmt == '-') left = 1;
      else break;
    }
    while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') {
      ++fmt;
      prec = 0;
      while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
    }
    for (;; ++fmt) {
      if (*fmt == 'l') lng++;
      else if (*fmt == 'z') lng = 2;
      else if (*fmt == 'h') continue; /* promoted to int by varargs */
      else break;
    }
    c = *fmt;
    if (c == 0) break;
    switch (c) {
      case '%': put_ch(&o, '%'); break;
      case 'c': put_ch(&o, (char)va_arg(ap, int)); break;
      case 's': {
        const char *s = va_arg(ap, const char *);
        size_t n = 0;
        int pad;
        if (!s) s = "(null)";
        while (s[n] && (prec < 0 || (int)n < prec)) ++n;
        pad = width - (int)n;
        if (!left) for (; pad > 0; --pad) put_ch(&o, ' ');
        for (size_t i = 0; i < n; ++i) put_ch(&o, s[i]);
        if (left) for (; pad > 0; --pad) put_ch(&o, ' ');
        break;
      }
      case 'd':
      case 'i': {
        int64_t v;
        int neg;
        uint64_t mag;
        if (lng >= 2) v = va_arg(ap, long long);
        else if (lng == 1) v = (int64_t)va_arg(ap, long); /* LLP64: 32-bit */
        else v = va_arg(ap, int);
        neg = v < 0;
        mag = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
        put_num(&o, mag, 10, 0, neg, zero, left, width);
        break;
      }
      case 'u':
      case 'x':
      case 'X': {
        uint64_t v;
        if (lng >= 2) v = va_arg(ap, unsigned long long);
        else if (lng == 1) v = va_arg(ap, unsigned long);
        else v = va_arg(ap, unsigned int);
        put_num(&o, v, c == 'u' ? 10u : 16u, c == 'X', 0, zero, left, width);
        break;
      }
      case 'p': {
        uintptr_t v = (uintptr_t)va_arg(ap, void *);
        put_ch(&o, '0');
        put_ch(&o, 'x');
        put_num(&o, (uint64_t)v, 16, 0, 0, 0, 0, 0);
        break;
      }
      default:
        /* Unknown conversion: print it verbatim so it is visible, never
         * silently dropped. */
        put_ch(&o, '%');
        put_ch(&o, c);
        break;
    }
  }
  if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
  return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return n;
}

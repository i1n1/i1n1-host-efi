/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Debug link loop for the EFI image: serves
 * link ABI1 (include/debug_link.h, docs/contracts/debug-link.md) over the COM2
 * byte pipe against the guest RAM window.
 *
 * Framing follows the C5 contract: scan bytes for the 0xAA55FF magic, read the remaining header
 * bytes under a whole-request deadline, verify the checksum (a confirmed
 * header with a wrong checksum is answered ST_CSUMERR echoing the received
 * type, touching nothing), then dispatch. Unsupported types and inner
 * opcodes are answered BADCMD, never emulated.
 *
 * EFI-specific inner-status extension (documented in proxy.md section 12):
 * a scalar access outside the RAM window or misaligned for its width is
 * answered with inner status I1N1_EFI_PROXY_S_FAULT (-14) and counted by
 * P_GET_EXC_COUNT; it is never served as zero.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "debug_link.h"
#include "runner.h"

#define I1N1_EFI_PROXY_S_FAULT (-14)

struct loop {
  const struct i1n1_dlink_io *io;
  uint8_t *host;
  uint64_t base, bytes;
  uint64_t exc_count;
  struct i1n1_dlink_stats *st;
};

static int in_window(const struct loop *l, uint64_t addr, uint64_t len) {
  return len != 0 && addr >= l->base && addr - l->base <= l->bytes &&
         len <= l->bytes - (addr - l->base);
}

static void send_bytes(const struct loop *l, const uint8_t *p, size_t n) {
  size_t i;
  for (i = 0; i < n; ++i) l->io->write_byte(l->io->ctx, p[i]);
}

static void reply(struct loop *l, uint32_t type, int32_t status,
                  const uint8_t payload[I1N1_DLINK_REPLY_PAYLOAD_BYTES],
                  const uint8_t *body, size_t body_len) {
  uint8_t hdr[I1N1_DLINK_REPLY_BYTES];
  if (i1n1_dlink_encode_reply(hdr, type, status, payload) != I1N1_DLINK_OK)
    return;
  send_bytes(l, hdr, sizeof hdr);
  if (body_len) send_bytes(l, body, body_len);
  l->st->replies++;
  if (status == I1N1_DLINK_ST_CSUMERR) l->st->csum_errors++;
  if (status == I1N1_DLINK_ST_XFRERR) l->st->xfer_errors++;
  if (status == I1N1_DLINK_ST_BADCMD) l->st->badcmd++;
}

/* Reads exactly n bytes before `deadline`. Returns 0 or -1 on timeout. */
static int read_exact(struct loop *l, uint8_t *dst, size_t n, uint64_t deadline) {
  size_t got = 0;
  while (got < n) {
    uint8_t b;
    if (l->io->read_byte(l->io->ctx, &b) == 0) {
      dst[got++] = b;
      continue;
    }
    if (l->io->now_ms(l->io->ctx) >= deadline) return -1;
  }
  return 0;
}

static void u64_to(uint8_t *p, uint64_t v) { i1n1_dlink_store_u64le(p, v); }

static void do_proxy(struct loop *l, uint32_t type, const uint8_t payload[56]) {
  uint64_t op = i1n1_dlink_load_u64le(payload);
  uint64_t a0 = i1n1_dlink_load_u64le(payload + 8);
  uint64_t a1 = i1n1_dlink_load_u64le(payload + 16);
  uint8_t out[I1N1_DLINK_REPLY_PAYLOAD_BYTES];
  int64_t status = I1N1_DLINK_S_OK;
  uint64_t ret = 0;
  l->st->proxy_ops++;
  switch (op) {
    case I1N1_DLINK_P_NOP: break;
    case I1N1_DLINK_P_SET_EXC_GUARD:
      if (a0 != I1N1_DLINK_GUARD_RETURN) status = I1N1_DLINK_S_BADCMD;
      break;
    case I1N1_DLINK_P_GET_EXC_COUNT: ret = l->exc_count; break;
    case I1N1_DLINK_P_WRITE64:
    case I1N1_DLINK_P_WRITE32:
    case I1N1_DLINK_P_WRITE16:
    case I1N1_DLINK_P_WRITE8:
    case I1N1_DLINK_P_READ64:
    case I1N1_DLINK_P_READ32:
    case I1N1_DLINK_P_READ16:
    case I1N1_DLINK_P_READ8: {
      int is_read = op >= I1N1_DLINK_P_READ64;
      unsigned w = 8u >> (op & 3u); /* 64,32,16,8 -> 8,4,2,1 bytes */
      if ((a0 & (w - 1u)) || !in_window(l, a0, w)) {
        if (l->exc_count != UINT64_MAX) l->exc_count++;
        status = I1N1_EFI_PROXY_S_FAULT;
      } else if (is_read) {
        uint64_t v = 0;
        unsigned i;
        for (i = 0; i < w; ++i) v |= (uint64_t)l->host[a0 - l->base + i] << (8 * i);
        ret = v;
      } else {
        unsigned i;
        for (i = 0; i < w; ++i) l->host[a0 - l->base + i] = (uint8_t)(a1 >> (8 * i));
      }
      break;
    }
    default: status = I1N1_DLINK_S_BADCMD; break;
  }
  memset(out, 0, sizeof out);
  u64_to(out, op);
  u64_to(out + 8, (uint64_t)status);
  u64_to(out + 16, ret);
  reply(l, type, I1N1_DLINK_ST_OK, out, NULL, 0);
}

static void do_memread(struct loop *l, uint32_t type, const uint8_t payload[56]) {
  uint64_t addr = i1n1_dlink_load_u64le(payload);
  uint64_t len = i1n1_dlink_load_u64le(payload + 8);
  uint8_t out[I1N1_DLINK_REPLY_PAYLOAD_BYTES];
  uint32_t csum = 0;
  l->st->memreads++;
  if (len == 0 || len > I1N1_DLINK_MAX_TRANSFER) {
    reply(l, type, I1N1_DLINK_ST_INVAL, NULL, NULL, 0);
    return;
  }
  if (!in_window(l, addr, len)) {
    reply(l, type, I1N1_DLINK_ST_XFRERR, NULL, NULL, 0);
    return;
  }
  (void)i1n1_dlink_checksum(l->host + (addr - l->base), (size_t)len, &csum);
  memset(out, 0, sizeof out);
  i1n1_dlink_store_u32le(out, csum);
  reply(l, type, I1N1_DLINK_ST_OK, out, l->host + (addr - l->base), (size_t)len);
}

static void do_memwrite(struct loop *l, uint32_t type, const uint8_t payload[56],
                        const uint8_t *body) {
  uint64_t addr = i1n1_dlink_load_u64le(payload);
  uint64_t len = i1n1_dlink_load_u64le(payload + 8);
  uint32_t want = i1n1_dlink_load_u32le(payload + 16);
  uint32_t got = 0;
  uint8_t out[I1N1_DLINK_REPLY_PAYLOAD_BYTES];
  l->st->memwrites++;
  (void)i1n1_dlink_checksum(body, (size_t)len, &got);
  memset(out, 0, sizeof out);
  i1n1_dlink_store_u32le(out, got);
  if (got != want || !in_window(l, addr, len)) {
    reply(l, type, I1N1_DLINK_ST_XFRERR, out, NULL, 0);
    return;
  }
  memcpy(l->host + (addr - l->base), body, (size_t)len);
  reply(l, type, I1N1_DLINK_ST_OK, out, NULL, 0);
}

void i1n1_dlink_serve(const struct i1n1_dlink_io *io, uint8_t *host,
                      uint64_t base, uint64_t bytes, uint64_t wait_ms,
                      struct i1n1_dlink_stats *stats) {
  struct loop l;
  uint8_t body[I1N1_DLINK_MAX_TRANSFER];
  uint64_t idle_deadline;
  uint32_t window = 0; /* last three bytes, oldest in the low byte */
  if (!io || !io->read_byte || !io->write_byte || !io->now_ms || !stats) return;
  memset(stats, 0, sizeof *stats);
  l.io = io;
  l.host = host;
  l.base = base;
  l.bytes = bytes;
  l.exc_count = 0;
  l.st = stats;
  idle_deadline = io->now_ms(io->ctx) + wait_ms;
  for (;;) {
    uint8_t b;
    if (io->read_byte(io->ctx, &b) != 0) {
      if (io->now_ms(io->ctx) >= idle_deadline) return;
      continue;
    }
    window = (window >> 8) | ((uint32_t)b << 16);
    window &= 0xffffffu;
    /* wire order is FF 55 AA, so after three bytes window == 0xAA55FF */
    if (window == 0xAA55FFu) {
      uint8_t hdr[I1N1_DLINK_REQUEST_BYTES];
      uint64_t deadline = io->now_ms(io->ctx) + I1N1_DLINK_IDLE_TIMEOUT_MS;
      uint32_t type, sum = 0, want;
      window = 0;
      hdr[0] = 0xFF;
      hdr[1] = 0x55;
      hdr[2] = 0xAA;
      if (read_exact(&l, hdr + 3, sizeof hdr - 3, deadline) != 0) return;
      stats->requests++;
      type = i1n1_dlink_load_u32le(hdr);
      (void)i1n1_dlink_checksum(hdr, I1N1_DLINK_REQUEST_CHECKSUM_OFFSET, &sum);
      want = i1n1_dlink_load_u32le(hdr + I1N1_DLINK_REQUEST_CHECKSUM_OFFSET);
      if (sum != want) {
        reply(&l, type, I1N1_DLINK_ST_CSUMERR, NULL, NULL, 0);
      } else {
        const uint8_t *payload = hdr + 4;
        switch (type) {
          case I1N1_DLINK_REQ_NOP:
            stats->nops++;
            reply(&l, type, I1N1_DLINK_ST_OK, NULL, NULL, 0);
            break;
          case I1N1_DLINK_REQ_OP: do_proxy(&l, type, payload); break;
          case I1N1_DLINK_REQ_MEMREAD: do_memread(&l, type, payload); break;
          case I1N1_DLINK_REQ_MEMWRITE: {
            uint64_t len = i1n1_dlink_load_u64le(payload + 8);
            if (len == 0 || len > I1N1_DLINK_MAX_TRANSFER) {
              /* The body length is untrustworthy: answer INVAL and keep
               * scanning; the stray body bytes resynchronize via the magic. */
              reply(&l, type, I1N1_DLINK_ST_INVAL, NULL, NULL, 0);
            } else if (read_exact(&l, body, (size_t)len, deadline) != 0) {
              return;
            } else {
              do_memwrite(&l, type, payload, body);
            }
            break;
          }
          default: /* BOOT, EVENT, unknown: never emulated */
            reply(&l, type, I1N1_DLINK_ST_BADCMD, NULL, NULL, 0);
            break;
        }
      }
      idle_deadline = io->now_ms(io->ctx) + I1N1_DLINK_IDLE_TIMEOUT_MS;
    }
  }
}

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * i1n1.efi boot runner. See runner.h for the pipeline and serial grammar.
 *
 * Design rules enforced here (project rules):
 *  1. An unsupported event is a deterministic `I1N1:STOP`, never a NOP, never
 *     a zero, never a pretend success. The first stop wins and ends the run.
 *  2. Only checkpoints count as progress evidence; they are printed as
 *     `I1N1:CHECKPOINT` lines in strict id order.
 */
#include "runner.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "apple_init_provider.h"
#include "dbt.h"
#include "gicv3_lite.h"
/* guest.h (WS4, internal) and i1n1/cpu.h (WS2, public, pulled in by dbt.h)
 * both define the enumerator I1N1_STOP_NONE. Rename guest.h's for this TU
 * only. */
#define I1N1_STOP_NONE I1N1_GUEST_STOP_NONE
#include "guest.h"
#include "boot_policy.h"
#undef I1N1_STOP_NONE
#include "mmio_router.h"
#include "ept.h"
#include "owned_ram.h"

#define LINE_MAX_BYTES 384u
#define UART_LINE_MAX 160u
#define STEP_CHUNK 4096u
#define PE_SP_RESERVE 16u
#define ARENA_BYTES 0x40000u
#define HVC_OPCODE 0xd4000002u
#define HVC_MASK 0xffe0001fu

struct runner;

struct uart {
  struct runner *rn;
  char line[UART_LINE_MAX];
  size_t n;
  uint64_t bytes;
};

struct runner {
  const struct i1n1_efi_inputs *in;
  const struct i1n1_efi_console *con;
  struct i1n1_efi_result *res;

  struct i1n1_ram ram;
  struct i1n1_ept ept;
  struct i1n1_mmio_router router;
  struct i1n1_gic gic;
  struct uart uart;

  struct i1n1_dbt dbt;
  int dbt_ready;
  struct ocnext_a64_cpu cpu;
  struct ocnext_a64_memory memory;

  struct i1n1_hvc_router hvc;
  struct apple_init_provider apple;
  struct apple_init_key_input apple_in;
  uint64_t psci_off_seen;
  int shutdown;

  struct i1n1_boot_run run;
  int synthetic;
  int armed;
  uint64_t armed_pc;
  uint64_t hvc_pc;
  int stopped;
};

static struct runner g_runner;

/* ---- output ------------------------------------------------------------ */

static void emit(struct runner *rn, const char *s, size_t n) {
  if (rn->con && rn->con->write) rn->con->write(rn->con->ctx, s, n);
}

static void say(struct runner *rn, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void say(struct runner *rn, const char *fmt, ...) {
  char b[LINE_MAX_BYTES];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(b, sizeof b - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n > sizeof b - 2) n = (int)sizeof b - 2;
  b[n++] = '\n';
  emit(rn, b, (size_t)n);
}

static void sanitize(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (cap == 0) return;
  for (; src && src[i] && i + 1 < cap; ++i) {
    unsigned char c = (unsigned char)src[i];
    dst[i] = (c < 32 || c > 126 || c == '"' || c == '\\') ? '?' : (char)c;
  }
  dst[i] = 0;
}

static void hex_of(const uint8_t d[32], char out[65]) { i1n1_hex32(d, out); }

/* ---- stop -------------------------------------------------------------- */

static void do_stop(struct runner *rn, const char *token, uint64_t pc,
                    uint32_t opcode, uint64_t retired, const char *detail,
                    enum i1n1_stop_reason why) {
  char d[112];
  if (rn->stopped) return; /* first stop wins */
  rn->stopped = 1;
  sanitize(d, sizeof d, detail);
  if (!rn->run.has_stop) {
    rn->run.has_stop = 1;
    memset(&rn->run.stop, 0, sizeof rn->run.stop);
    rn->run.stop.reason = why;
    rn->run.stop.pc = pc;
    rn->run.stop.opcode = opcode;
    rn->run.stop.retired = retired;
    memcpy(rn->run.stop.x, rn->cpu.x, sizeof rn->run.stop.x); /* X0..X7 at the stop */
    snprintf(rn->run.stop.detail, sizeof rn->run.stop.detail, "%s", d);
  }
  rn->res->pc = pc;
  rn->res->retired = retired;
  snprintf(rn->res->reason, sizeof rn->res->reason, "%s", token);
  say(rn, "I1N1:STOP reason=%s pc=0x%llx opcode=0x%08x retired=%llu detail=\"%s\"",
      token, (unsigned long long)pc, (unsigned)opcode,
      (unsigned long long)retired, d);
  {
    char js[512];
    size_t n = i1n1_stop_to_json(&rn->run.stop, js, sizeof js);
    /* single line: drop trailing newline, collapse nothing else */
    while (n && (js[n - 1] == '\n' || js[n - 1] == '\r')) js[--n] = 0;
    if (n) {
      emit(rn, "I1N1:STOPJSON ", 14);
      emit(rn, js, n);
      emit(rn, "\n", 1);
    }
  }
}

static void input_stop(struct runner *rn, const char *token, const char *detail) {
  do_stop(rn, token, 0, 0, 0, detail, I1N1_STOP_IMAGE_UNSUPPORTED);
  rn->res->exit_code = I1N1_EFI_EXIT_INPUT;
}

/* ---- checkpoints -------------------------------------------------------- */

static int ckpt(struct runner *rn, enum i1n1_ckpt_id id, uint64_t pc) {
  struct i1n1_ckpt_obs o;
  const struct i1n1_ckpt_def *d = i1n1_ckpt_def(id);
  o.id = id;
  o.pc = pc;
  o.retired = rn->cpu.retired;
  o.blocks = rn->dbt.dispatches;
  if (!rn->synthetic) {
    if (i1n1_ckpt_record(&rn->run, &o) != 0) {
      do_stop(rn, "checkpoint_order", pc, 0, o.retired,
              rn->run.stop.detail, I1N1_STOP_CHECKPOINT_ORDER);
      return -1;
    }
  } else {
    /* The synthetic guest lives at its own addresses, so PC checkpoints are
     * recorded at the PC actually observed. Ordering rules stay identical. */
    if ((unsigned)id != rn->run.nobs ||
        (rn->run.nobs > 0 && o.retired < rn->run.obs[rn->run.nobs - 1].retired)) {
      do_stop(rn, "checkpoint_order", pc, 0, o.retired,
              "synthetic checkpoint out of order", I1N1_STOP_CHECKPOINT_ORDER);
      return -1;
    }
    rn->run.obs[rn->run.nobs++] = o;
  }
  say(rn, "I1N1:CHECKPOINT name=%s id=%u pc=0x%llx retired=%llu blocks=%llu profile=%s",
      d->name, (unsigned)id, (unsigned long long)pc,
      (unsigned long long)o.retired, (unsigned long long)o.blocks,
      rn->synthetic ? "synthetic" : "kernel");
  rn->res->checkpoints = rn->run.nobs;
  return 0;
}

/* ---- guest bus: EPT -> RAM or MMIO router ------------------------------- */

static void le_store(uint8_t *dst, uint64_t v, size_t n) {
  size_t i;
  for (i = 0; i < n; ++i) dst[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t le_load(const uint8_t *src, size_t n) {
  uint64_t v = 0;
  size_t i;
  for (i = 0; i < n; ++i) v |= (uint64_t)src[i] << (8 * i);
  return v;
}

static int bus_read(void *ctx, uint64_t address, uint8_t *dst, size_t size,
                    enum ocnext_a64_access access) {
  struct runner *rn = (struct runner *)ctx;
  struct i1n1_ept_violation v;
  uint64_t hpa = 0;
  uint32_t perm = access == OCNEXT_A64_ACCESS_FETCH ? I1N1_EPT_X : I1N1_EPT_R;
  int rc;
  if (size == 0 || size > 64) return -1;
  rc = i1n1_ept_translate(&rn->ept, address, size, perm, &hpa, &v);
  if (rc == I1N1_EPT_OK) {
    memcpy(dst, (const void *)(uintptr_t)hpa, size);
    return 0;
  }
  if (rc == I1N1_EPT_VIOLATION && v.reason == I1N1_EPT_VIOL_MMIO &&
      size <= 8) {
    uint64_t val = 0;
    if (i1n1_mmio_handle_violation(&rn->router, &v, (unsigned)size, &val) !=
        I1N1_MMIO_OK)
      return -1;
    le_store(dst, val, size);
    return 0;
  }
  return -1; /* hole or permission: the engine reports an exact fault */
}

static int bus_write(void *ctx, uint64_t address, const uint8_t *src,
                     size_t size) {
  struct runner *rn = (struct runner *)ctx;
  struct i1n1_ept_violation v;
  uint64_t hpa = 0;
  int rc;
  if (size == 0 || size > 64) return -1;
  rc = i1n1_ept_translate(&rn->ept, address, size, I1N1_EPT_W, &hpa, &v);
  if (rc == I1N1_EPT_OK) {
    /* SMC contract (docs/contracts/dbt.md): notify before the store lands. */
    if (rn->dbt_ready)
      i1n1_dbt_notify_write(&rn->dbt.cache, &rn->dbt.chain, address, size);
    memcpy((void *)(uintptr_t)hpa, src, size);
    return 0;
  }
  if (rc == I1N1_EPT_VIOLATION && v.reason == I1N1_EPT_VIOL_MMIO &&
      size <= 8) {
    uint64_t val = le_load(src, size);
    return i1n1_mmio_handle_violation(&rn->router, &v, (unsigned)size, &val) ==
                   I1N1_MMIO_OK
               ? 0
               : -1;
  }
  return -1;
}

/* ---- PL011-lite UART ---------------------------------------------------- */

static void uart_flush(struct uart *u) {
  struct runner *rn = u->rn;
  char clean[UART_LINE_MAX + 2];
  size_t i;
  for (i = 0; i < u->n; ++i) {
    unsigned char c = (unsigned char)u->line[i];
    clean[i] = (c < 32 || c > 126) ? '?' : (char)c;
  }
  clean[u->n] = '\n';
  emit(rn, clean, u->n + 1);
  clean[u->n] = 0;
  /* First post-INIT line is the serial marker checkpoint. */
  if (rn->run.nobs == I1N1_CKPT_INIT_SUCCESS + 1 && u->n > 0 &&
      rn->run.serial_marker[0] == 0) {
    snprintf(rn->run.serial_marker, sizeof rn->run.serial_marker, "%s", clean);
    snprintf(rn->res->marker, sizeof rn->res->marker, "%s", clean);
    (void)ckpt(rn, I1N1_CKPT_FIRST_SERIAL, rn->cpu.pc);
  }
  u->n = 0;
}

static int uart_write(void *ctx, uint64_t address, unsigned width,
                      uint64_t value) {
  struct uart *u = (struct uart *)ctx;
  uint64_t off = address - I1N1_EFI_UART_BASE;
  if (off != 0 || width > 4) return 1; /* only UARTDR, byte..word */
  u->bytes++;
  if ((uint8_t)value == '\n') {
    uart_flush(u);
  } else if ((uint8_t)value != '\r') {
    if (u->n >= UART_LINE_MAX) uart_flush(u);
    u->line[u->n++] = (char)(uint8_t)value;
  }
  return 0;
}

static int uart_read(void *ctx, uint64_t address, unsigned width,
                     uint64_t *value) {
  uint64_t off = address - I1N1_EFI_UART_BASE;
  (void)ctx;
  if (off == 0x18u && width <= 4) { /* UARTFR: TX FIFO empty, RX FIFO empty */
    *value = 0x90u;
    return 0;
  }
  return 1; /* RX data and every other register: not implemented */
}

/* ---- HVC providers ------------------------------------------------------ */

static uint64_t guest_pstate(const struct ocnext_a64_cpu *cpu) {
  return (uint64_t)((cpu->system.current_el << 2) | cpu->system.spsel) |
         cpu->system.daif | cpu->nzcv;
}

static enum i1n1_hvc_status apple_handle(void *ctx,
                                         const struct i1n1_hvc_request *rq,
                                         struct i1n1_hvc_response *rs) {
  struct runner *rn = (struct runner *)ctx;
  struct apple_init_hvc_request a;
  struct apple_init_hvc_result r;
  unsigned i;
  memset(&a, 0, sizeof a);
  a.pc = rq->pc;
  a.opcode = rq->opcode;
  a.pstate = guest_pstate(&rn->cpu);
  for (i = 0; i < 4; ++i) a.x[i] = rq->x[i];
  if (apple_init_provider_handle_hvc(&rn->apple, &a, &r) != 0) {
    snprintf(rs->detail, sizeof rs->detail, "apple_init null argument");
    return I1N1_HVC_ERROR;
  }
  if (r.verdict != APPLE_INIT_RESUME) {
    snprintf(rs->detail, sizeof rs->detail, "apple_init stop: %s",
             apple_init_stop_reason_name(r.reason));
    return I1N1_HVC_UNSUPPORTED;
  }
  if (r.next_pc != rq->pc + 4u) {
    snprintf(rs->detail, sizeof rs->detail, "apple_init next_pc mismatch");
    return I1N1_HVC_ERROR;
  }
  for (i = 0; i < 4; ++i) rs->x[i] = r.x[i];
  rs->nx = 4;
  return I1N1_HVC_COMPLETED;
}

static enum i1n1_hvc_status psci_handle(void *ctx,
                                        const struct i1n1_hvc_request *rq,
                                        struct i1n1_hvc_response *rs) {
  struct runner *rn = (struct runner *)ctx;
  if (rq->x[0] == I1N1_EFI_PSCI_SYSTEM_OFF) {
    rn->shutdown = 1;
    rn->psci_off_seen++;
    rs->nx = 0;
    return I1N1_HVC_COMPLETED;
  }
  snprintf(rs->detail, sizeof rs->detail, "psci fid 0x%llx not implemented",
           (unsigned long long)rq->x[0]);
  return I1N1_HVC_UNSUPPORTED;
}

/* ---- config ------------------------------------------------------------- */

static int parse_u64(const char *s, size_t n, uint64_t *out) {
  uint64_t v = 0;
  unsigned base = 10;
  size_t i = 0;
  if (n == 0) return -1;
  if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  }
  for (; i < n; ++i) {
    unsigned d;
    char c = s[i];
    if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10;
    else if (base == 16 && c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10;
    else return -1;
    if (v > (UINT64_MAX - d) / base) return -1;
    v = v * base + d;
  }
  *out = v;
  return 0;
}

int i1n1_efi_config_parse(const char *text, size_t len,
                          struct i1n1_efi_config *cfg, char *err,
                          size_t err_cap) {
  size_t p = 0;
  uint64_t ram_mib = 0;
  int have_mib = 0;
#define FAIL(...)                                  \
  do {                                             \
    if (err && err_cap) snprintf(err, err_cap, __VA_ARGS__); \
    return -1;                                     \
  } while (0)
  if (!text || !cfg) FAIL("null argument");
  memset(cfg, 0, sizeof *cfg);
  cfg->ram_base = I1N1_EFI_DEFAULT_RAM_BASE;
  cfg->max_steps = 50u * 1000u * 1000u;
  cfg->proxy_wait_s = 20;
  while (p < len) {
    size_t e = p, ks, ke, vs, ve;
    uint64_t v;
    while (e < len && text[e] != '\n') ++e;
    ks = p;
    while (ks < e && (text[ks] == ' ' || text[ks] == '\t' || text[ks] == '\r')) ++ks;
    ve = e;
    while (ve > ks && (text[ve - 1] == ' ' || text[ve - 1] == '\t' || text[ve - 1] == '\r')) --ve;
    p = e + 1;
    if (ks >= ve || text[ks] == '#') continue;
    ke = ks;
    while (ke < ve && text[ke] != '=') ++ke;
    if (ke >= ve) FAIL("line without '=': %.20s", text + ks);
    vs = ke + 1;
#define KEY(name) (ke - ks == sizeof(name) - 1 && strncmp(text + ks, name, ke - ks) == 0)
    if (KEY("profile")) {
      if (ve - vs == 9 && strncmp(text + vs, "synthetic", 9) == 0)
        cfg->profile = I1N1_EFI_PROFILE_SYNTHETIC;
      else if (ve - vs == 6 && strncmp(text + vs, "kernel", 6) == 0)
        cfg->profile = I1N1_EFI_PROFILE_KERNEL;
      else FAIL("unknown profile");
    } else if (KEY("ram_base")) {
      if (parse_u64(text + vs, ve - vs, &v)) FAIL("bad ram_base");
      cfg->ram_base = v;
    } else if (KEY("ram_mib")) {
      if (parse_u64(text + vs, ve - vs, &v) || v == 0 || v > (1u << 20)) FAIL("bad ram_mib");
      ram_mib = v;
      have_mib = 1;
    } else if (KEY("max_steps")) {
      if (parse_u64(text + vs, ve - vs, &v) || v == 0) FAIL("bad max_steps");
      cfg->max_steps = v;
    } else if (KEY("proxy")) {
      if (parse_u64(text + vs, ve - vs, &v) || v > 1) FAIL("bad proxy");
      cfg->proxy = (uint32_t)v;
    } else if (KEY("dbt")) {
      if (parse_u64(text + vs, ve - vs, &v) || v > 2) FAIL("bad dbt (0, 1 or 2)");
      cfg->dbt = (uint32_t)v;
    } else if (KEY("nvram")) {
      if (parse_u64(text + vs, ve - vs, &v) || v > 1) FAIL("bad nvram (0 or 1)");
      cfg->nvram = (uint32_t)v;
    } else if (KEY("continue_after_serial")) {
      if (parse_u64(text + vs, ve - vs, &v) || v > 1) FAIL("bad continue_after_serial (0 or 1)");
      cfg->cont = (uint32_t)v;
    } else if (KEY("video")) {
      enum i1n1_video_mode vm;
      if (i1n1_video_mode_parse(text + vs, ve - vs, &vm)) FAIL("bad video (none|stub)");
      cfg->video_mode = (uint32_t)vm;
    } else if (KEY("video_size")) {
      if (i1n1_video_size_parse(text + vs, ve - vs, &cfg->video_w, &cfg->video_h)) FAIL("bad video_size (WxH, 64..4096)");
    } else if (KEY("cmdline")) {
      enum i1n1_cmdline_mode cm;
      if (i1n1_cmdline_policy_parse(text + vs, ve - vs, &cm, cfg->cmdline_raw, sizeof cfg->cmdline_raw))
        FAIL("bad cmdline (file|quiet|raw:<text>)");
      cfg->cmdline_mode = (uint32_t)cm;
    } else if (KEY("proxy_wait_s")) {
      if (parse_u64(text + vs, ve - vs, &v) || v > 3600) FAIL("bad proxy_wait_s");
      cfg->proxy_wait_s = (uint32_t)v;
    } else {
      FAIL("unknown key: %.24s", text + ks);
    }
#undef KEY
  }
  if (cfg->profile == I1N1_EFI_PROFILE_NONE) FAIL("profile is required");
  if (cfg->nvram && cfg->profile != I1N1_EFI_PROFILE_KERNEL) FAIL("nvram=1 needs profile=kernel");
  if (cfg->cont && cfg->profile != I1N1_EFI_PROFILE_KERNEL) FAIL("continue_after_serial=1 needs profile=kernel");
  if (cfg->dbt && cfg->profile != I1N1_EFI_PROFILE_KERNEL) FAIL("dbt=1|2 needs profile=kernel");
  if ((cfg->video_mode || cfg->video_w || cfg->cmdline_mode) && cfg->profile != I1N1_EFI_PROFILE_KERNEL)
    FAIL("video/cmdline policy needs profile=kernel");
  if (cfg->video_w && !cfg->video_mode) FAIL("video_size needs video=stub");
  if (!have_mib)
    ram_mib = cfg->profile == I1N1_EFI_PROFILE_SYNTHETIC ? 16u : 4096u;
  cfg->ram_bytes = ram_mib << 20;
  if (cfg->ram_base & (I1N1_GRANULE - 1)) FAIL("ram_base not 16 KiB aligned");
#undef FAIL
  return 0;
}

/* ---- run ---------------------------------------------------------------- */

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static const char *fault_token(uint32_t code) {
  switch (code) {
    case I1N1_STOP_UNSUPPORTED: return "unsupported_insn";
    case I1N1_STOP_FETCH: return "guest_fetch_fault";
    case I1N1_STOP_READ: return "guest_read_fault";
    case I1N1_STOP_WRITE: return "guest_write_fault";
    case I1N1_STOP_ALIGNMENT: return "guest_alignment_fault";
    case I1N1_STOP_DEBUG_HALT: return "guest_debug_halt";
    case I1N1_STOP_ARGUMENT: return "engine_argument";
    case I1N1_STOP_UNDEFINED: return "guest_undefined";
    case I1N1_STOP_SYSTEM_ACCESS: return "guest_system_access";
    case I1N1_STOP_EXCEPTION_RETURN: return "guest_exception_return";
    case I1N1_STOP_SYNC_PROVIDER: return "sync_provider";
    case I1N1_STOP_MAINTENANCE_PROVIDER: return "maintenance_provider";
    default: return "engine_stop";
  }
}

/* Returns 1 when the DBT stop is an HVC #imm trap that the router must see. */
static int is_hvc_stop(const struct runner *rn, const struct i1n1_stop_info *s) {
  return s->code == I1N1_STOP_UNSUPPORTED &&
         (s->opcode & HVC_MASK) == HVC_OPCODE && s->pc == rn->cpu.pc;
}

/* Dispatches the HVC at the current PC. Returns 0 to resume, -1 after a stop. */
static int service_hvc(struct runner *rn, const struct i1n1_stop_info *s) {
  struct i1n1_hvc_request rq;
  struct i1n1_hvc_outcome out;
  unsigned i;
  memset(&rq, 0, sizeof rq);
  rq.pc = s->pc;
  rq.opcode = s->opcode;
  rq.imm16 = (s->opcode >> 5) & 0xffffu;
  for (i = 0; i < 8; ++i) rq.x[i] = rn->cpu.x[i];
  rq.retired = rn->cpu.retired;

  if (rq.x[0] == I1N1_HVC_FID_VMAPPLE_PAC_SET_INITIAL_STATE &&
      rn->run.nobs == I1N1_CKPT_PRE_STOP) {
    rn->hvc_pc = rq.pc;
    if (ckpt(rn, I1N1_CKPT_PRE_STOP, rq.pc) != 0) return -1;
  }
  i1n1_hvc_dispatch(&rn->hvc, &rq, &out);
  if (!out.completed) {
    do_stop(rn, i1n1_stop_reason_name(out.stop.reason), out.stop.pc,
            out.stop.opcode, out.stop.retired, out.stop.detail,
            out.stop.reason);
    return -1;
  }
  for (i = 0; i < out.nx && i < 4; ++i) rn->cpu.x[i] = out.x[i];
  rn->cpu.pc = out.resume_pc;
  rn->cpu.retired++;
  memset(&rn->cpu.fault, 0, sizeof rn->cpu.fault);
  say(rn, "I1N1:HVC pc=0x%llx fid=0x%llx status=completed nx=%u resume=0x%llx",
      (unsigned long long)rq.pc, (unsigned long long)rq.x[0], out.nx,
      (unsigned long long)out.resume_pc);
  if (rq.x[0] == I1N1_HVC_FID_VMAPPLE_PAC_SET_INITIAL_STATE &&
      rn->run.nobs == I1N1_CKPT_PRE_STOP + 1) {
    rn->armed = 1;
    rn->armed_pc = rn->synthetic ? rq.pc + APPLE_INIT_RET_OFFSET_FROM_HVC
                                 : I1N1_CKPT_RET_PC;
  }
  return 0;
}

static int observe_cold_ret(struct runner *rn) {
  rn->armed = 0;
  if (ckpt(rn, I1N1_CKPT_RET_TARGET, rn->cpu.pc) != 0) return -1;
  /* INIT success: the provider committed exactly one INIT with a resume
   * verdict, and the guest sees the provider's X0 (0 = success) at the RET. */
  if (rn->apple.init_count != 1 || !rn->apple.keys.initialized ||
      rn->apple.stopped || rn->cpu.x[0] != 0) {
    do_stop(rn, "init_not_confirmed", rn->cpu.pc, 0, rn->cpu.retired,
            "provider did not report a committed INIT at the cold RET",
            I1N1_STOP_CHECKPOINT_ORDER);
    return -1;
  }
  return ckpt(rn, I1N1_CKPT_INIT_SUCCESS, rn->cpu.pc);
}

static void run_guest(struct runner *rn, uint64_t max_steps) {
  uint64_t steps = 0;
  while (!rn->stopped) {
    struct i1n1_dbt_outcome o;
    enum i1n1_dbt_status st;
    uint64_t budget;
    if (rn->armed && rn->cpu.pc == rn->armed_pc) {
      if (observe_cold_ret(rn) != 0) return;
    }
    if (rn->shutdown) return;
    if (!rn->synthetic && rn->run.nobs == I1N1_CKPT_COUNT) return;
    if (steps >= max_steps) {
      do_stop(rn, "step_budget", rn->cpu.pc, 0, rn->cpu.retired,
              "max_steps exhausted before shutdown or all checkpoints",
              I1N1_STOP_UNSUPPORTED_INSN);
      return;
    }
    budget = rn->armed ? 1u : STEP_CHUNK;
    if (budget > max_steps - steps) budget = max_steps - steps;
    st = i1n1_dbt_run(&rn->dbt, &rn->cpu, &rn->memory, budget, 0, &o);
    steps += o.steps;
    if (st == I1N1_DBT_BUDGET) continue;
    if (st == I1N1_DBT_DONE) {
      if (rn->shutdown) return;
      do_stop(rn, "guest_halted", rn->cpu.pc, rn->cpu.last_opcode,
              rn->cpu.retired, "guest halted without a shutdown request",
              I1N1_STOP_UNSUPPORTED_INSN);
      return;
    }
    if (st == I1N1_DBT_STOP) {
      if (is_hvc_stop(rn, &o.stop)) {
        if (service_hvc(rn, &o.stop) != 0) return;
        continue;
      }
      /* Unsupported instruction, bad memory access, MMIO refusal, ... */
      {
        char d[112];
        if (rn->router.stopped) {
          snprintf(d, sizeof d, "mmio %s at 0x%llx: %s",
                   rn->router.stop.is_write ? "write" : "read",
                   (unsigned long long)rn->router.stop.gpa,
                   i1n1_mmio_status_str(rn->router.stop.status));
          do_stop(rn, "mmio_fault", o.stop.pc, o.stop.opcode, rn->cpu.retired,
                  d, I1N1_STOP_UNSUPPORTED_INSN);
        } else {
          snprintf(d, sizeof d, "%s addr=0x%llx", o.stop.reason,
                   (unsigned long long)o.stop.address);
          do_stop(rn, fault_token(o.stop.code), o.stop.pc, o.stop.opcode,
                  rn->cpu.retired, d, I1N1_STOP_UNSUPPORTED_INSN);
        }
      }
      return;
    }
    do_stop(rn, "dbt_argument", rn->cpu.pc, 0, rn->cpu.retired,
            "dbt rejected its arguments", I1N1_STOP_UNSUPPORTED_INSN);
    return;
  }
}

static const char *setup_devices(struct runner *rn) {
  struct i1n1_mmio_device d;
  memset(&d, 0, sizeof d);
  i1n1_mmio_router_init(&rn->router);
  rn->uart.rn = rn;
  d.name = "pl011-lite";
  d.base = I1N1_EFI_UART_BASE;
  d.size = I1N1_EFI_UART_SIZE;
  d.perm = I1N1_MMIO_PERM_R | I1N1_MMIO_PERM_W;
  d.context = &rn->uart;
  d.read = uart_read;
  d.write = uart_write;
  if (i1n1_mmio_register(&rn->router, &d) != I1N1_MMIO_OK) return "uart registration";
  if (i1n1_gic_init(&rn->gic, 1, I1N1_EFI_GICD_BASE, I1N1_EFI_GICR_BASE) !=
      I1N1_GIC_OK)
    return "gic init";
  if (i1n1_gic_register(&rn->gic, &rn->router) != I1N1_MMIO_OK) return "gic registration";

  i1n1_ept_init(&rn->ept);
  if (i1n1_ept_map_ram(&rn->ept, &rn->ram) != I1N1_EPT_OK) return "ept map of guest RAM";
  if (i1n1_ept_declare_mmio(&rn->ept, I1N1_EFI_UART_BASE, I1N1_EFI_UART_SIZE) != I1N1_EPT_OK)
    return "ept uart hole";
  if (i1n1_ept_declare_mmio(&rn->ept, I1N1_EFI_GICD_BASE, I1N1_GICD_SIZE) != I1N1_EPT_OK)
    return "ept gicd hole";
  if (i1n1_ept_declare_mmio(&rn->ept, I1N1_EFI_GICR_BASE, I1N1_GICR_STRIDE) != I1N1_EPT_OK)
    return "ept gicr hole";
  return NULL;
}

int i1n1_efi_boot_guest(const struct i1n1_efi_inputs *in,
                        const struct i1n1_efi_console *con,
                        struct plat_phase_tracker *adapter,
                        struct i1n1_efi_result *res) {
  struct runner *rn = &g_runner;
  struct i1n1_guest_ram gram;
  struct i1n1_image img;
  struct i1n1_bootargs ba;
  struct i1n1_pe_state pe;
  struct i1n1_stage1 s1;
  struct i1n1_ram_region region;
  struct i1n1_hvc_provider prov;
  struct i1n1_dbt_config dcfg;
  uint8_t ba_bytes[I1N1_BOOTARGS_BYTES];
  char hex[65];
  uint64_t boot_args_pa, dt_pa, load_pa, arena_pa, top, dt_res;
  size_t cmd_n;
  unsigned i;

  if (!res) return I1N1_EFI_EXIT_INPUT;
  memset(res, 0, sizeof *res);
  res->exit_code = I1N1_EFI_EXIT_STOP;
  memset(rn, 0, sizeof *rn);
  rn->in = in;
  rn->con = con;
  rn->res = res;
  if (!in || !con) {
    res->exit_code = I1N1_EFI_EXIT_INPUT;
    return res->exit_code;
  }
  rn->synthetic = in->cfg.profile == I1N1_EFI_PROFILE_SYNTHETIC;
  say(rn, "I1N1:BOOT runner=%u profile=%s ram_base=0x%llx ram_bytes=0x%llx max_steps=%llu",
      I1N1_EFI_RUNNER_VERSION, rn->synthetic ? "synthetic" : "kernel",
      (unsigned long long)in->cfg.ram_base, (unsigned long long)in->cfg.ram_bytes,
      (unsigned long long)in->cfg.max_steps);

  /* platform lifetime rule: the guest only runs after ExitBootServices. */
  if (plat_phase_require_post_ebs(adapter, "i1n1_efi_boot_guest") != 0) {
    input_stop(rn, "phase_violation", plat_phase_last_reason(adapter));
    return res->exit_code;
  }
  say(rn, "I1N1:PLAT phase=post_ebs transitions=%llu rejected=%llu",
      (unsigned long long)adapter->transitions,
      (unsigned long long)plat_phase_rejected(adapter));

  if (in->cfg.profile != I1N1_EFI_PROFILE_SYNTHETIC &&
      in->cfg.profile != I1N1_EFI_PROFILE_KERNEL) {
    input_stop(rn, "config_invalid", "unknown profile");
    return res->exit_code;
  }
  if (!in->guest || in->guest_bytes == 0 || !in->dt || in->dt_bytes == 0 ||
      !in->ram || in->cfg.ram_bytes < 0x100000u) {
    input_stop(rn, "input_missing", "guest image, device tree and RAM are required");
    return res->exit_code;
  }
  /* The EPT model maps host backing at 4 KiB granularity (AllocatePages gives
   * that; a host test must use an aligned allocation). */
  if (((uintptr_t)in->ram & 0xfffu) != 0 || (in->cfg.ram_bytes & 0xfffu) != 0) {
    input_stop(rn, "ram_unaligned", "guest RAM backing must be 4 KiB aligned and sized");
    return res->exit_code;
  }
  if (in->cmdline_bytes && !in->cmdline) {
    input_stop(rn, "input_missing", "cmdline pointer is null");
    return res->exit_code;
  }

  if (!rn->synthetic) return i1n1_efi_boot_kernel(in, con, res);

  /* Input digests (receipt fields). */
  if (in->efi_image && in->efi_bytes)
    i1n1_sha256(in->efi_image, in->efi_bytes, rn->run.efi_sha256);
  if (in->efi_image && in->efi_bytes) {
    hex_of(rn->run.efi_sha256, hex);
    say(rn, "I1N1:INPUT name=efi bytes=%llu sha256=%s", (unsigned long long)in->efi_bytes, hex);
  }
  i1n1_sha256(in->guest, in->guest_bytes, rn->run.image_sha256);
  i1n1_sha256(in->dt, in->dt_bytes, rn->run.dt_sha256);
  hex_of(rn->run.image_sha256, hex);
  say(rn, "I1N1:INPUT name=guest bytes=%llu sha256=%s", (unsigned long long)in->guest_bytes, hex);
  hex_of(rn->run.dt_sha256, hex);
  say(rn, "I1N1:INPUT name=dt bytes=%llu sha256=%s", (unsigned long long)in->dt_bytes, hex);

  /* ---- RAM + WS4 layout ---- */
  memset(&region, 0, sizeof region);
  region.guest_base = in->cfg.ram_base;
  region.bytes = in->cfg.ram_bytes;
  region.perm = I1N1_RAM_PERM_ALL;
  region.backing = in->ram;
  if (i1n1_ram_init(&rn->ram, &region, 1) != I1N1_RAM_OK) {
    input_stop(rn, "ram_invalid", "owned RAM layout rejected");
    return res->exit_code;
  }
  gram.host = in->ram;
  gram.pa_base = in->cfg.ram_base;
  gram.size = in->cfg.ram_bytes;

  boot_args_pa = in->cfg.ram_base + 0x4000u;
  dt_pa = boot_args_pa + 0x4000u;
  dt_res = align_up(in->dt_bytes, I1N1_GRANULE);
  load_pa = align_up(dt_pa + dt_res, 0x10000u);

  memset(&img, 0, sizeof img);
  {
    struct i1n1_image_config icfg;
    enum i1n1_image_result ir;
    icfg.load_pa = load_pa;
    icfg.virt_base = load_pa; /* flat, MMU off in the DBT: VA == PA */
    ir = i1n1_image_load(in->guest, in->guest_bytes, &icfg, &gram, &img);
    if (ir != I1N1_IMG_OK) {
      char d[112];
      snprintf(d, sizeof d, "%s: %s", i1n1_image_result_name(ir), img.detail);
      input_stop(rn, "image_unsupported", d);
      return res->exit_code;
    }
  }
  say(rn, "I1N1:LOAD image=ok segs=%u entry=0x%llx extent=0x%llx fixups=%u arm64e=%d load_pa=0x%llx",
      img.nsegs, (unsigned long long)img.entry_va, (unsigned long long)img.extent_bytes,
      (unsigned)img.fixups_applied, img.is_arm64e, (unsigned long long)load_pa);

  if (i1n1_dt_validate(in->dt, in->dt_bytes, NULL) != 0) {
    input_stop(rn, "dt_invalid", "device tree failed validation");
    return res->exit_code;
  }
  memcpy(in->ram + (dt_pa - in->cfg.ram_base), in->dt, in->dt_bytes);

  cmd_n = in->cmdline_bytes;
  while (cmd_n && (in->cmdline[cmd_n - 1] == '\n' || in->cmdline[cmd_n - 1] == '\r' ||
                   in->cmdline[cmd_n - 1] == ' '))
    --cmd_n;
  arena_pa = align_up(load_pa + img.extent_bytes, I1N1_GRANULE);
  top = arena_pa + ARENA_BYTES;
  memset(&ba, 0, sizeof ba);
  ba.revision = 3;
  ba.version = 2;
  ba.virt_base = in->cfg.ram_base;
  ba.phys_base = in->cfg.ram_base;
  ba.mem_size = ba.mem_size_actual = in->cfg.ram_bytes;
  ba.kernel_pa = load_pa;
  ba.kernel_bytes = img.extent_bytes;
  ba.boot_args_pa = boot_args_pa;
  ba.device_tree_pa = dt_pa;
  ba.device_tree_bytes = (uint32_t)in->dt_bytes;
  ba.top_of_kernel_data = top;
  ba.command_line = in->cmdline;
  ba.command_line_bytes = cmd_n;
  if (top > in->cfg.ram_base + in->cfg.ram_bytes) {
    input_stop(rn, "ram_too_small", "image + stage-1 arena exceed guest RAM");
    return res->exit_code;
  }
  if (i1n1_bootargs_encode(&ba, ba_bytes, sizeof ba_bytes) != I1N1_BA_OK ||
      i1n1_bootargs_validate(ba_bytes, sizeof ba_bytes) != 0) {
    input_stop(rn, "bootargs_invalid", "boot args encode/validate failed");
    return res->exit_code;
  }
  memcpy(in->ram + (boot_args_pa - in->cfg.ram_base), ba_bytes, sizeof ba_bytes);
  i1n1_sha256(ba_bytes, sizeof ba_bytes, rn->run.boot_args_sha256);
  hex_of(rn->run.boot_args_sha256, hex);
  say(rn, "I1N1:BOOTARGS pa=0x%llx dt_pa=0x%llx dt_bytes=%llu top=0x%llx sha256=%s",
      (unsigned long long)boot_args_pa, (unsigned long long)dt_pa,
      (unsigned long long)in->dt_bytes, (unsigned long long)top, hex);

  /* Stage-1 tables for the image (identity VA == PA). They are built and
   * validated here; the DBT core runs MMU-off on a flat bus, so TTBR1 stays
   * 0 in the PE state. Enabling translation is WS2 work. */
  if (i1n1_stage1_init(&s1, &gram, arena_pa, ARENA_BYTES) != I1N1_S1_OK) {
    input_stop(rn, "stage1_failed", "arena rejected");
    return res->exit_code;
  }
  for (i = 0; i < img.nsegs; ++i) {
    uint64_t va = img.segs[i].pa; /* identity */
    uint64_t sz = align_up(img.segs[i].vmsize, I1N1_GRANULE);
    if (i1n1_stage1_map(&s1, va, img.segs[i].pa, sz, img.segs[i].prot,
                        I1N1_S1_ATTR_NORMAL) != I1N1_S1_OK) {
      input_stop(rn, "stage1_failed", "segment mapping rejected");
      return res->exit_code;
    }
  }
  if (i1n1_stage1_map(&s1, I1N1_EFI_UART_BASE, I1N1_EFI_UART_BASE,
                      align_up(I1N1_EFI_UART_SIZE, I1N1_GRANULE),
                      I1N1_PROT_R | I1N1_PROT_W, I1N1_S1_ATTR_DEVICE) != I1N1_S1_OK) {
    input_stop(rn, "stage1_failed", "uart mapping rejected");
    return res->exit_code;
  }
  say(rn, "I1N1:STAGE1 root=0x%llx used=0x%llx mmu=off-in-dbt",
      (unsigned long long)s1.root_pa, (unsigned long long)s1.arena_used);

  if (i1n1_pe_state_build(&pe, img.entry_va, boot_args_pa,
                          in->cfg.ram_base + in->cfg.ram_bytes - PE_SP_RESERVE,
                          0, I1N1_PSTATE_EL1H_DAIF) != 0) {
    input_stop(rn, "pe_state_invalid", "entry/boot args/pstate rejected");
    return res->exit_code;
  }
  hex_of(pe.digest, hex);
  say(rn, "I1N1:PE pc=0x%llx x0=0x%llx sp=0x%llx pstate=0x%llx digest=%s",
      (unsigned long long)pe.pc, (unsigned long long)pe.x[0],
      (unsigned long long)pe.sp, (unsigned long long)pe.pstate, hex);

  /* ---- devices, bus, providers ---- */
  {
    const char *why = setup_devices(rn);
    if (why) {
      input_stop(rn, "device_setup_failed", why);
      return res->exit_code;
    }
  }
  say(rn, "I1N1:DEVICE name=pl011-lite base=0x%llx size=0x%llx",
      (unsigned long long)I1N1_EFI_UART_BASE, (unsigned long long)I1N1_EFI_UART_SIZE);
  say(rn, "I1N1:DEVICE name=gicv3-lite gicd=0x%llx gicr=0x%llx",
      (unsigned long long)I1N1_EFI_GICD_BASE, (unsigned long long)I1N1_EFI_GICR_BASE);

  i1n1_hvc_router_init(&rn->hvc);
  {
    const uint8_t *seed = in->seed;
    uint8_t synth[32];
    const char *seed_src = "esp-file";
    if (!seed && rn->synthetic) {
      static const char tag[] = "i1n1.selftest.synthetic-apple-init-seed/1";
      i1n1_sha256(tag, sizeof tag - 1, synth);
      seed = synth;
      seed_src = "synthetic-selftest";
    }
    apple_init_key_input_clear(&rn->apple_in);
    if (seed) apple_init_key_input_set(&rn->apple_in, seed);
    else seed_src = "none";
    apple_init_provider_reset(&rn->apple, seed ? &rn->apple_in : NULL);
    say(rn, "I1N1:PROVIDER name=apple_init fid=0xc1000000 profile=research-software-keys seed=%s",
        seed_src);
  }
  memset(&prov, 0, sizeof prov);
  prov.abi_version = I1N1_HVC_ABI_VERSION;
  prov.name = "apple_init";
  prov.fid_lo = prov.fid_hi = APPLE_INIT_FID_SET_INITIAL_STATE;
  prov.ctx = rn;
  prov.handle = apple_handle;
  if (i1n1_hvc_register(&rn->hvc, &prov) != 0) {
    input_stop(rn, "provider_register_failed", "apple_init");
    return res->exit_code;
  }
  if (rn->synthetic) {
    memset(&prov, 0, sizeof prov);
    prov.abi_version = I1N1_HVC_ABI_VERSION;
    prov.name = "psci-system-off";
    prov.fid_lo = 0x84000000ull;
    prov.fid_hi = 0x8400ffffull;
    prov.ctx = rn;
    prov.handle = psci_handle;
    if (i1n1_hvc_register(&rn->hvc, &prov) != 0) {
      input_stop(rn, "provider_register_failed", "psci");
      return res->exit_code;
    }
  }

  /* ---- DBT + CPU ---- */
  i1n1_dbt_config_default(&dcfg);
  if (i1n1_dbt_init(&rn->dbt, &dcfg) != 0) {
    input_stop(rn, "dbt_init_failed", "translation cache allocation failed");
    return res->exit_code;
  }
  rn->dbt_ready = 1;
  rn->memory.context = rn;
  rn->memory.read = bus_read;
  rn->memory.write = bus_write;
  ocnext_a64_reset(&rn->cpu, pe.pc, pe.sp);
  if (ocnext_a64_configure_system(&rn->cpu, OCNEXT_A64_SYSTEM_ARMV8_EL1_SYNC, 1) != 0 ||
      ocnext_a64_configure_banked_sp(&rn->cpu, 0, pe.sp, 1) != 0 ||
      ocnext_a64_configure_daif_access(&rn->cpu, 0,
                                       (uint32_t)(pe.pstate & OCNEXT_A64_DAIF_MASK)) != 0) {
    input_stop(rn, "cpu_config_failed", "EL1 system profile rejected");
    i1n1_dbt_fini(&rn->dbt);
    return res->exit_code;
  }
  for (i = 0; i < 31; ++i) rn->cpu.x[i] = pe.x[i];
  say(rn, "I1N1:RUN start pc=0x%llx x0=0x%llx el=1 profile=%s",
      (unsigned long long)rn->cpu.pc, (unsigned long long)rn->cpu.x[0],
      rn->synthetic ? "synthetic" : "kernel");

  run_guest(rn, in->cfg.max_steps);

  /* Flush a partial UART line so a missing newline cannot hide output. */
  if (rn->uart.n) uart_flush(&rn->uart);

  res->retired = rn->cpu.retired;
  res->blocks = rn->dbt.dispatches;
  res->pc = rn->cpu.pc;
  rn->run.blocks = rn->dbt.dispatches;
  rn->run.instructions = rn->cpu.retired;

  if (!rn->stopped && rn->synthetic && !rn->shutdown) {
    do_stop(rn, "run_ended", rn->cpu.pc, 0, rn->cpu.retired,
            "run ended without shutdown", I1N1_STOP_UNSUPPORTED_INSN);
  }
  if (!rn->stopped && i1n1_run_passed(&rn->run)) {
    uint8_t dg[32];
    static char json[2048];
    size_t n;
    res->passed = 1;
    res->exit_code = I1N1_EFI_EXIT_PASSED;
    i1n1_run_digest(&rn->run, dg);
    hex_of(dg, hex);
    say(rn, "I1N1:RESULT status=passed checkpoints=%u retired=%llu blocks=%llu psci_off=%llu run_digest=%s",
        rn->run.nobs, (unsigned long long)res->retired,
        (unsigned long long)res->blocks, (unsigned long long)rn->psci_off_seen, hex);
    n = i1n1_run_receipt_json(&rn->run, json, sizeof json);
    if (n) {
      while (n && (json[n - 1] == '\n' || json[n - 1] == '\r')) json[--n] = 0;
      /* emitted in chunks: the receipt JSON can exceed one say() line */
      emit(rn, "I1N1:RECEIPT ", 13);
      emit(rn, json, n);
      emit(rn, "\n", 1);
    }
  } else {
    if (!rn->stopped) {
      do_stop(rn, "incomplete", rn->cpu.pc, 0, rn->cpu.retired,
              "shutdown before all checkpoints were observed",
              I1N1_STOP_CHECKPOINT_ORDER);
    }
    {
      /* The digest of a stopped run is still deterministic evidence (same
       * inputs stop the same way); it never makes the run "pass". */
      uint8_t dg[32];
      i1n1_run_digest(&rn->run, dg);
      hex_of(dg, hex);
    }
    say(rn, "I1N1:RESULT status=stopped checkpoints=%u reason=%s retired=%llu blocks=%llu run_digest=%s",
        rn->run.nobs, res->reason, (unsigned long long)res->retired,
        (unsigned long long)res->blocks, hex);
  }
  res->checkpoints = rn->run.nobs;
  i1n1_dbt_fini(&rn->dbt);
  rn->dbt_ready = 0;
  return res->exit_code;
}

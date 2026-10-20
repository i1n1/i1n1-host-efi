/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * i1n1.efi `profile=kernel`: boots the real kernelcache on the shared boot
 * machine (src/guest/machine.c, the same code the engine fixture and the
 * native harness run) up to the four M-BOOT checkpoints.
 *
 * Inputs (all from the ESP, never embedded): the physical-layout Mach-O, the
 * Apple device tree, the 1152-byte boot-args page and the research cmdline.
 * Geometry (virt/phys base, RAM size, DT address, boot-args page) is derived
 * from boot-args.bin and cross-checked against the Mach-O; nothing is
 * guessed. See docs/contracts/boot-efi.md section 9 and boot-cpu-profile.md.
 *
 * Honesty rules (project rules): every unsupported event is a deterministic
 * `I1N1:STOP`; checkpoints are printed only from state the machine observed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "runner.h"

#include "adt_finalize.h"
#include "boot_dt_policy.h"
#include "boot_policy.h"
#include "owned_ram.h"
#include "machine.h"

#define BOOT_ARGS_BYTES 1152u
#define BOOT_ARGS_PAGE 0x4000u
#define BOOT_ARGS_CMDLINE_OFF 108u
#define BOOT_ARGS_CMDLINE_MAX 1024u
#define DT_MAX_BYTES 65536u
#define GRANULE 0x4000u
#define LINE_BYTES 1024u
#define CHUNK_DISPATCHES 1000000ull
#define LC_SEGMENT_64 0x19u
#define LC_UNIXTHREAD 0x5u
#define MH_MAGIC_64 0xfeedfacfu
#define CPU_TYPE_ARM64 0x0100000cu

/* Research platform map of the kernel profile (docs/contracts/boot-cpu-profile.md section 4). */
#define K_UART_BASE UINT64_C(0x20010000)
#define K_GICD_BASE UINT64_C(0x10000000)
#define K_GICR_BASE UINT64_C(0x10010000)

struct kb {
  const struct i1n1_efi_console *con;
  struct i1n1_efi_result *res;
  struct i1n1_boot_run run;
  int stopped;
};

static struct kb g_kb;
static struct i1n1_machine g_machine;
static struct i1n1_boot_dt_policy g_policy;
static uint8_t g_dt_scratch[DT_MAX_BYTES];

/* ---- output --------------------------------------------------------------- */

static void kemit(struct kb *k, const char *s, size_t n) {
  if (k->con && k->con->write) k->con->write(k->con->ctx, s, n);
}

static void ksay(struct kb *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ksay(struct kb *k, const char *fmt, ...) {
  char b[LINE_BYTES];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(b, sizeof b - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n > sizeof b - 2) n = (int)sizeof b - 2;
  b[n++] = '\n';
  kemit(k, b, (size_t)n);
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

static uint64_t le(const uint8_t *p, unsigned n) {
  uint64_t v = 0;
  unsigned i;
  for (i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
  return v;
}

static void put_le(uint8_t *p, uint64_t v, unsigned n) {
  unsigned i;
  for (i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t kms(struct kb *k) {
  return (k->con && k->con->now_ms) ? k->con->now_ms(k->con->ctx) : 0;
}

/* ---- stop ------------------------------------------------------------------ */

static void kstop(struct kb *k, const char *token, uint64_t pc, uint32_t opcode,
                  uint64_t retired, const char *detail, enum i1n1_stop_reason why,
                  const struct i1n1_stop *src) {
  char d[112];
  char js[512];
  size_t n;
  if (k->stopped) return;
  k->stopped = 1;
  sanitize(d, sizeof d, detail);
  if (!k->run.has_stop) {
    k->run.has_stop = 1;
    if (src) {
      k->run.stop = *src;
    } else {
      memset(&k->run.stop, 0, sizeof k->run.stop);
      k->run.stop.reason = why;
      k->run.stop.pc = pc;
      k->run.stop.opcode = opcode;
      k->run.stop.retired = retired;
      snprintf(k->run.stop.detail, sizeof k->run.stop.detail, "%s", d);
    }
  }
  k->res->pc = pc;
  k->res->retired = retired;
  snprintf(k->res->reason, sizeof k->res->reason, "%s", token);
  ksay(k, "I1N1:STOP reason=%s pc=0x%llx opcode=0x%08x retired=%llu detail=\"%s\"", token,
       (unsigned long long)pc, (unsigned)opcode, (unsigned long long)retired, d);
  n = i1n1_stop_to_json(&k->run.stop, js, sizeof js);
  while (n && (js[n - 1] == '\n' || js[n - 1] == '\r')) js[--n] = 0;
  if (n) {
    kemit(k, "I1N1:STOPJSON ", 14);
    kemit(k, js, n);
    kemit(k, "\n", 1);
  }
}

static int kinput(struct kb *k, const char *token, const char *detail) {
  kstop(k, token, 0, 0, 0, detail, I1N1_STOP_IMAGE_UNSUPPORTED, NULL);
  k->res->exit_code = I1N1_EFI_EXIT_INPUT;
  return k->res->exit_code;
}

/* ---- Mach-O verification --------------------------------------------------- */

/* The study image is a physical-layout fileset Mach-O: for every segment with
 * file contents, file offset == vmaddr - virt_base, so the file is the RAM image. The loader does
 * not rebase anything (chained fixups stay untouched, exactly as the engine fixture ran it). */
int i1n1_kernel_macho_check(const uint8_t *img, size_t bytes, uint64_t virt_base,
                            struct i1n1_kernel_macho *out, char *why, size_t why_cap) {
  uint32_t ncmds, sizeofcmds, i;
  size_t off = 32;
  uint64_t lowest = UINT64_MAX;
  int have_entry = 0;
#define BAD(msg)                                                  \
  do {                                                            \
    if (why && why_cap) snprintf(why, why_cap, "%s", msg);        \
    return -1;                                                    \
  } while (0)
  if (!img || !out) BAD("null argument");
  memset(out, 0, sizeof *out);
  if (bytes < 32 || le(img, 4) != MH_MAGIC_64) BAD("not a 64-bit Mach-O");
  if (le(img + 4, 4) != CPU_TYPE_ARM64) BAD("cputype is not arm64");
  out->filetype = (uint32_t)le(img + 12, 4);
  ncmds = (uint32_t)le(img + 16, 4);
  sizeofcmds = (uint32_t)le(img + 20, 4);
  if (sizeofcmds > bytes - 32 || ncmds == 0 || ncmds > 4096) BAD("load commands exceed the file");
  for (i = 0; i < ncmds; ++i) {
    uint32_t cmd, cs;
    if (off + 8 > 32 + (size_t)sizeofcmds) BAD("load command table truncated");
    cmd = (uint32_t)le(img + off, 4);
    cs = (uint32_t)le(img + off + 4, 4);
    if (cs < 8 || (cs & 7u) || off + cs > 32 + (size_t)sizeofcmds) BAD("bad load command size");
    if (cmd == LC_SEGMENT_64) {
      uint64_t va, vsz, fo, fs;
      uint32_t prot;
      if (cs < 72) BAD("short LC_SEGMENT_64");
      va = le(img + off + 24, 8);
      vsz = le(img + off + 32, 8);
      fo = le(img + off + 40, 8);
      fs = le(img + off + 48, 8);
      prot = (uint32_t)le(img + off + 60, 4);
      if (fs > bytes || fo > bytes - fs) BAD("segment file range outside the file");
      if (fs > vsz) BAD("segment filesize exceeds vmsize");
      if (fs != 0 && va - virt_base != fo) BAD("segment is not in physical layout (offset != va - virt_base)");
      if ((prot & 2u) && (prot & 4u)) BAD("W+X segment");
      if (va < lowest) lowest = va;
      if (out->nsegs < I1N1_KERNEL_MACHO_SEGS) {
        out->seg[out->nsegs].va = va;
        out->seg[out->nsegs].vmsize = vsz;
        out->seg[out->nsegs].prot = prot;
      }
      out->nsegs++;
      if (va - virt_base + vsz > out->extent) out->extent = va - virt_base + vsz;
    } else if (cmd == LC_UNIXTHREAD) {
      /* flavor, count, then ARM_THREAD_STATE64 (x0..x28, fp, lr, sp, pc, cpsr) */
      if (cs != 288 || le(img + off + 8, 4) != 6u || le(img + off + 12, 4) != 68u)
        BAD("LC_UNIXTHREAD is not ARM_THREAD_STATE64");
      if (have_entry) BAD("more than one LC_UNIXTHREAD");
      out->entry_va = le(img + off + 16 + 256, 8);
      have_entry = 1;
    }
    off += cs;
  }
  if (out->nsegs == 0 || out->nsegs > I1N1_KERNEL_MACHO_SEGS) BAD("segment count out of range");
  if (!have_entry) BAD("no LC_UNIXTHREAD entry");
  if (lowest != virt_base) BAD("lowest segment is not at boot-args virt_base");
  {
    unsigned s;
    int in_x = 0;
    for (s = 0; s < out->nsegs; ++s) {
      if ((out->seg[s].prot & 4u) && out->entry_va >= out->seg[s].va &&
          out->entry_va - out->seg[s].va < out->seg[s].vmsize)
        in_x = 1;
    }
    if (!in_x) BAD("entry point is not inside an executable segment");
  }
#undef BAD
  return 0;
}

/* ---- checkpoints ------------------------------------------------------------ */

static const char *const kCkptName[I1N1_CKPT_COUNT] = {"pre_stop_hvc", "cold_ret", "init_success",
                                                      "first_serial_marker"};

static int ckpt_push(struct kb *k, enum i1n1_ckpt_id id, uint64_t pc, uint64_t retired) {
  struct i1n1_ckpt_obs o;
  const struct i1n1_ckpt_def *d = i1n1_ckpt_def(id);
  o.id = id;
  o.pc = pc;
  o.retired = retired;
  o.blocks = retired; /* informational; excluded from the run digest */
  if ((unsigned)id != k->run.nobs ||
      (k->run.nobs > 0 && retired < k->run.obs[k->run.nobs - 1].retired)) {
    kstop(k, "checkpoint_order", pc, 0, retired, "kernel checkpoint out of order",
          I1N1_STOP_CHECKPOINT_ORDER, NULL);
    return -1;
  }
  k->run.obs[k->run.nobs++] = o;
  if (d->has_pc) {
    ksay(k, "I1N1:CHECKPOINT name=%s id=%u pc=0x%llx retired=%llu profile=kernel", kCkptName[id],
         (unsigned)id, (unsigned long long)pc, (unsigned long long)retired);
  } else {
    ksay(k, "I1N1:CHECKPOINT name=%s id=%u pc=unknown retired=%llu profile=kernel", kCkptName[id],
         (unsigned)id, (unsigned long long)retired);
  }
  k->res->checkpoints = k->run.nobs;
  return 0;
}

/* Prints newly observed checkpoints in id order. Returns -1 after a stop. */
static int ckpt_flush(struct kb *k, const struct i1n1_machine *m) {
  const struct i1n1_boot_hvc *g = &m->glue;
  if (k->run.nobs == 0 && g->init_completed) {
    if (g->hvc_pc != I1N1_CKPT_PRE_STOP_PC) {
      kstop(k, "checkpoint_order", g->hvc_pc, 0, g->hvc_retired,
            "INIT HVC is not at the baseline pc 0x803eb8350", I1N1_STOP_CHECKPOINT_ORDER, NULL);
      return -1;
    }
    if (ckpt_push(k, I1N1_CKPT_PRE_STOP, g->hvc_pc, g->hvc_retired) != 0) return -1;
  }
  if (k->run.nobs == 1 && g->ret_seen) {
    if (g->ret_pc != g->hvc_pc + I1N1_BOOT_RET_OFFSET_FROM_HVC) {
      kstop(k, "checkpoint_order", g->ret_pc, 0, g->ret_retired,
            "cold RET is not HVC pc + 0x48", I1N1_STOP_CHECKPOINT_ORDER, NULL);
      return -1;
    }
    if (ckpt_push(k, I1N1_CKPT_RET_TARGET, g->ret_pc, g->ret_retired) != 0) return -1;
  }
  if (k->run.nobs == 2 && g->init_success_seen) {
    /* INIT success: the provider committed exactly one INIT with keys derived. */
    if (g->apple.init_count != 1 || !g->apple.keys.initialized || g->apple.stopped) {
      kstop(k, "init_not_confirmed", g->ret_pc, 0, g->init_success_retired,
            "provider did not report a committed INIT at the cold RET",
            I1N1_STOP_CHECKPOINT_ORDER, NULL);
      return -1;
    }
    if (ckpt_push(k, I1N1_CKPT_INIT_SUCCESS, 0, g->init_success_retired) != 0) return -1;
  }
  if (k->run.nobs == 3 && g->serial_seen) {
    snprintf(k->run.serial_marker, sizeof k->run.serial_marker, "%s", g->serial_marker);
    snprintf(k->res->marker, sizeof k->res->marker, "%s", g->serial_marker);
    if (ckpt_push(k, I1N1_CKPT_FIRST_SERIAL, 0, g->serial_retired) != 0) return -1;
  }
  return 0;
}

/* ---- the run ---------------------------------------------------------------- */

static const char *fault_token(enum ocnext_a64_fault_code c) {
  switch (c) {
    case OCNEXT_A64_FAULT_UNSUPPORTED: return "unsupported_insn";
    case OCNEXT_A64_FAULT_FETCH: return "guest_fetch_fault";
    case OCNEXT_A64_FAULT_READ: return "guest_read_fault";
    case OCNEXT_A64_FAULT_WRITE: return "guest_write_fault";
    case OCNEXT_A64_FAULT_ALIGNMENT: return "guest_alignment_fault";
    case OCNEXT_A64_FAULT_DEBUG_HALT: return "guest_debug_halt";
    case OCNEXT_A64_FAULT_ARGUMENT: return "engine_argument";
    case OCNEXT_A64_FAULT_UNDEFINED: return "guest_undefined";
    case OCNEXT_A64_FAULT_SYSTEM_ACCESS: return "guest_system_access";
    case OCNEXT_A64_FAULT_EXCEPTION_RETURN: return "guest_exception_return";
    case OCNEXT_A64_FAULT_SYNC_PROVIDER: return "sync_provider";
    case OCNEXT_A64_FAULT_MAINTENANCE_PROVIDER: return "maintenance_provider";
    case OCNEXT_A64_FAULT_HVC_PROVIDER: return "hvc_provider";
    default: return "engine_stop";
  }
}

/* Maps whatever ended the run (router/sysreg stop, bus refusal, core fault) to one STOP line. */
static void stop_from_machine(struct kb *k, const struct i1n1_machine *m,
                              enum ocnext_a64_result r) {
  char d[112];
  if (m->glue.has_stop) {
    const struct i1n1_stop *s = &m->glue.stop;
    kstop(k, i1n1_stop_reason_name(s->reason), s->pc, s->opcode, s->retired, s->detail, s->reason, s);
    return;
  }
  if (m->bus.stop.kind != 0) {
    snprintf(d, sizeof d, "bus %s pa=0x%llx width=%u device=%s",
             m->bus.stop.is_write ? "write" : "read", (unsigned long long)m->bus.stop.pa,
             (unsigned)m->bus.stop.width, m->bus.stop.device);
    kstop(k, "mmio_fault", m->cpu.fault.pc, m->cpu.fault.opcode, m->cpu.retired, d,
          I1N1_STOP_UNSUPPORTED_INSN, NULL);
    return;
  }
  if (r == OCNEXT_A64_FAULTED) {
    snprintf(d, sizeof d, "%s addr=0x%llx", ocnext_a64_fault_string(m->cpu.fault.code),
             (unsigned long long)m->cpu.fault.address);
    kstop(k, fault_token(m->cpu.fault.code), m->cpu.fault.pc, m->cpu.fault.opcode, m->cpu.retired,
          d, I1N1_STOP_UNSUPPORTED_INSN, NULL);
    return;
  }
  snprintf(d, sizeof d, "machine returned %d", (int)r);
  kstop(k, r == OCNEXT_A64_HALTED ? "guest_halted" : "engine_stop", m->cpu.pc, m->cpu.last_opcode,
        m->cpu.retired, d, I1N1_STOP_UNSUPPORTED_INSN, NULL);
}

/* Continue mode: every UART line goes to the serial log as it arrives (late-panic diagnosis). */
static void uart_stream(void *ctx, uint64_t index, const char *text, unsigned len) {
  struct kb *k = ctx;
  (void)len;
  ksay(k, "I1N1:UART idx=%llu text=%s", (unsigned long long)index, text);
}

static const char *engine_name(uint32_t dbt) {
  return dbt == 2 ? "dbt-native" : dbt == 1 ? "dbt" : "interpreter";
}

int i1n1_efi_boot_kernel(const struct i1n1_efi_inputs *in, const struct i1n1_efi_console *con,
                         struct i1n1_efi_result *res) {
  struct kb *k = &g_kb;
  struct i1n1_kernel_macho mo;
  struct i1n1_adt_stats ds;
  struct i1n1_machine_cfg mc;
  uint8_t seed32[32];
  uint8_t args[BOOT_ARGS_BYTES];
  uint8_t final_args_sha[32], dt_final_sha[32];
  uint64_t virt, phys, memsz, top, dtva, args_pa, dt_pa, entry_pa;
  uint64_t t_start, t_end;
  uint64_t max_dispatches = in->cfg.max_steps;
  size_t cmd_n;
  long final_dt;
  char why[96], hex[65], cmdline_tag[BOOT_ARGS_CMDLINE_MAX + 1], cmdline_eff[BOOT_ARGS_CMDLINE_MAX];
  struct i1n1_video_plan vp;
  int have_video = 0;
  enum ocnext_a64_result r = OCNEXT_A64_BUDGET;
  unsigned i;
  uint8_t *ram = in->ram;

  memset(k, 0, sizeof *k);
  k->con = con;
  k->res = res;

  if (!in->bootargs_bin || in->bootargs_bin_bytes != BOOT_ARGS_BYTES)
    return kinput(k, "input_missing", "kernel profile needs boot-args.bin of exactly 1152 bytes");
  if (!in->cmdline || in->cmdline_bytes == 0)
    return kinput(k, "input_missing", "kernel profile needs a cmdline (bootargs.txt)");
  if (!in->seed)
    return kinput(k, "keys_not_configured", "kernel profile needs keyseed.bin (explicit research seed)");
  memcpy(seed32, in->seed, 32);

  /* ---- input digests (receipt fields) ---- */
  if (in->efi_image && in->efi_bytes) {
    i1n1_sha256(in->efi_image, in->efi_bytes, k->run.efi_sha256);
    i1n1_hex32(k->run.efi_sha256, hex);
    ksay(k, "I1N1:INPUT name=efi bytes=%llu sha256=%s", (unsigned long long)in->efi_bytes, hex);
  }
  i1n1_sha256(in->guest, in->guest_bytes, k->run.image_sha256);
  i1n1_hex32(k->run.image_sha256, hex);
  ksay(k, "I1N1:INPUT name=guest bytes=%llu sha256=%s", (unsigned long long)in->guest_bytes, hex);
  {
    uint8_t dtraw[32];
    i1n1_sha256(in->dt, in->dt_bytes, dtraw);
    i1n1_hex32(dtraw, hex);
    ksay(k, "I1N1:INPUT name=dt bytes=%llu sha256=%s", (unsigned long long)in->dt_bytes, hex);
    memcpy(k->run.dt_sha256, dtraw, 32); /* replaced by the finalized digest below */
  }
  memcpy(args, in->bootargs_bin, BOOT_ARGS_BYTES);
  {
    uint8_t as[32];
    i1n1_sha256(args, BOOT_ARGS_BYTES, as);
    i1n1_hex32(as, hex);
    ksay(k, "I1N1:INPUT name=boot-args bytes=%u sha256=%s", BOOT_ARGS_BYTES, hex);
  }

  /* ---- geometry from boot-args.bin ---- */
  if (le(args, 2) != 3u || le(args + 2, 2) != 2u)
    return kinput(k, "bootargs_invalid", "boot-args revision/version is not 3/2");
  virt = le(args + 8, 8);
  phys = le(args + 16, 8);
  memsz = le(args + 24, 8);
  top = le(args + 32, 8);
  dtva = le(args + 96, 8);
  if ((virt | phys | memsz | top) & (GRANULE - 1u))
    return kinput(k, "bootargs_invalid", "boot-args bases are not 16 KiB aligned");
  if (memsz != in->cfg.ram_bytes)
    return kinput(k, "config_invalid", "cfg ram_mib must equal boot-args mem_size");
  if (le(args + 1144, 8) != memsz)
    return kinput(k, "bootargs_invalid", "boot-args mem_size_actual differs from mem_size");
  /* Range-check phys/memsz first: every later phys+X / dt_pa+X sum stays below 2^48 and cannot wrap. */
  if (memsz == 0 || memsz > (UINT64_C(1) << 48) || phys > (UINT64_C(1) << 48) - memsz)
    return kinput(k, "bootargs_invalid", "boot-args physical base/mem_size outside the 48-bit physical range");
  if (top < phys + BOOT_ARGS_PAGE || top - phys > memsz || dtva < virt || (dtva - virt) >= memsz)
    return kinput(k, "bootargs_invalid", "boot-args top/DT address outside RAM");
  args_pa = top - BOOT_ARGS_PAGE;
  dt_pa = phys + (dtva - virt);
  if ((dt_pa & (GRANULE - 1u)) || le(args + 104, 4) != in->dt_bytes)
    return kinput(k, "bootargs_invalid", "boot-args DT length differs from DeviceTree.adt");
  if (in->dt_bytes > DT_MAX_BYTES || dt_pa + DT_MAX_BYTES > args_pa)
    return kinput(k, "bootargs_invalid", "no room for the finalized device tree below the boot-args page");

  if (i1n1_kernel_macho_check(in->guest, in->guest_bytes, virt, &mo, why, sizeof why) != 0)
    return kinput(k, "image_unsupported", why);
  if (mo.extent > dt_pa - phys || in->guest_bytes > dt_pa - phys)
    return kinput(k, "image_unsupported", "kernel image overlaps the device tree");
  entry_pa = phys + (mo.entry_va - virt);
  cmd_n = i1n1_cmdline_compose((enum i1n1_cmdline_mode)in->cfg.cmdline_mode, in->cmdline, in->cmdline_bytes,
                               in->cfg.cmdline_raw, cmdline_eff, sizeof cmdline_eff);
  if (cmd_n == 0 || cmd_n >= BOOT_ARGS_CMDLINE_MAX)
    return kinput(k, "bootargs_invalid", "cmdline empty or longer than 1023 bytes");
  for (i = 0; i < cmd_n; ++i) {
    unsigned char c = (unsigned char)cmdline_eff[i];
    if (c < 0x20 || c > 0x7e) return kinput(k, "bootargs_invalid", "cmdline is not printable ASCII");
  }
  if (!ram || ((uintptr_t)ram & 0xfffu))
    return kinput(k, "ram_unaligned", "guest RAM backing must be 4 KiB aligned");

  /* ---- lay out guest RAM (the host copy is the only thing modified) ---- */
  memcpy(ram, in->guest, in->guest_bytes);
  ksay(k, "I1N1:LOAD image=ok kind=macho-physical-layout filetype=0x%x segs=%u entry_va=0x%llx "
          "entry_pa=0x%llx extent=0x%llx fixups=none load_pa=0x%llx virt_base=0x%llx",
       (unsigned)mo.filetype, mo.nsegs, (unsigned long long)mo.entry_va,
       (unsigned long long)entry_pa, (unsigned long long)mo.extent, (unsigned long long)phys,
       (unsigned long long)virt);

  /* Device tree finalization exactly as src/guest/adt_finalize.c / the engine fixture. */
  memcpy(g_dt_scratch, in->dt, in->dt_bytes);
  if (i1n1_boot_dt_policy_init(&g_policy, phys, memsz) != 0 ||
      (in->cfg.nvram && i1n1_boot_dt_policy_enable_nvram(&g_policy) != 0))
    return kinput(k, "config_invalid", "device-tree policy table is full");
  if (in->cfg.video_mode == I1N1_VIDEO_STUB) {
    /* Stub framebuffer in guest RAM directly above the boot-args page; reserved in boot_args
     * (topOfKernelData) and in the ADT (/chosen/memory-map MemoryMapReserved-0). No host display. */
    const char *vwhy = "";
    uint32_t vw = in->cfg.video_w ? in->cfg.video_w : I1N1_VIDEO_DEFAULT_W;
    uint32_t vh = in->cfg.video_h ? in->cfg.video_h : I1N1_VIDEO_DEFAULT_H;
    if (i1n1_video_plan(vw, vh, phys, memsz, phys + mo.extent, dt_pa, DT_MAX_BYTES, args_pa, top, &vp, &vwhy) != 0)
      return kinput(k, "bootargs_invalid", vwhy);
    {
      /* Same layout validation the owned-RAM layer applies: no overlap, in range. */
      struct i1n1_ram_region rg[4];
      struct i1n1_ram chk;
      uint64_t kb = (mo.extent + GRANULE - 1) & ~(uint64_t)(GRANULE - 1);
      rg[0].guest_base = phys;          rg[0].bytes = kb;                rg[0].backing = ram;
      rg[1].guest_base = dt_pa;         rg[1].bytes = DT_MAX_BYTES;      rg[1].backing = ram + (dt_pa - phys);
      rg[2].guest_base = args_pa;       rg[2].bytes = BOOT_ARGS_PAGE;    rg[2].backing = ram + (args_pa - phys);
      rg[3].guest_base = vp.pa;         rg[3].bytes = vp.reserved;       rg[3].backing = ram + (vp.pa - phys);
      for (i = 0; i < 4; ++i) rg[i].perm = I1N1_RAM_PERM_ALL;
      if (i1n1_ram_init(&chk, rg, 4) != I1N1_RAM_OK)
        return kinput(k, "bootargs_invalid", "stub framebuffer layout rejected by the owned-RAM validator");
    }
    if (i1n1_video_reserve_dt(&g_policy, &vp) != 0)
      return kinput(k, "dt_invalid", "no room in the DT policy for the framebuffer reservation");
    have_video = 1;
    top = vp.top_of_kernel_data;
  }
  final_dt = i1n1_adt_finalize(g_dt_scratch, in->dt_bytes, ram + (dt_pa - phys),
                               (size_t)(args_pa - dt_pa), g_policy.sets, g_policy.nsets, &ds);
  if (final_dt < 0) return kinput(k, "dt_invalid", "device tree finalization failed");
  /* The raw tree carries length placeholders (bit 31) the strict validator rejects by design;
   * the finalized tree must validate. */
  if (i1n1_dt_validate(ram + (dt_pa - phys), (size_t)final_dt, NULL) != 0)
    return kinput(k, "dt_invalid", "finalized device tree failed validation");
  i1n1_sha256(ram + (dt_pa - phys), (size_t)final_dt, dt_final_sha);
  memcpy(k->run.dt_sha256, dt_final_sha, 32);
  i1n1_hex32(dt_final_sha, hex);
  ksay(k, "I1N1:DT raw_bytes=%llu final_bytes=%ld nodes=%u props=%u placeholders_zeroed=%u deleted=%u "
          "set_replaced=%u added=%u final_sha256=%s",
       (unsigned long long)in->dt_bytes, final_dt, ds.nodes, ds.props, ds.resolved_zero, ds.deleted,
       ds.set_replaced, ds.set_added, hex);

  /* boot_args: deviceTreeLength follows the finalized tree, cmdline from the ESP. */
  put_le(args + 104, (uint64_t)final_dt, 4);
  memset(args + BOOT_ARGS_CMDLINE_OFF, 0, BOOT_ARGS_CMDLINE_MAX);
  memcpy(args + BOOT_ARGS_CMDLINE_OFF, cmdline_eff, cmd_n);
  if (have_video) {
    i1n1_video_apply_bootargs(args, &vp);
    if (i1n1_bootargs_validate(args, BOOT_ARGS_BYTES) != 0)
      return kinput(k, "bootargs_invalid", "boot-args block with stub Video failed validation");
    ksay(k, "I1N1:VIDEO mode=stub pa=0x%llx width=%llu height=%llu stride=%llu depth=%llu bytes=%llu "
            "reserved=0x%llx top=0x%llx format=bgrx8888-assumed storage=guest-ram-no-display",
         (unsigned long long)vp.pa, (unsigned long long)vp.width, (unsigned long long)vp.height,
         (unsigned long long)vp.stride, (unsigned long long)vp.depth, (unsigned long long)vp.bytes,
         (unsigned long long)vp.reserved, (unsigned long long)vp.top_of_kernel_data);
  }
  memcpy(ram + (args_pa - phys), args, BOOT_ARGS_BYTES);
  i1n1_sha256(args, BOOT_ARGS_BYTES, final_args_sha);
  memcpy(k->run.boot_args_sha256, final_args_sha, 32);
  i1n1_hex32(final_args_sha, hex);
  ksay(k, "I1N1:BOOTARGS pa=0x%llx dt_pa=0x%llx dt_bytes=%ld top=0x%llx sha256=%s",
       (unsigned long long)args_pa, (unsigned long long)dt_pa, final_dt, (unsigned long long)top, hex);

  /* ---- machine ---- */
  memset(&mc, 0, sizeof mc);
  mc.ram = ram;
  mc.ram_base = phys;
  mc.ram_size = memsz;
  mc.entry_pc = entry_pa;
  mc.boot_args_pa = args_pa;
  mc.uart_base = K_UART_BASE;
  mc.gicd_base = K_GICD_BASE;
  mc.gicr_base = K_GICR_BASE;
  mc.research_seed = seed32;
  mc.continue_mode = in->cfg.cont ? 1 : 0;
  if (i1n1_machine_init(&g_machine, &mc) != 0)
    return kinput(k, "machine_init_failed", "boot machine rejected its configuration");
  if (in->cfg.dbt != 0) {
    /* No silent downgrade: a build that cannot run the requested engine stops. */
    if (i1n1_machine_dbt_enable(&g_machine, (int)in->cfg.dbt, 2u, in->native_arena, in->native_arena_bytes) != 0)
      return kinput(k, "dbt_unavailable",
                    in->cfg.dbt == 2 ? "dbt=2 needs native x86_64 execution (x86_64 build and an executable arena)"
                                     : "DBT engine could not be initialised");
  }
  ksay(k, "I1N1:DEVICE name=pl011 base=0x%llx size=0x10000", (unsigned long long)K_UART_BASE);
  ksay(k, "I1N1:DEVICE name=gicv3-lite gicd=0x%llx gicr=0x%llx", (unsigned long long)K_GICD_BASE,
       (unsigned long long)K_GICR_BASE);
  ksay(k, "I1N1:PROVIDER name=apple_init fid=0xc1000000 profile=research-software-keys seed=esp-file");
  if (in->cfg.cont) {
    k->run.cont.enabled = 1;
    g_machine.line_sink = uart_stream;
    g_machine.line_sink_ctx = k;
    ksay(k, "I1N1:RUN start pc=0x%llx x0=0x%llx el=1 profile=kernel engine=%s max_steps=%llu continue_after_serial=1",
         (unsigned long long)g_machine.cpu.pc, (unsigned long long)g_machine.cpu.x[0], engine_name(in->cfg.dbt),
         (unsigned long long)max_dispatches);
  } else {
    ksay(k, "I1N1:RUN start pc=0x%llx x0=0x%llx el=1 profile=kernel engine=%s max_steps=%llu",
         (unsigned long long)g_machine.cpu.pc, (unsigned long long)g_machine.cpu.x[0], engine_name(in->cfg.dbt),
         (unsigned long long)max_dispatches);
  }
  ksay(k, "I1N1:ENGINE name=%s dbt=%u native=%u", engine_name(in->cfg.dbt), in->cfg.dbt, in->cfg.dbt == 2 ? 1u : 0u);

  t_start = kms(k);
  while (!k->stopped) {
    uint64_t limit = g_machine.dispatches + CHUNK_DISPATCHES;
    if (limit > max_dispatches) limit = max_dispatches;
    r = i1n1_machine_run(&g_machine, limit);
    if (ckpt_flush(k, &g_machine) != 0) break;
    if (in->cfg.cont && !k->run.cont.entered && g_machine.glue.serial_seen) {
      /* optional event checkpoint after the four M-BOOT ones; deterministic fields only (no pc) */
      k->run.cont.entered = 1;
      k->run.cont.entered_retired = g_machine.glue.serial_retired;
      ksay(k, "I1N1:CHECKPOINT name=post_banner_continue_entered id=%u pc=unknown retired=%llu profile=kernel",
           (unsigned)I1N1_CKPT_COUNT, (unsigned long long)k->run.cont.entered_retired);
    }
    if (g_machine.done && in->cfg.cont) {
      /* AVP panic first (exact event), then the other continue-mode halts */
      const struct i1n1_event *pe = i1n1_event_log_first_panic(&g_machine.avp.events);
      if (pe != NULL) {
        i1n1_run_record_guest_panic(&k->run, pe->pc, pe->retired, pe->pa, pe->value);
        kstop(k, "guest_panic", pe->pc, 0, pe->retired, "guest wrote PANICKED to pvpanic",
              I1N1_STOP_GUEST_PANIC, NULL);
      } else if (g_machine.halt == I1N1_HALT_PANIC_LINE) {
        char d[96];
        i1n1_run_record_guest_panic(&k->run, 0, g_machine.panic_line_retired, K_UART_BASE, 0);
        k->run.guest_panic.uart_text = 1;
        k->run.guest_panic.line_index = g_machine.panic_line_index;
        snprintf(d, sizeof d, "UART line %llu contains 'panic' (text in the serial log)",
                 (unsigned long long)g_machine.panic_line_index);
        kstop(k, "guest_panic", 0, 0, g_machine.panic_line_retired, d, I1N1_STOP_GUEST_PANIC, NULL);
      } else if (g_machine.halt == I1N1_HALT_POWER_OFF) {
        kstop(k, "guest_power_off", g_machine.glue.power_off_pc, 0, g_machine.glue.power_off_retired,
              "guest PSCI SYSTEM_OFF", I1N1_STOP_GUEST_POWER_OFF, NULL);
      } else if (r != OCNEXT_A64_BUDGET) {
        stop_from_machine(k, &g_machine, r);
      } else {
        kstop(k, "engine_stop", g_machine.cpu.pc, 0, g_machine.cpu.retired,
              "continue-mode halt without a cause", I1N1_STOP_UNSUPPORTED_INSN, NULL);
      }
      break;
    }
    if (g_machine.done) break;
    {
      const struct i1n1_event *pe = i1n1_event_log_first_panic(&g_machine.avp.events);
      if (pe != NULL) { /* optional checkpoint guest_panic_event (pvpanic write) */
        i1n1_run_record_guest_panic(&k->run, pe->pc, pe->retired, pe->pa, pe->value);
        kstop(k, "guest_panic", pe->pc, 0, pe->retired, "guest wrote PANICKED to pvpanic",
              I1N1_STOP_GUEST_PANIC, NULL);
        break;
      }
    }
    if (r != OCNEXT_A64_BUDGET) {
      stop_from_machine(k, &g_machine, r);
      break;
    }
    if (g_machine.dispatches >= max_dispatches) {
      kstop(k, "step_budget", g_machine.cpu.pc, 0, g_machine.cpu.retired,
            in->cfg.cont ? "max_steps exhausted in continue mode"
                         : "max_steps exhausted before all checkpoints", I1N1_STOP_UNSUPPORTED_INSN, NULL);
      break;
    }
    ksay(k, "I1N1:PROGRESS dispatches=%llu retired=%llu pc=0x%llx elapsed_ms=%llu",
         (unsigned long long)g_machine.dispatches, (unsigned long long)g_machine.cpu.retired,
         (unsigned long long)g_machine.cpu.pc, (unsigned long long)(kms(k) - t_start));
  }
  t_end = kms(k);

  res->retired = g_machine.cpu.retired;
  res->blocks = g_machine.dispatches;
  res->pc = g_machine.cpu.pc;
  k->run.blocks = g_machine.dispatches;
  k->run.instructions = g_machine.cpu.retired;
  k->run.elapsed_ms = t_end - t_start;

  /* Guest-side facts, deterministic: these lines enter the receipt's guest digest. */
  ksay(k, "OCNEXT:TARGET_RESULT result=%d retired=%llu dispatches=%llu pc=0x%llx fault=%d", (int)r,
       (unsigned long long)g_machine.cpu.retired, (unsigned long long)g_machine.dispatches,
       (unsigned long long)g_machine.cpu.pc, r == OCNEXT_A64_FAULTED ? 1 : 0);
  if (g_machine.glue.serial_seen) {
    i1n1_hex32(g_machine.serial_sha, hex);
    ksay(k, "OCNEXT:SERIAL_MARKER len=%lu sha256=%s", g_machine.serial_len, hex);
  }
  if (in->cfg.cont) {
    uint8_t us[32];
    struct apple_init_sha256 c = g_machine.uart_sha;
    apple_init_sha256_final(&c, us);
    i1n1_hex32(us, hex);
    ksay(k, "OCNEXT:UART_STREAM lines=%llu sha256=%s", (unsigned long long)g_machine.uart.lines, hex);
    if (g_machine.panic_line_seen) {
      uint8_t ph[32];
      i1n1_sha256(g_machine.panic_line, g_machine.panic_line_len, ph);
      i1n1_hex32(ph, hex);
      ksay(k, "OCNEXT:PANIC_LINE idx=%llu len=%u retired=%llu sha256=%s", (unsigned long long)g_machine.panic_line_index,
           g_machine.panic_line_len, (unsigned long long)g_machine.panic_line_retired, hex);
      ksay(k, "I1N1:PANICLINE idx=%llu text=%s", (unsigned long long)g_machine.panic_line_index, g_machine.panic_line);
    }
  }
  for (i = 0; i < I1N1_MACHINE_LOG_LINES && i < g_machine.uart.lines; ++i) {
    char t[I1N1_PL011_LINE_MAX];
    sanitize(t, sizeof t, g_machine.log[i]);
    ksay(k, "I1N1:GUESTLOG idx=%u len=%u text=\"%.200s\"", i, g_machine.log_len[i], t);
  }
  ksay(k, "I1N1:MACHINE uart_bytes=%llu uart_lines=%llu mmio_reads=%llu mmio_writes=%llu fiq=%llu irq=%llu "
          "hvc_dispatches=%llu",
       (unsigned long long)g_machine.uart.tx_bytes, (unsigned long long)g_machine.uart.lines,
       (unsigned long long)g_machine.bus.mmio_reads, (unsigned long long)g_machine.bus.mmio_writes,
       (unsigned long long)g_machine.fiqs_taken, (unsigned long long)g_machine.irqs_taken,
       (unsigned long long)g_machine.glue.router.dispatches);
  if (have_video) {
    uint8_t fbd[32];
    char fbhex[65];
    uint64_t nz;
    i1n1_video_fingerprint(ram + (vp.pa - phys), &vp, fbd, &nz);
    i1n1_hex32(fbd, fbhex);
    ksay(k, "I1N1:FB pa=0x%llx bytes=%llu nonzero_bytes=%llu sha256=%s", (unsigned long long)vp.pa,
         (unsigned long long)vp.bytes, (unsigned long long)nz, fbhex);
  }
  {
    /* Guest-visible state digest (CPU, banks, glue, devices; not RAM): the DBT must equal the interpreter. */
    uint8_t sd[32];
    char sdhex[65];
    i1n1_machine_state_digest(&g_machine, 0, sd);
    i1n1_hex32(sd, sdhex);
    ksay(k, "I1N1:STATE engine=%s dispatches=%llu retired=%llu pc=0x%llx state_sha256=%s", engine_name(in->cfg.dbt),
         (unsigned long long)g_machine.dispatches, (unsigned long long)g_machine.cpu.retired,
         (unsigned long long)g_machine.cpu.pc, sdhex);
  }
  if (in->cfg.dbt != 0) {
    struct i1n1_machine_dbt_stats st;
    i1n1_machine_dbt_stats(&g_machine, &st);
    ksay(k, "I1N1:DBT mode=%u translations=%llu lookups=%llu hits=%llu chained_hops=%llu revalidations=%llu "
            "revalidation_drops=%llu smc_invalidations=%llu flushes=%llu ic_flushes=%llu regime_changes=%llu "
            "tlbi=%llu host_steps=%llu native_steps=%llu native_callouts=%llu native_compiles=%llu",
         in->cfg.dbt, (unsigned long long)st.translations, (unsigned long long)st.lookups,
         (unsigned long long)st.hits, (unsigned long long)st.chained_hops, (unsigned long long)st.revalidations,
         (unsigned long long)st.revalidation_drops, (unsigned long long)st.smc_invalidations,
         (unsigned long long)st.flushes, (unsigned long long)st.ic_flushes, (unsigned long long)st.regime_changes,
         (unsigned long long)st.tlbi_seen, (unsigned long long)st.host_steps, (unsigned long long)st.native_steps,
         (unsigned long long)st.native_callouts, (unsigned long long)st.native_compiles);
  }
  {
    uint64_t ms = t_end - t_start;
    ksay(k, "I1N1:PERF engine=%s dispatches=%llu retired=%llu elapsed_ms=%llu kinsn_per_s=%llu",
         engine_name(in->cfg.dbt), (unsigned long long)g_machine.dispatches, (unsigned long long)g_machine.cpu.retired,
         (unsigned long long)ms, (unsigned long long)(ms ? g_machine.cpu.retired / ms : 0));
  }

  /* Receipt context (build.boot): everything that must be identical across runs. */
  {
    uint8_t td[32];
    char tdhex[65], mhex[65];
    for (i = 0; i < cmd_n && i < BOOT_ARGS_CMDLINE_MAX; ++i)
      cmdline_tag[i] = cmdline_eff[i] == ' ' ? '_' : cmdline_eff[i];
    cmdline_tag[i] = 0;
    apple_init_trace_digest(&g_machine.glue.apple.trace, td);
    i1n1_hex32(td, tdhex);
    i1n1_hex32(g_machine.serial_sha, mhex);
    {
      uint8_t rawdt[32], argsraw[32];
      char rhex[65], ahex[65], fhex[65];
      i1n1_sha256(in->dt, in->dt_bytes, rawdt);
      i1n1_sha256(in->bootargs_bin, BOOT_ARGS_BYTES, argsraw);
      i1n1_hex32(rawdt, rhex);
      i1n1_hex32(final_args_sha, ahex);
      i1n1_hex32(dt_final_sha, fhex);
      ksay(k, "I1N1:BOOTCTX profile=research-software-keys+apple-vm-research-v1 cmdline=%s "
              "args_sha256=%s dt_raw_sha256=%s dt_final_sha256=%s dt_final_bytes=%ld "
              "provider_trace_digest=%s hvc_dispatches=%llu serial_marker_len=%lu "
              "serial_marker_sha256=%s budget=%llu%s",
           cmdline_tag, ahex, rhex, fhex, final_dt, tdhex,
           (unsigned long long)g_machine.glue.router.dispatches, g_machine.serial_len, mhex,
           (unsigned long long)max_dispatches, in->cfg.cont ? " continue_after_serial=1" : "");
    }
  }

  if (in->cfg.cont) {
    /* Continue mode never passes: the outcome is one of stopped / guest_panic / budget_exhausted /
     * guest_power_off and every outcome has a recorded stop (docs/contracts/boot-efi.md 9.6). */
    uint8_t dg[32];
    static char cjson[4096];
    size_t cn;
    if (!k->stopped)
      kstop(k, "incomplete", g_machine.cpu.pc, 0, g_machine.cpu.retired,
            "continue-mode run ended without an outcome", I1N1_STOP_CHECKPOINT_ORDER, NULL);
    k->run.cont.outcome = strcmp(res->reason, "guest_panic") == 0     ? I1N1_CONT_GUEST_PANIC
                          : strcmp(res->reason, "guest_power_off") == 0 ? I1N1_CONT_POWER_OFF
                          : strcmp(res->reason, "step_budget") == 0     ? I1N1_CONT_BUDGET_EXHAUSTED
                                                                          : I1N1_CONT_STOPPED;
    i1n1_run_digest(&k->run, dg);
    i1n1_hex32(dg, hex);
    cn = i1n1_run_receipt_json(&k->run, cjson, sizeof cjson);
    if (cn) {
      while (cn && (cjson[cn - 1] == '\n' || cjson[cn - 1] == '\r')) cjson[--cn] = 0;
      kemit(k, "I1N1:RECEIPT ", 13);
      kemit(k, cjson, cn);
      kemit(k, "\n", 1);
    }
    ksay(k, "I1N1:RESULT status=%s continue=1 checkpoints=%u entered=%u reason=%s retired=%llu blocks=%llu run_digest=%s",
         i1n1_cont_outcome_name(k->run.cont.outcome), k->run.nobs, (unsigned)k->run.cont.entered, res->reason,
         (unsigned long long)res->retired, (unsigned long long)res->blocks, hex);
  } else if (!k->stopped && i1n1_run_passed(&k->run)) {
    uint8_t dg[32];
    static char json[2048];
    size_t n;
    res->passed = 1;
    res->exit_code = I1N1_EFI_EXIT_PASSED;
    i1n1_run_digest(&k->run, dg);
    i1n1_hex32(dg, hex);
    ksay(k, "I1N1:RESULT status=passed checkpoints=%u retired=%llu blocks=%llu run_digest=%s",
         k->run.nobs, (unsigned long long)res->retired, (unsigned long long)res->blocks, hex);
    n = i1n1_run_receipt_json(&k->run, json, sizeof json);
    if (n) {
      while (n && (json[n - 1] == '\n' || json[n - 1] == '\r')) json[--n] = 0;
      kemit(k, "I1N1:RECEIPT ", 13);
      kemit(k, json, n);
      kemit(k, "\n", 1);
    }
  } else {
    uint8_t dg[32];
    if (!k->stopped)
      kstop(k, "incomplete", g_machine.cpu.pc, 0, g_machine.cpu.retired,
            "run ended before all checkpoints were observed", I1N1_STOP_CHECKPOINT_ORDER, NULL);
    i1n1_run_digest(&k->run, dg);
    i1n1_hex32(dg, hex);
    if (k->run.guest_panic.present) { /* the receipt carries guest_panic=true even though the run did not pass */
      static char pjson[2048];
      size_t pn = i1n1_run_receipt_json(&k->run, pjson, sizeof pjson);
      if (pn) {
        while (pn && (pjson[pn - 1] == '\n' || pjson[pn - 1] == '\r')) pjson[--pn] = 0;
        kemit(k, "I1N1:RECEIPT ", 13);
        kemit(k, pjson, pn);
        kemit(k, "\n", 1);
      }
    }
    ksay(k, "I1N1:RESULT status=stopped checkpoints=%u reason=%s retired=%llu blocks=%llu run_digest=%s",
         k->run.nobs, res->reason, (unsigned long long)res->retired,
         (unsigned long long)res->blocks, hex);
  }
  res->checkpoints = k->run.nobs;
  return res->exit_code;
}

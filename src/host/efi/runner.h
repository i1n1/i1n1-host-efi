/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * i1n1.efi boot runner (WS1/WS4 glue). Everything between "inputs are in
 * memory" and "guest finished or stopped" lives here, with no firmware
 * dependency, so the same code runs inside the freestanding EFI image and in
 * host unit tests (tests/integration/efi_runner_test.c).
 *
 * Pipeline: validate inputs -> WS4 Mach-O load, DT, boot args, stage-1,
 * PE state -> WS3 owned RAM + EPT + MMIO router (PL011-lite, GICv3-lite) ->
 * WS2 DBT over the engine A64 core -> WS4 HVC router -> WS8 apple_init
 * provider. Every unsupported event is a deterministic stop printed as a
 * machine-readable `I1N1:STOP reason=... pc=...` serial line; checkpoints are
 * printed as `I1N1:CHECKPOINT name=...`. Nothing is skipped or faked.
 *
 * Serial grammar (one record per line, `key=value` tokens, values contain no
 * spaces except the quoted detail=):
 *   I1N1:<TAG> k=v k=v ...
 * Lines the guest writes to its emulated UART are forwarded verbatim.
 */
#ifndef I1N1_EFI_RUNNER_H
#define I1N1_EFI_RUNNER_H

#include <stddef.h>
#include <stdint.h>

#include "plat_phase.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I1N1_EFI_RUNNER_VERSION 1u

/* Guest-visible machine map of the synthetic/kernel profiles (flat, MMU off
 * in the DBT; PA == VA). The RAM base is configurable, devices are fixed. */
#define I1N1_EFI_GICD_BASE UINT64_C(0x08000000)
#define I1N1_EFI_GICR_BASE UINT64_C(0x080a0000)
#define I1N1_EFI_UART_BASE UINT64_C(0x09000000)
#define I1N1_EFI_UART_SIZE UINT64_C(0x1000)
#define I1N1_EFI_DEFAULT_RAM_BASE UINT64_C(0x800000000)

/* The PSCI function id the synthetic profile accepts for shutdown. */
#define I1N1_EFI_PSCI_SYSTEM_OFF UINT64_C(0x84000008)

enum i1n1_efi_profile {
  I1N1_EFI_PROFILE_NONE = 0,
  I1N1_EFI_PROFILE_SYNTHETIC = 1, /* repo-built A64 self-test blob */
  I1N1_EFI_PROFILE_KERNEL = 2     /* real kernel image: exact baseline PCs */
};

struct i1n1_efi_config {
  enum i1n1_efi_profile profile; /* required; no default */
  uint64_t ram_base;
  uint64_t ram_bytes;
  uint64_t max_steps;            /* engine-step budget before a stop */
  uint32_t proxy;                /* 1: serve the COM2 debug link afterwards */
  uint32_t proxy_wait_s;         /* seconds to wait for the first request */
  uint32_t dbt;                  /* kernel profile engine: 0 interpreter, 1 DBT (TB cache, hooked steps),
                                    2 DBT + native x86_64 blocks. Equivalence: boot-efi.md section 9 */
  uint32_t cont;                 /* kernel profile: continue_after_serial=1 keeps running past the first serial marker
                                    (boot-efi.md section 9.6); default 0 = the four M-BOOT checkpoints */
  uint32_t nvram;                /* kernel profile: 1 adds the synthetic /chosen NVRAM DT data (default 0) */
  /* Kernel-profile boot-args policy (src/guest/boot_policy.h). Defaults keep the historical
   * behaviour: Video zero, cmdline = bootargs.txt. */
  uint32_t video_mode;           /* enum i1n1_video_mode: 0 none, 1 stub framebuffer in guest RAM */
  uint32_t video_w, video_h;     /* stub geometry (0 = default 640x480) */
  uint32_t cmdline_mode;         /* enum i1n1_cmdline_mode: 0 file, 1 quiet, 2 raw */
  char cmdline_raw[240];         /* cmdline=raw:<text> */
};

/* Parses `key=value` lines ('#' comments). Keys: profile=synthetic|kernel,
 * ram_base=<hex|dec>, ram_mib=<dec>, max_steps=<dec>, proxy=0|1,
 * proxy_wait_s=<dec>, dbt=0|1|2, nvram=0|1, continue_after_serial=0|1, video=none|stub, video_size=WxH,
 * cmdline=file|quiet|raw:<text> (nvram, continue_after_serial and video/cmdline: kernel profile only). Unknown keys and a missing profile are errors.
 * Returns 0, or -1 with a short reason in err. */
int i1n1_efi_config_parse(const char *text, size_t len,
                          struct i1n1_efi_config *cfg, char *err,
                          size_t err_cap);

struct i1n1_efi_console {
  void *ctx;
  void (*write)(void *ctx, const char *s, size_t n);
  /* Optional coarse monotonic clock (ms) for progress/perf lines; may be NULL. */
  uint64_t (*now_ms)(void *ctx);
};

struct i1n1_efi_inputs {
  struct i1n1_efi_config cfg;
  const uint8_t *efi_image;
  size_t efi_bytes;          /* hashed into the receipt (0 allowed in tests) */
  const uint8_t *guest;
  size_t guest_bytes;
  const uint8_t *dt;
  size_t dt_bytes;
  const char *cmdline;
  size_t cmdline_bytes;
  const uint8_t *seed;       /* 32-byte APPLE INIT research seed, or NULL */
  const uint8_t *bootargs_bin; /* kernel profile: the 1152-byte boot-args page */
  size_t bootargs_bin_bytes;
  uint8_t *ram;              /* host backing for cfg.ram_bytes (zeroed) */
  /* Executable memory for native DBT blocks (cfg.dbt == 2) on firmware builds
   * (EfiLoaderCode pages); NULL on hosted builds, which map their own. */
  void *native_arena;
  size_t native_arena_bytes;
};

enum i1n1_efi_exit {
  I1N1_EFI_EXIT_PASSED = 0,
  I1N1_EFI_EXIT_STOP = 43,   /* deterministic stop (docs/contracts/boot.md) */
  I1N1_EFI_EXIT_INPUT = 2    /* inputs/config rejected before the guest ran */
};

struct i1n1_efi_result {
  int exit_code;             /* enum i1n1_efi_exit */
  int passed;                /* all four checkpoints, no stop, marker seen */
  char reason[48];           /* stop reason token, empty when passed */
  uint64_t pc, retired, blocks;
  unsigned checkpoints;      /* how many were observed */
  char marker[96];           /* first guest serial line after INIT */
};

/* Runs the whole pipeline. `adapter` must already be in the post-EBS phase
 * (the runner gates on it, per platform phase rules). Output goes to `con`. */
int i1n1_efi_boot_guest(const struct i1n1_efi_inputs *in,
                        const struct i1n1_efi_console *con,
                        struct plat_phase_tracker *adapter,
                        struct i1n1_efi_result *res);

/* ---- kernel profile (kernel_boot.c) --------------------------------------- */

#define I1N1_KERNEL_MACHO_SEGS 16u

struct i1n1_kernel_macho {
  uint32_t filetype;
  uint32_t nsegs;            /* LC_SEGMENT_64 count (seg[] holds the first 16) */
  uint64_t entry_va;         /* LC_UNIXTHREAD pc */
  uint64_t extent;           /* highest vmaddr+vmsize relative to virt_base */
  struct {
    uint64_t va, vmsize;
    uint32_t prot;
  } seg[I1N1_KERNEL_MACHO_SEGS];
};

/* Verifies the physical-layout arm64 Mach-O the kernel profile runs: every
 * segment with file contents has fileoff == vmaddr - virt_base, no W+X, the
 * lowest segment is at virt_base, LC_UNIXTHREAD entry lies in an executable
 * segment. 0 ok, -1 with a reason. Nothing is rebased or modified. */
int i1n1_kernel_macho_check(const uint8_t *img, size_t bytes, uint64_t virt_base,
                            struct i1n1_kernel_macho *out, char *why, size_t why_cap);

/* Runs the kernel profile on the shared boot machine (src/guest/machine.c);
 * called by i1n1_efi_boot_guest after the common input checks. */
int i1n1_efi_boot_kernel(const struct i1n1_efi_inputs *in, const struct i1n1_efi_console *con,
                         struct i1n1_efi_result *res);

/* ---- COM2 proxy loop (proxy_loop.c) ------------------------------------- */

struct i1n1_dlink_io {
  void *ctx;
  /* Returns 0 and a byte, or -1 if none is available now. */
  int (*read_byte)(void *ctx, uint8_t *b);
  void (*write_byte)(void *ctx, uint8_t b);
  /* Monotonic coarse clock in milliseconds. */
  uint64_t (*now_ms)(void *ctx);
};

struct i1n1_dlink_stats {
  uint64_t requests, replies, nops, memreads, memwrites, proxy_ops;
  uint64_t badcmd, csum_errors, xfer_errors;
};

/* Serves debug link ABI1 (include/debug_link.h) over `io` against a RAM
 * window [base, base+bytes) at `host`. Returns when the idle deadline passes
 * (wait_ms before the first request, I1N1_DLINK_IDLE_TIMEOUT_MS between
 * requests). BOOT/EVENT and every opcode outside the supported subset are
 * answered ST_BADCMD/S_BADCMD, never emulated. */
void i1n1_dlink_serve(const struct i1n1_dlink_io *io, uint8_t *host,
                      uint64_t base, uint64_t bytes, uint64_t wait_ms,
                      struct i1n1_dlink_stats *stats);

#ifdef __cplusplus
}
#endif

#endif

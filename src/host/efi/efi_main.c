/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * i1n1.efi entry point (x86_64 PE/COFF UEFI application).
 *
 * Phases (the platform phase tracker enforces the split, docs/contracts/debug-link.md section 7):
 *   boot services : COM1 init, read EFI\i1n1\{i1n1.cfg,guest.macho,guest.dt,
 *                   bootargs.txt[,keyseed.bin]} from the ESP, allocate guest
 *                   RAM + heap, GetMemoryMap, ExitBootServices
 *   post-EBS      : runner (WS4 loader/stage-1/PE state, WS3 RAM/EPT/MMIO/GIC,
 *                   WS2 DBT, WS8 provider through the WS4 HVC router), then the
 *                   optional COM2 debug link loop, then shutdown.
 *
 * Apple material is never embedded: every guest input comes from the ESP at
 * run time (project rule 3).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "freestanding/stdio.h"
#include "freestanding/stdlib.h"
#include "freestanding/string.h"
#include "guest.h"
#include "plat_phase.h"
#include "runner.h"
#include "uefi.h"

#define COM1 0x3f8
#define COM2 0x2f8
#define HEAP_BYTES (32u * 1024u * 1024u)
#define NATIVE_ARENA_BYTES (4u * 1024u * 1024u)
#define MAX_INPUT_BYTES (512u * 1024u * 1024u)
#define MMAP_BYTES (64u * 1024u)

/* ---- port I/O and serial ------------------------------------------------ */

static inline void outb(uint16_t port, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t v) {
  __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static void uart_init(uint16_t base) {
  outb(base + 1, 0x00); /* no interrupts */
  outb(base + 3, 0x80); /* DLAB */
  outb(base + 0, 0x01); /* 115200 baud */
  outb(base + 1, 0x00);
  outb(base + 3, 0x03); /* 8N1 */
  outb(base + 2, 0xc7); /* FIFO on, cleared */
  outb(base + 4, 0x0b); /* DTR RTS OUT2 */
}

static void uart_putc(uint16_t base, uint8_t c) {
  unsigned spin = 0;
  while (!(inb(base + 5) & 0x20) && ++spin < 1000000u) {
  }
  outb(base, c);
}

static void com1_write(void *ctx, const char *s, size_t n) {
  size_t i;
  (void)ctx;
  for (i = 0; i < n; ++i) {
    if (s[i] == '\n') uart_putc(COM1, '\r');
    uart_putc(COM1, (uint8_t)s[i]);
  }
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
  char b[320];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(b, sizeof b - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n > sizeof b - 2) n = (int)sizeof b - 2;
  b[n++] = '\n';
  com1_write(NULL, b, (size_t)n);
}

/* COM2: debug link byte pipe. */
static int com2_read(void *ctx, uint8_t *b) {
  (void)ctx;
  if (!(inb(COM2 + 5) & 0x01)) return -1;
  *b = inb(COM2);
  return 0;
}
static void com2_write(void *ctx, uint8_t b) {
  (void)ctx;
  uart_putc(COM2, b);
}

/* Coarse monotonic milliseconds from the CMOS RTC seconds register. Must be
 * polled at least once per minute (the proxy loop polls continuously). */
static uint8_t cmos(uint8_t reg) {
  outb(0x70, reg);
  return inb(0x71);
}
static uint64_t rtc_now_ms(void *ctx) {
  static uint64_t total_ms;
  static int last = -1;
  uint8_t s, b;
  int sec;
  unsigned guard = 0;
  (void)ctx;
  while ((cmos(0x0a) & 0x80) && ++guard < 100000u) {
  }
  s = cmos(0x00);
  b = cmos(0x0b);
  sec = (b & 0x04) ? s : (int)((s & 0x0f) + ((s >> 4) * 10));
  if (last >= 0) total_ms += (uint64_t)(((sec - last) + 60) % 60) * 1000u;
  last = sec;
  return total_ms;
}

/* ---- ESP file access ----------------------------------------------------- */

static const EFI_GUID kLoadedImageGuid = {0x5b1b31a1, 0x9562, 0x11d2,
                                          {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID kSimpleFsGuid = {0x964e5b22, 0x6459, 0x11d2,
                                       {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID kFileInfoGuid = {0x09576e92, 0x6d3f, 0x11d2,
                                       {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

struct blob {
  uint8_t *data;
  size_t bytes;
};

static EFI_BOOT_SERVICES *g_bs;

static uint8_t *alloc_pages_zeroed(uint64_t bytes) {
  EFI_PHYSICAL_ADDRESS addr = 0;
  UINTN pages = (UINTN)((bytes + 4095u) / 4096u);
  if (g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr) != EFI_SUCCESS)
    return NULL;
  memset((void *)(uintptr_t)addr, 0, (size_t)pages * 4096u);
  return (uint8_t *)(uintptr_t)addr;
}

/* Reads \EFI\i1n1\<name> into freshly allocated pages. `opt` files return
 * EFI_NOT_FOUND quietly. */
static EFI_STATUS read_esp(EFI_FILE_PROTOCOL *root, const CHAR16 *name,
                           struct blob *out) {
  EFI_FILE_PROTOCOL *f = NULL;
  uint8_t info_buf[512];
  UINTN info_sz = sizeof info_buf;
  EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
  UINTN rd;
  EFI_STATUS st;
  out->data = NULL;
  out->bytes = 0;
  st = root->Open(root, &f, name, EFI_FILE_MODE_READ, 0);
  if (st != EFI_SUCCESS) return st;
  st = f->GetInfo(f, &kFileInfoGuid, &info_sz, info_buf);
  if (st != EFI_SUCCESS) {
    f->Close(f);
    return st;
  }
  if (info->FileSize == 0 || info->FileSize > MAX_INPUT_BYTES) {
    f->Close(f);
    return EFI_LOAD_ERROR;
  }
  out->data = alloc_pages_zeroed(info->FileSize);
  if (!out->data) {
    f->Close(f);
    return EFI_OUT_OF_RESOURCES;
  }
  rd = (UINTN)info->FileSize;
  st = f->Read(f, &rd, out->data);
  f->Close(f);
  if (st != EFI_SUCCESS || rd != info->FileSize) return EFI_LOAD_ERROR;
  out->bytes = (size_t)rd;
  return EFI_SUCCESS;
}

/* ---- termination ----------------------------------------------------------- */

#define EXIT_VALUE_PASS 0x10  /* qemu isa-debug-exit -> process status 33 */
#define EXIT_VALUE_STOP 0x15  /* -> 43, the contract's stop exit code */
#define EXIT_VALUE_INPUT 0x11 /* -> 35 */

static EFI_RUNTIME_SERVICES *g_rt;

static void finish(int exit_code) __attribute__((noreturn));
static void finish(int exit_code) {
  uint8_t v = exit_code == I1N1_EFI_EXIT_PASSED  ? EXIT_VALUE_PASS
              : exit_code == I1N1_EFI_EXIT_STOP ? EXIT_VALUE_STOP
                                                 : EXIT_VALUE_INPUT;
  say("I1N1:EXIT code=%d", exit_code);
  outb(0xf4, v); /* isa-debug-exit when present (run-qemu adds it) */
  if (g_rt && g_rt->ResetSystem) g_rt->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  outw(0x604, 0x2000); /* q35 ACPI PM shutdown fallback */
  for (;;) __asm__ volatile("cli; hlt");
}

static void stop_early(const char *reason, const char *detail) __attribute__((noreturn));
static void stop_early(const char *reason, const char *detail) {
  say("I1N1:STOP reason=%s pc=0x0 opcode=0x00000000 retired=0 detail=\"%s\"", reason, detail);
  say("I1N1:RESULT status=stopped checkpoints=0 reason=%s retired=0 blocks=0", reason);
  finish(I1N1_EFI_EXIT_STOP);
}

/* ---- entry ---------------------------------------------------------------- */

static uint8_t g_mmap[MMAP_BYTES] __attribute__((aligned(16)));
static struct i1n1_efi_inputs g_inputs;
static struct i1n1_efi_result g_result;
static struct plat_phase_tracker g_adapter;

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  /* Slot order: cfg, guest image, device tree, cmdline, keyseed, boot-args page.
   * The synthetic profile uses the generic names; the kernel profile the real
   * Apple input names (docs/contracts/boot-efi.md section 2). */
  static const CHAR16 *const kSynthNames[] = {
      u"\\EFI\\i1n1\\i1n1.cfg", u"\\EFI\\i1n1\\guest.macho",
      u"\\EFI\\i1n1\\guest.dt", u"\\EFI\\i1n1\\bootargs.txt",
      u"\\EFI\\i1n1\\keyseed.bin", NULL};
  static const CHAR16 *const kKernelNames[] = {
      u"\\EFI\\i1n1\\i1n1.cfg", u"\\EFI\\i1n1\\kernelcache.macho",
      u"\\EFI\\i1n1\\DeviceTree.adt", u"\\EFI\\i1n1\\bootargs.txt",
      u"\\EFI\\i1n1\\keyseed.bin", u"\\EFI\\i1n1\\boot-args.bin"};
  static const char *const kSynthLabels[] = {"i1n1.cfg", "guest.macho", "guest.dt",
                                             "bootargs.txt", "keyseed.bin", ""};
  static const char *const kKernelLabels[] = {"i1n1.cfg", "kernelcache.macho", "DeviceTree.adt",
                                              "bootargs.txt", "keyseed.bin", "boot-args.bin"};
  const CHAR16 *const *kNames = kSynthNames;
  const char *const *kLabels = kSynthLabels;
  struct blob files[6];
  unsigned nfiles = 5;
  EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
  EFI_FILE_PROTOCOL *root = NULL;
  struct i1n1_efi_console con;
  EFI_STATUS es;
  char err[64];
  unsigned i;
  UINTN mmap_sz, map_key, desc_sz;
  uint32_t desc_ver;
  uint8_t *heap;

  g_bs = st->BootServices;
  g_rt = st->RuntimeServices;
  uart_init(COM1);
  uart_init(COM2);
  con.ctx = NULL;
  con.write = com1_write;
  con.now_ms = rtc_now_ms;
  say("I1N1:EFI entry version=%u abi_boot=%u abi_hvc=%u",
      I1N1_EFI_RUNNER_VERSION, I1N1_BOOT_ABI_VERSION, I1N1_HVC_ABI_VERSION);

  plat_phase_init(&g_adapter);
  plat_phase_enter_boot_services(&g_adapter);

  if (g_bs->HandleProtocol(image, &kLoadedImageGuid, (void **)&li) != EFI_SUCCESS || !li)
    stop_early("esp_unavailable", "loaded image protocol missing");
  if (g_bs->HandleProtocol(li->DeviceHandle, &kSimpleFsGuid, (void **)&sfs) != EFI_SUCCESS || !sfs)
    stop_early("esp_unavailable", "simple file system missing");
  if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS || !root)
    stop_early("esp_unavailable", "cannot open ESP volume");

  memset(files, 0, sizeof files);
  /* The config decides which files exist, so it is read and parsed first. */
  es = read_esp(root, kSynthNames[0], &files[0]);
  if (es != EFI_SUCCESS) {
    char d[64];
    snprintf(d, sizeof d, "i1n1.cfg status=0x%llx", (unsigned long long)es);
    stop_early("esp_file_missing", d);
  }
  say("I1N1:ESP file=i1n1.cfg bytes=%llu", (unsigned long long)files[0].bytes);
  memset(&g_inputs, 0, sizeof g_inputs);
  if (i1n1_efi_config_parse((const char *)files[0].data, files[0].bytes, &g_inputs.cfg,
                            err, sizeof err) != 0)
    stop_early("config_invalid", err);
  if (g_inputs.cfg.profile == I1N1_EFI_PROFILE_KERNEL) {
    kNames = kKernelNames;
    kLabels = kKernelLabels;
    nfiles = 6;
  }

  for (i = 1; i < nfiles; ++i) {
    es = read_esp(root, kNames[i], &files[i]);
    if (es == EFI_SUCCESS) {
      say("I1N1:ESP file=%s bytes=%llu", kLabels[i], (unsigned long long)files[i].bytes);
    } else if (i == 4 && es == EFI_NOT_FOUND) {
      say("I1N1:ESP file=%s absent=optional", kLabels[i]);
    } else {
      char d[64];
      snprintf(d, sizeof d, "%s status=0x%llx", kLabels[i], (unsigned long long)es);
      stop_early("esp_file_missing", d);
    }
  }
  if (files[4].data && files[4].bytes != 32) stop_early("keyseed_invalid", "keyseed.bin must be 32 bytes");

  heap = alloc_pages_zeroed(HEAP_BYTES);
  g_inputs.ram = alloc_pages_zeroed(g_inputs.cfg.ram_bytes);
  if (!heap || !g_inputs.ram) stop_early("alloc_failed", "heap or guest RAM allocation failed");
  i1n1_fs_heap_init(heap, HEAP_BYTES);
  if (g_inputs.cfg.dbt == 2) {
    /* Native DBT blocks are emitted here: EfiLoaderCode memory is executable. */
    EFI_PHYSICAL_ADDRESS code = 0;
    UINTN pages = NATIVE_ARENA_BYTES / 4096u;
    if (g_bs->AllocatePages(AllocateAnyPages, EfiLoaderCode, pages, &code) != EFI_SUCCESS)
      stop_early("alloc_failed", "native code arena allocation failed");
    g_inputs.native_arena = (void *)(uintptr_t)code;
    g_inputs.native_arena_bytes = NATIVE_ARENA_BYTES;
  }
  say("I1N1:ALLOC heap=%u ram_bytes=0x%llx", HEAP_BYTES, (unsigned long long)g_inputs.cfg.ram_bytes);

  /* Hash the on-disk image (what the receipt's efi.sha256 names), not the
   * relocated in-memory copy, whose bytes depend on the load address. */
  {
    struct blob self;
    if (read_esp(root, u"\\EFI\\BOOT\\BOOTX64.EFI", &self) == EFI_SUCCESS) {
      g_inputs.efi_image = self.data;
      g_inputs.efi_bytes = self.bytes;
    } else {
      say("I1N1:ESP file=BOOTX64.EFI absent=efi_sha256-unavailable");
    }
  }
  g_inputs.guest = files[1].data;
  g_inputs.guest_bytes = files[1].bytes;
  g_inputs.dt = files[2].data;
  g_inputs.dt_bytes = files[2].bytes;
  g_inputs.cmdline = (const char *)files[3].data;
  g_inputs.cmdline_bytes = files[3].bytes;
  g_inputs.seed = files[4].data;
  g_inputs.bootargs_bin = files[5].data;
  g_inputs.bootargs_bin_bytes = files[5].bytes;

  /* ExitBootServices: the map key must be fresh, so retry once on a stale key. */
  for (i = 0; i < 2; ++i) {
    mmap_sz = sizeof g_mmap;
    es = g_bs->GetMemoryMap(&mmap_sz, (EFI_MEMORY_DESCRIPTOR *)g_mmap, &map_key, &desc_sz, &desc_ver);
    if (es != EFI_SUCCESS) stop_early("memory_map_failed", "GetMemoryMap failed");
    es = g_bs->ExitBootServices(image, map_key);
    if (es == EFI_SUCCESS) break;
  }
  if (es != EFI_SUCCESS) stop_early("ebs_failed", "ExitBootServices rejected the map key");
  g_bs = NULL; /* never touch boot services again */
  plat_phase_enter_post_ebs(&g_adapter);
  say("I1N1:EBS status=ok map_bytes=%llu desc=%llu", (unsigned long long)mmap_sz,
      (unsigned long long)desc_sz);

  i1n1_efi_boot_guest(&g_inputs, &con, &g_adapter, &g_result);

  if (g_inputs.cfg.proxy) {
    struct i1n1_dlink_io io;
    struct i1n1_dlink_stats ps;
    {
      /* Firmware console output may sit in the COM2 RX path; drop it so the
       * proxy framing starts clean (bounded: a chatty peer cannot stall us). */
      unsigned n = 0;
      uint8_t junk;
      while (n < 65536u && com2_read(NULL, &junk) == 0) ++n;
    }
    io.ctx = NULL;
    io.read_byte = com2_read;
    io.write_byte = com2_write;
    io.now_ms = rtc_now_ms;
    say("I1N1:PROXY begin port=com2 wait_s=%u ram_base=0x%llx", g_inputs.cfg.proxy_wait_s,
        (unsigned long long)g_inputs.cfg.ram_base);
    i1n1_dlink_serve(&io, g_inputs.ram, g_inputs.cfg.ram_base, g_inputs.cfg.ram_bytes,
                     (uint64_t)g_inputs.cfg.proxy_wait_s * 1000u, &ps);
    say("I1N1:PROXY end requests=%llu replies=%llu nops=%llu memreads=%llu memwrites=%llu "
        "proxy_ops=%llu badcmd=%llu csum_errors=%llu xfer_errors=%llu",
        (unsigned long long)ps.requests, (unsigned long long)ps.replies,
        (unsigned long long)ps.nops, (unsigned long long)ps.memreads,
        (unsigned long long)ps.memwrites, (unsigned long long)ps.proxy_ops,
        (unsigned long long)ps.badcmd, (unsigned long long)ps.csum_errors,
        (unsigned long long)ps.xfer_errors);
  }

  finish(g_result.exit_code);
}

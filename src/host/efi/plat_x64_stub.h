/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * x64 stubs for ARM-only platform facilities.
 *
 * EL2 entry, GICv3, the EL2 tick, and ARM CPU bring-up cannot run on x64.
 * Every stub below refuses with -ENOSYS and records a reason through the
 * logger; nothing here reports success. The x64 replacements live in
 * src/host (VMX-root, EPT, APIC/HPET, TSC-deadline) and are separate work.
 */
#ifndef PLAT_X64_STUB_H
#define PLAT_X64_STUB_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ENOSYS
#define ENOSYS 38
#endif

typedef void (*plat_stub_logger)(const char *name, const char *reason,
                                void *context);

/* Install a reason logger (default writes to stderr). Passing NULL restores
 * the default. Context is opaque to the stubs. */
void plat_x64_stub_set_logger(plat_stub_logger logger, void *context);

/* Last stub name that refused, and its reason. Never NULL. */
const char *plat_x64_stub_last_name(void);
const char *plat_x64_stub_last_reason(void);
uint64_t plat_x64_stub_refusals(void);

/* ARM EL2 entry (the ARM payload takes the machine at EL2 after ExitBootServices).
 * x64 equivalent: VMX-root entry, owned by src/host. */
int plat_x64_el2_enter(void);

/* ARM GICv3 distributor/redistributor init. x64 equivalent: APIC plus a
 * virtual GIC-lite model, owned by src/host and src/platform. */
int plat_x64_gicv3_init(uint64_t distributor_base);

/* ARM EL2 generic-timer tick read. x64 equivalent: TSC-deadline, owned by
 * src/host. */
int plat_x64_el2_tick_read(uint64_t *ticks_out);

/* ARM CPU bring-up (stage-1 MMU, secondary CPUs). x64 equivalent: VMX/EPT
 * bring-up, owned by src/host. */
int plat_x64_cpu_bring_up(unsigned cpu_index);

#ifdef __cplusplus
}
#endif

#endif /* PLAT_X64_STUB_H */

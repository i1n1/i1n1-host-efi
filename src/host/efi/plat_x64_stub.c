/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WS1 x64 stubs: ARM-only facilities refuse with -ENOSYS plus a reason.
 * No path in this file reports success.
 */
#include "plat_x64_stub.h"

#include <stdio.h>

static void default_logger(const char *name, const char *reason, void *context)
{
  (void)context;
  fprintf(stderr, "x64 stub refused: %s: %s\n", name, reason);
}

static plat_stub_logger active_logger = default_logger;
static void *active_context = NULL;
static const char *last_name = "none";
static const char *last_reason = "no stub called yet";
static uint64_t refusals = 0;

void plat_x64_stub_set_logger(plat_stub_logger logger, void *context)
{
  if (logger == NULL) {
    active_logger = default_logger;
    active_context = NULL;
    return;
  }
  active_logger = logger;
  active_context = context;
}

const char *plat_x64_stub_last_name(void)
{
  return last_name;
}

const char *plat_x64_stub_last_reason(void)
{
  return last_reason;
}

uint64_t plat_x64_stub_refusals(void)
{
  return refusals;
}

static int refuse(const char *name, const char *reason)
{
  last_name = name;
  last_reason = reason;
  refusals++;
  active_logger(name, reason, active_context);
  return -ENOSYS;
}

int plat_x64_el2_enter(void)
{
  return refuse("plat_x64_el2_enter",
                "ARM EL2 entry has no x64 counterpart; "
                "x64 enters through VMX-root (src/host)");
}

int plat_x64_gicv3_init(uint64_t distributor_base)
{
  (void)distributor_base;
  return refuse("plat_x64_gicv3_init",
                "ARM GICv3 is not present on x64; "
                "interrupts use APIC plus a virtual GIC-lite model");
}

int plat_x64_el2_tick_read(uint64_t *ticks_out)
{
  (void)ticks_out;
  return refuse("plat_x64_el2_tick_read",
                "ARM EL2 generic timer is not present on x64; "
                "time comes from TSC-deadline (src/host)");
}

int plat_x64_cpu_bring_up(unsigned cpu_index)
{
  (void)cpu_index;
  return refuse("plat_x64_cpu_bring_up",
                "ARM stage-1 MMU and secondary CPU bring-up do not apply; "
                "x64 bring-up uses VMX-root and EPT (src/host)");
}

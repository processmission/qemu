/*
 * K230 system controller blocks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_SYSCTL_H
#define HW_MISC_K230_SYSCTL_H

#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "qom/object.h"

#define TYPE_K230_SYSCTL_BOOT "riscv.k230.sysctl-boot"
OBJECT_DECLARE_SIMPLE_TYPE(K230SysctlBootState, K230_SYSCTL_BOOT)

#define TYPE_K230_SYSCTL_POWER "riscv.k230.sysctl-power"
OBJECT_DECLARE_SIMPLE_TYPE(K230SysctlPowerState, K230_SYSCTL_POWER)

#define TYPE_K230_SYSCTL_RESET "riscv.k230.sysctl-reset"
OBJECT_DECLARE_SIMPLE_TYPE(K230SysctlResetState, K230_SYSCTL_RESET)

#define K230_SYSCTL_SIZE 0x1000

struct K230SysctlBootState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_SYSCTL_SIZE];
};

struct K230SysctlPowerState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_SYSCTL_SIZE];
};

struct K230SysctlResetState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_SYSCTL_SIZE];
    K230SysctlBootState *boot;
    CPUState *cpu1;
    QEMUTimer *release_timer;
    uint8_t *rtt_saved;
    hwaddr rtt_addr;
    uint32_t rtt_size;
    bool rtt_saved_valid;
    bool defer_cpu1_release;
    uint32_t last_cpu1_rstvec;
    uint32_t deferred_rstvec;
};

#endif /* HW_MISC_K230_SYSCTL_H */

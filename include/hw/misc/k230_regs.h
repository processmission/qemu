/*
 * K230 scratch register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_REGS_H
#define HW_MISC_K230_REGS_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_K230_REGS "riscv.k230.regs"
OBJECT_DECLARE_SIMPLE_TYPE(K230RegsState, K230_REGS)

#define K230_REGS_DEFAULT_SIZE 0x1000
#define K230_REGS_STORAGE_SIZE 0x10000
#define K230_REGS_NO_IRQ_OFFSET UINT64_MAX

struct K230RegsState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    uint64_t size;
    uint64_t irq_start_offset;
    uint64_t irq_start2_offset;
    uint64_t irq_start3_offset;
    uint64_t irq_start_mask;
    uint64_t irq_clear_offset;
    uint64_t irq_status_offset;
    uint64_t irq_status_size;
    uint64_t irq_status_value;
    uint64_t irq_delay_ns;
    uint64_t irq_command_start_offset;
    uint64_t irq_command_end_offset;
    uint64_t irq_command_hi_offset;
    uint64_t complete_zero_base;
    uint64_t complete_zero_size;
    uint64_t complete_zero_page_size;
    uint64_t counter_offset;
    uint64_t counter_size;
    uint64_t counter_frequency;
    QEMUTimer irq_timer;
    bool irq_pending;
    bool irq_clear_clears_status;
    bool irq_on_any_write;
    bool complete_zero_command_pages;
    bool irq_level;
    uint8_t regs[K230_REGS_STORAGE_SIZE];
};

#endif /* HW_MISC_K230_REGS_H */

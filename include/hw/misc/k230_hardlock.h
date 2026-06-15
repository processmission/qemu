/*
 * K230 hardlock registers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_HARDLOCK_H
#define HW_MISC_K230_HARDLOCK_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_K230_HARDLOCK "riscv.k230.hardlock"
OBJECT_DECLARE_SIMPLE_TYPE(K230HardlockState, K230_HARDLOCK)

#define K230_HARDLOCK_SIZE 0x1000
#define K230_HARDLOCK_COUNT 128
#define K230_HARDLOCK_IPCM_IRQ_COUNT 4

struct K230HardlockState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irqs[K230_HARDLOCK_IPCM_IRQ_COUNT];
    uint8_t regs[K230_HARDLOCK_SIZE];
    bool locks[K230_HARDLOCK_COUNT];
};

#endif /* HW_MISC_K230_HARDLOCK_H */

/*
 * Phytium SCP mailbox model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PHYTIUM_SCP_MAILBOX_H
#define HW_MISC_PHYTIUM_SCP_MAILBOX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PHYTIUM_SCP_MAILBOX "phytium-scp-mailbox"
OBJECT_DECLARE_SIMPLE_TYPE(PhytiumScpMailboxState, PHYTIUM_SCP_MAILBOX)

#define PHYTIUM_SCP_MAILBOX_MMIO_SIZE 0x100

struct PhytiumScpMailboxState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[PHYTIUM_SCP_MAILBOX_MMIO_SIZE / sizeof(uint32_t)];
};

#endif /* HW_MISC_PHYTIUM_SCP_MAILBOX_H */

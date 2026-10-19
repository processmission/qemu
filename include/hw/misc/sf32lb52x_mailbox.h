/*
 * SiFli SF32LB52X mailbox controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_MAILBOX_H
#define HW_MISC_SF32LB52X_MAILBOX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_MAILBOX "sifli.sf32lb52x-mailbox"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XMailboxState, SF32LB52X_MAILBOX)

#define SF32LB52X_MAILBOX_SIZE 0x1000
#define SF32LB52X_MAILBOX_MAX_BLOCKS 4
#define SF32LB52X_MAILBOX_BLOCK_STRIDE 0x18

/* Register offsets inside one channel block (Cx...). */
#define SF32LB52X_MAILBOX_IER 0x00
#define SF32LB52X_MAILBOX_ITR 0x04
#define SF32LB52X_MAILBOX_ICR 0x08
#define SF32LB52X_MAILBOX_ISR 0x0c
#define SF32LB52X_MAILBOX_MISR 0x10
#define SF32LB52X_MAILBOX_EXR 0x14

/* One block owns 16 interrupt/notification channels, INT0..INT15. */
#define SF32LB52X_MAILBOX_CHANNEL_MASK 0x0000ffff

/* EXR: ID[3:0] = owner core, EX[31] = 1 when the mutex is unlocked. */
#define SF32LB52X_MAILBOX_EXR_ID_MASK 0x0000000f
#define SF32LB52X_MAILBOX_EXR_EX (1U << 31)
#define SF32LB52X_MAILBOX_EXR_MASK \
    (SF32LB52X_MAILBOX_EXR_ID_MASK | SF32LB52X_MAILBOX_EXR_EX)

struct SF32LB52XMailboxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    /*
     * Number of 6-register channel blocks implemented by this instance:
     * 4 for MAILBOX1 (HPSYS/H2L) and 2 for MAILBOX2 (LPSYS/L2H). Set by
     * the SoC before realize; the reset default is the maximum.
     */
    uint32_t num_blocks;

    uint32_t ier[SF32LB52X_MAILBOX_MAX_BLOCKS];
    uint32_t isr[SF32LB52X_MAILBOX_MAX_BLOCKS];
    uint32_t exr[SF32LB52X_MAILBOX_MAX_BLOCKS];
};

#endif /* HW_MISC_SF32LB52X_MAILBOX_H */

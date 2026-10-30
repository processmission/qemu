/*
 * Sony PlayStation 2 subsystem interface (SIF) and IOP high-level emulation
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PS2_SIF_H
#define HW_MISC_PS2_SIF_H

#include "hw/core/sysbus.h"
#include "hw/dma/ps2_dmac.h"
#include "qom/object.h"

#define TYPE_PS2_SIF "ps2-sif"
OBJECT_DECLARE_SIMPLE_TYPE(PS2SIFState, PS2_SIF)

/* The SIF registers start at 0x1000f200. */
#define PS2_SIF_SIZE            0x100
#define PS2_SIF_MAINADDR        0x00
#define PS2_SIF_SUBADDR         0x10
#define PS2_SIF_MSFLAG          0x20
#define PS2_SIF_SMFLAG          0x30
#define PS2_SIF_SUBCTRL         0x40
#define PS2_SIF_UNKNOWN60       0x60

#define PS2_SIF_STATUS_SIFINIT  0x10000
#define PS2_SIF_STATUS_CMDINIT  0x20000
#define PS2_SIF_STATUS_BOOTEND  0x40000

#define PS2_SIF_CMD_PACKET_MAX  112
#define PS2_SIF_QUEUE_LEN       32
#define PS2_SIF_HEAP_MAX_BLOCKS 64
#define PS2_SIF_NUM_IOP_IRQS    64

#define PS2_IOP_RAM_SIZE        (2 * MiB)

struct PS2SIFState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    PS2DMACState *dmac;
    MemoryRegion *iop_mr;
    AddressSpace iop_as;
    QEMUBH *sif0_bh;
    QEMUTimer *reset_timer;

    uint32_t mainaddr;
    uint32_t subaddr;
    uint32_t msflag;
    uint32_t smflag;
    uint32_t subctrl;
    bool resetting;

    /* EE address of the SIF0 command buffer. */
    uint32_t sif0_addr;
    /* Packets waiting for the EE to start SIF0. */
    uint8_t queue[PS2_SIF_QUEUE_LEN][PS2_SIF_CMD_PACKET_MAX];
    uint32_t queue_head;
    uint32_t queue_len;

    /* Linked IOP modules with an emulated function, a PS2SIFModule mask. */
    uint32_t modules;
    uint32_t module_count;

    /* Allocated IOP heap blocks, sorted by address. */
    uint32_t heap_addr[PS2_SIF_HEAP_MAX_BLOCKS];
    uint32_t heap_size[PS2_SIF_HEAP_MAX_BLOCKS];
    uint32_t heap_count;

    /* EE interrupt number for each relayed IOP interrupt, or zero. */
    uint8_t relay[PS2_SIF_NUM_IOP_IRQS];
    uint64_t iop_irq_level;
};

#endif

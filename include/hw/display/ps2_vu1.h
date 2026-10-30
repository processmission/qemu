/*
 * Sony PlayStation 2 vector unit 1 (VU1) and its interface (VIF1)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_PS2_VU1_H
#define HW_DISPLAY_PS2_VU1_H

#include "hw/core/sysbus.h"
#include "hw/display/ps2_gs.h"
#include "hw/dma/ps2_dmac.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_PS2_VU1 "ps2-vu1"
OBJECT_DECLARE_SIMPLE_TYPE(PS2VU1State, PS2_VU1)

/* VIF1 registers at 0x10003c00, 16 bytes apart. */
#define PS2_VIF1_SIZE           0x400
#define PS2_VIF1_STAT           0x000
#define PS2_VIF1_FBRST          0x010
#define PS2_VIF1_ERR            0x020
#define PS2_VIF1_MARK           0x030
#define PS2_VIF1_CYCLE          0x040
#define PS2_VIF1_MODE           0x050
#define PS2_VIF1_NUM            0x060
#define PS2_VIF1_MASK           0x070
#define PS2_VIF1_CODE           0x080
#define PS2_VIF1_ITOPS          0x090
#define PS2_VIF1_BASE           0x0a0
#define PS2_VIF1_OFST           0x0b0
#define PS2_VIF1_TOPS           0x0c0
#define PS2_VIF1_ITOP           0x0d0
#define PS2_VIF1_TOP            0x0e0
#define PS2_VIF1_R0             0x100
#define PS2_VIF1_C0             0x140
#define PS2_VIF1_NUM_REGS       (0x180 / 0x10)

#define PS2_VIF1_STAT_MRK       (1 << 6)
#define PS2_VIF1_STAT_DBF       (1 << 7)
#define PS2_VIF1_STAT_FDR       (1 << 23)
#define PS2_VIF1_FBRST_RST      (1 << 0)

/* The longest UNPACK: 256 vectors of 4 words. */
#define PS2_VIF1_UNPACK_MAX     1024

/* The VIF1 FIFO at 0x10005000. */
#define PS2_VIF1_FIFO_SIZE      0x1000

/* VU1 micro (program) and data memory at 0x11008000 and 0x1100c000. */
#define PS2_VU1_MEM_SIZE        (16 * KiB)

struct PS2VU1State {
    SysBusDevice parent_obj;

    MemoryRegion vif_iomem;
    MemoryRegion fifo_iomem;
    MemoryRegion micro;
    MemoryRegion data;
    PS2DMACState *dmac;
    PS2GSState *gs;

    /* VIF1 registers, indexed by offset / 16. */
    uint32_t regs[PS2_VIF1_NUM_REGS];
    /* The VIFcode being processed and the words of data it still takes. */
    uint32_t cmd;
    uint32_t cmd_left;
    /* The next address of MPG (in words) or UNPACK (in quadwords). */
    uint32_t cmd_addr;
    /* The data of the UNPACK being received. */
    uint32_t unpack_data[PS2_VIF1_UNPACK_MAX];
    uint32_t unpack_len;

    /* VU1 registers; tpc is where MSCNT continues, in instruction pairs. */
    uint32_t tpc;
    uint32_t vf[32][4];
    uint32_t acc[4];
    uint16_t vi[16];
    uint32_t q;
    uint32_t p;
    /* The results of FDIV and EFU instructions, after a latency. */
    uint32_t q_next;
    uint32_t q_wait;
    uint32_t p_next;
    uint32_t p_wait;
    uint32_t i;
    uint32_t r;
    uint32_t clip;
    uint32_t mac;
    uint32_t status;
};

#endif

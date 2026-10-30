/*
 * Sony PlayStation 2 Emotion Engine DMA controller (DMAC)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_PS2_DMAC_H
#define HW_DMA_PS2_DMAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PS2_DMAC "ps2-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(PS2DMACState, PS2_DMAC)

enum {
    PS2_DMAC_VIF0 = 0,
    PS2_DMAC_VIF1 = 1,
    PS2_DMAC_GIF = 2,
    PS2_DMAC_FROM_IPU = 3,
    PS2_DMAC_TO_IPU = 4,
    PS2_DMAC_SIF0 = 5,
    PS2_DMAC_SIF1 = 6,
    PS2_DMAC_SIF2 = 7,
    PS2_DMAC_FROM_SPR = 8,
    PS2_DMAC_TO_SPR = 9,
    PS2_DMAC_NUM_CHANNELS = 10,
};

/* The channel registers start at 0x10008000, the globals at 0x1000e000. */
#define PS2_DMAC_CHAN_SIZE      0x6100
#define PS2_DMAC_GLOBAL_BASE    0x6000
/* D_ENABLER and D_ENABLEW live in a second window at 0x1000f500. */
#define PS2_DMAC_ENABLE_SIZE    0x100

#define PS2_DMAC_CHCR           0x00
#define PS2_DMAC_MADR           0x10
#define PS2_DMAC_QWC            0x20
#define PS2_DMAC_TADR           0x30
#define PS2_DMAC_ASR0           0x40
#define PS2_DMAC_ASR1           0x50
#define PS2_DMAC_SADR           0x80

#define PS2_DMAC_CHCR_DIR       (1 << 0)
#define PS2_DMAC_CHCR_MOD       (3 << 2)
#define PS2_DMAC_CHCR_MOD_CHAIN (1 << 2)
#define PS2_DMAC_CHCR_ASP_SHIFT 4
#define PS2_DMAC_CHCR_ASP       (3 << PS2_DMAC_CHCR_ASP_SHIFT)
#define PS2_DMAC_CHCR_TTE       (1 << 6)
#define PS2_DMAC_CHCR_TIE       (1 << 7)
#define PS2_DMAC_CHCR_STR       (1 << 8)

#define PS2_DMAC_D_CTRL         0x00
#define PS2_DMAC_D_STAT         0x10
#define PS2_DMAC_D_PCR          0x20
#define PS2_DMAC_D_SQWC         0x30
#define PS2_DMAC_D_RBSR         0x40
#define PS2_DMAC_D_RBOR         0x50
#define PS2_DMAC_D_STADR        0x60

#define PS2_DMAC_D_ENABLER      0x20
#define PS2_DMAC_D_ENABLEW      0x90

typedef struct PS2DMACChannelOps {
    /* Memory-to-peripheral channels: one call per block read from RAM. */
    void (*sink)(void *opaque, const uint8_t *buf, uint32_t len);
    /*
     * Peripheral-to-memory channels: the guest has started the channel.
     * The peripheral writes RAM itself and calls ps2_dmac_channel_done().
     */
    void (*start)(void *opaque);
} PS2DMACChannelOps;

typedef struct PS2DMACChannel {
    uint32_t chcr;
    uint32_t madr;
    uint32_t qwc;
    uint32_t tadr;
    uint32_t asr[2];
    uint32_t sadr;

    const PS2DMACChannelOps *ops;
    void *opaque;
} PS2DMACChannel;

struct PS2DMACState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion enable_iomem;
    qemu_irq irq;

    PS2DMACChannel ch[PS2_DMAC_NUM_CHANNELS];
    uint32_t ctrl;
    uint32_t stat;
    uint32_t pcr;
    uint32_t sqwc;
    uint32_t rbsr;
    uint32_t rbor;
    uint32_t stadr;
    uint32_t enablew;
};

void ps2_dmac_set_channel_ops(PS2DMACState *s, int ch,
                              const PS2DMACChannelOps *ops, void *opaque);
bool ps2_dmac_channel_active(PS2DMACState *s, int ch);
void ps2_dmac_channel_done(PS2DMACState *s, int ch);

#endif

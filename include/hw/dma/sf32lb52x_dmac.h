/*
 * SiFli SF32LB52X DMA controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_SF32LB52X_DMAC_H
#define HW_DMA_SF32LB52X_DMAC_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qemu/main-loop.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_DMAC "sifli.sf32lb52x-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XDMACState, SF32LB52X_DMAC)

#define SF32LB52X_DMAC_SIZE 0x1000
#define SF32LB52X_DMAC_NUM_CHANNELS 8

/* 32-bit registers at 0x00..0xb0, one per 4-byte slot (45 registers). */
#define SF32LB52X_DMAC_R_MAX ((0xb0 / 4) + 1)

struct SF32LB52XDMACState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[SF32LB52X_DMAC_R_MAX];
    uint32_t regs[SF32LB52X_DMAC_R_MAX];

    qemu_irq irq[SF32LB52X_DMAC_NUM_CHANNELS];
    QEMUBH *bh;

    uint8_t pending; /* bitmask of channels with a scheduled transfer */
    uint8_t en_prev[SF32LB52X_DMAC_NUM_CHANNELS];
    uint32_t circ_cndtr[SF32LB52X_DMAC_NUM_CHANNELS];
    uint32_t circ_cpar[SF32LB52X_DMAC_NUM_CHANNELS];
    uint32_t circ_cm0ar[SF32LB52X_DMAC_NUM_CHANNELS];

    bool resetting;
};

#endif /* HW_DMA_SF32LB52X_DMAC_H */

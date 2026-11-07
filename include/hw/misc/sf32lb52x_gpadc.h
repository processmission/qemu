/*
 * SiFli SF32LB52X general purpose SAR ADC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_GPADC_H
#define HW_MISC_SF32LB52X_GPADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_GPADC "sifli.sf32lb52x-gpadc"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XGpadcState, SF32LB52X_GPADC)

#define SF32LB52X_GPADC_SIZE 0x1000
#define SF32LB52X_GPADC_MAX_SLOTS 8
#define SF32LB52X_GPADC_MAX_CHANNELS 8

/* Register offsets. */
#define SF32LB52X_GPADC_CFG1 0x00
#define SF32LB52X_GPADC_SLOT(n) (0x04 + 4 * (n))
#define SF32LB52X_GPADC_RDATA(n) (0x24 + 4 * (n))
#define SF32LB52X_GPADC_DMA_RDATA 0x34
#define SF32LB52X_GPADC_CTRL 0x38
#define SF32LB52X_GPADC_CTRL2 0x3c
#define SF32LB52X_GPADC_STATUS 0x40
#define SF32LB52X_GPADC_IRQ 0x44

/* Reset values from the SF32LB52X SVD. */
#define SF32LB52X_GPADC_CFG1_RESET 0x04714a44
#define SF32LB52X_GPADC_SLOT_RESET 0x00020801
#define SF32LB52X_GPADC_CTRL_RESET 0x00000130

/* CFG1 fields. */
#define SF32LB52X_GPADC_CFG1_SE (1U << 7)
#define SF32LB52X_GPADC_CFG1_EN_V18 (1U << 6)
#define SF32LB52X_GPADC_CFG1_LDOREF_EN (1U << 19)

/* SLOTn fields. */
#define SF32LB52X_GPADC_SLOT_EN (1U << 0)
#define SF32LB52X_GPADC_SLOT_PCHNL_SEL_MASK (0x7U << 8)
#define SF32LB52X_GPADC_SLOT_NCHNL_SEL_MASK (0x7U << 11)
#define SF32LB52X_GPADC_SLOT_ACC_NUM_MASK (0x3U << 17)
#define SF32LB52X_GPADC_SLOT_WMASK                                       \
    (SF32LB52X_GPADC_SLOT_EN | SF32LB52X_GPADC_SLOT_PCHNL_SEL_MASK |    \
     SF32LB52X_GPADC_SLOT_NCHNL_SEL_MASK | SF32LB52X_GPADC_SLOT_ACC_NUM_MASK)

/* CTRL fields. */
#define SF32LB52X_GPADC_CTRL_OP_MODE (1U << 0)
#define SF32LB52X_GPADC_CTRL_START (1U << 1)
#define SF32LB52X_GPADC_CTRL_STOP (1U << 2)

/* STATUS fields. */
#define SF32LB52X_GPADC_STATUS_ADC_DONE (1U << 0)
#define SF32LB52X_GPADC_STATUS_SLOT_DONE_MASK (0xffU << 1)
#define SF32LB52X_GPADC_STATUS_CUR_SLOT_MASK (0x7U << 9)

/* IRQ register. */
#define SF32LB52X_GPADC_IRQ_ICR (1U << 0)
#define SF32LB52X_GPADC_IRQ_IMR (1U << 1)
#define SF32LB52X_GPADC_IRQ_IRSR (1U << 2)
#define SF32LB52X_GPADC_IRQ_ISR (1U << 3)

struct SF32LB52XGpadcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t cfg1;
    uint32_t slots[SF32LB52X_GPADC_MAX_SLOTS];
    uint32_t rdata[SF32LB52X_GPADC_MAX_SLOTS / 2];
    uint32_t dma_rdata;
    uint32_t ctrl;
    uint32_t ctrl2;
    uint32_t status;
    uint32_t raw_isr;
    uint32_t irq_mask;
};

#endif /* HW_MISC_SF32LB52X_GPADC_H */

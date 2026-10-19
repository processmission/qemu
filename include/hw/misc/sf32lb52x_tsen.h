/*
 * SiFli SF32LB52X temperature sensor
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_TSEN_H
#define HW_MISC_SF32LB52X_TSEN_H

#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_TSEN "sifli.sf32lb52x-tsen"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XTsenState, SF32LB52X_TSEN)

#define SF32LB52X_TSEN_SIZE 0x1000

/* Register offsets. */
#define SF32LB52X_TSEN_CTRL 0x00
#define SF32LB52X_TSEN_RDATA 0x04
#define SF32LB52X_TSEN_IRQ 0x08

/*
 * CTRL_REG reset value from the SF32LB52X SVD: bit 2 (RUN), bit 6
 * (FCK_SEL), bit 9 (SER_PAR_SEL), bit 11 (EN) and reserved bits are set.
 */
#define SF32LB52X_TSEN_CTRL_RESET 0x04714a44

/* CTRL fields. */
#define SF32LB52X_TSEN_CTRL_PU (1U << 0)
#define SF32LB52X_TSEN_CTRL_RSTB (1U << 1)
#define SF32LB52X_TSEN_CTRL_RUN (1U << 2)
#define SF32LB52X_TSEN_CTRL_IG_VBE_MASK (0x7U << 3)
#define SF32LB52X_TSEN_CTRL_FCK_SEL_MASK (0x3U << 6)
#define SF32LB52X_TSEN_CTRL_SGN_EN (1U << 8)
#define SF32LB52X_TSEN_CTRL_SER_PAR_SEL (1U << 9)
#define SF32LB52X_TSEN_CTRL_RDY (1U << 10)
#define SF32LB52X_TSEN_CTRL_EN (1U << 11)
#define SF32LB52X_TSEN_CTRL_CLK_DIV_MASK (0x3fU << 12)

/* IRQ register. */
#define SF32LB52X_TSEN_IRQ_ICR (1U << 0)
#define SF32LB52X_TSEN_IRQ_IMR (1U << 1)
#define SF32LB52X_TSEN_IRQ_IRSR (1U << 2)
#define SF32LB52X_TSEN_IRQ_ISR (1U << 3)
#define SF32LB52X_TSEN_IRQ_MASK (SF32LB52X_TSEN_IRQ_IMR | \
                                 SF32LB52X_TSEN_IRQ_IRSR | \
                                 SF32LB52X_TSEN_IRQ_ISR)

/*
 * The model completes a conversion every 10 ms, which is fast enough for the
 * Zephyr driver polling loop (k_msleep(1)) and reproducible for qtests that
 * advance the virtual clock.
 */
#define SF32LB52X_TSEN_CONV_HZ 100

/*
 * 25 degrees C code derived from the driver formula
 * temp = ((code + 3000) * 749.2916 / 10100) - 277: code ~= 1071.
 */
#define SF32LB52X_TSEN_DEFAULT_CODE 1071

struct SF32LB52XTsenState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    ptimer_state *timer;

    uint32_t ctrl;
    uint32_t rdata;
    uint32_t raw_isr;
    uint32_t irq_mask;
    bool running;
};

#endif /* HW_MISC_SF32LB52X_TSEN_H */

/*
 * SiFli SF32LB52X true random number generator
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_TRNG_H
#define HW_MISC_SF32LB52X_TRNG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_TRNG "sifli.sf32lb52x-trng"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XTrngState, SF32LB52X_TRNG)

#define SF32LB52X_TRNG_SIZE     0x1000
#define SF32LB52X_TRNG_RAND_NUM 8

/* Register offsets. */
#define SF32LB52X_TRNG_CTRL       0x00
#define SF32LB52X_TRNG_STAT       0x04
#define SF32LB52X_TRNG_CFG        0x08
#define SF32LB52X_TRNG_ISR        0x0c
#define SF32LB52X_TRNG_SEED0      0x10
#define SF32LB52X_TRNG_RAND0      0x30
#define SF32LB52X_TRNG_CAL_CFG    0x50
#define SF32LB52X_TRNG_CAL_RESULT 0x54

/* CTRL */
#define SF32LB52X_TRNG_CTRL_SEED_START     (1U << 0)
#define SF32LB52X_TRNG_CTRL_RAND_START     (1U << 1)
#define SF32LB52X_TRNG_CTRL_SEED_STOP      (1U << 2)
#define SF32LB52X_TRNG_CTRL_RAND_STOP      (1U << 3)
#define SF32LB52X_TRNG_CTRL_RAND_SUSPEND   (1U << 4)

/* STAT */
#define SF32LB52X_TRNG_STAT_SEED_BUSY      (1U << 0)
#define SF32LB52X_TRNG_STAT_SEED_VALID     (1U << 1)
#define SF32LB52X_TRNG_STAT_RAND_BUSY      (1U << 2)
#define SF32LB52X_TRNG_STAT_RAND_VALID     (1U << 3)

/* CFG */
#define SF32LB52X_TRNG_CFG_USE_EXT_SEED    (1U << 1)

/* ISR: raw status bits 0..2, interrupt mask bits 16..18 */
#define SF32LB52X_TRNG_ISR_RAW_MASK        0x7
#define SF32LB52X_TRNG_ISR_MASK_SHIFT      16
#define SF32LB52X_TRNG_ISR_MASK \
    (0x7U << SF32LB52X_TRNG_ISR_MASK_SHIFT)
#define SF32LB52X_TRNG_ISR_SEED_GEN_DONE   (1U << 0)
#define SF32LB52X_TRNG_ISR_RAND_NUM_AVAIL  (1U << 1)
#define SF32LB52X_TRNG_ISR_PRNG_LOCKUP     (1U << 2)
#define SF32LB52X_TRNG_ISR_RESET           0x00070000

/* CAL_CFG */
#define SF32LB52X_TRNG_CAL_ENABLE          (1U << 4)
#define SF32LB52X_TRNG_CAL_DONE            (1U << 5)

struct SF32LB52XTrngState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t ctrl;
    uint32_t cfg;
    uint32_t raw_isr;
    uint32_t isr_mask;
    uint32_t seed[SF32LB52X_TRNG_RAND_NUM];
    uint32_t rnd[SF32LB52X_TRNG_RAND_NUM];
    uint32_t cal_cfg;
    uint32_t cal_result;
    bool seed_valid;
    bool rnd_valid;
    uint64_t prng_state;
};

#endif /* HW_MISC_SF32LB52X_TRNG_H */

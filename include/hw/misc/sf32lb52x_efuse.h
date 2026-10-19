/*
 * SiFli SF32LB52X eFuse (OTP) controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_EFUSE_H
#define HW_MISC_SF32LB52X_EFUSE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_EFUSE "sifli.sf32lb52x-efuse"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XEfuseState, SF32LB52X_EFUSE)

#define SF32LB52X_EFUSE_SIZE 0x0100
#define SF32LB52X_EFUSE_BANKS 4
#define SF32LB52X_EFUSE_BANK_WORDS 8

/* Register offsets. */
#define SF32LB52X_EFUSE_CR 0x00
#define SF32LB52X_EFUSE_TIMR 0x04
#define SF32LB52X_EFUSE_SR 0x08
#define SF32LB52X_EFUSE_RSVDR 0x0c
#define SF32LB52X_EFUSE_PGM_DATA(n) (0x10 + 4 * (n))
#define SF32LB52X_EFUSE_BANK_DATA(b, n) (0x30 + 0x20 * (b) + 4 * (n))
#define SF32LB52X_EFUSE_ANACR 0xb0
#define SF32LB52X_EFUSE_DB_SEL 0xb4

/* CR fields. */
#define SF32LB52X_EFUSE_CR_EN (1U << 0)
#define SF32LB52X_EFUSE_CR_MODE (1U << 1)
#define SF32LB52X_EFUSE_CR_BANKSEL_MASK (0x3U << 2)
#define SF32LB52X_EFUSE_CR_IE (1U << 4)
#define SF32LB52X_EFUSE_CR_BANKSEL_SHIFT 2

/* SR fields. */
#define SF32LB52X_EFUSE_SR_DONE (1U << 0)

struct SF32LB52XEfuseState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint32_t cr;
    uint32_t timr;
    uint32_t sr;
    uint32_t rsvdr;
    uint32_t pgm_data[SF32LB52X_EFUSE_BANK_WORDS];
    uint32_t bank[SF32LB52X_EFUSE_BANKS][SF32LB52X_EFUSE_BANK_WORDS];
    uint32_t anacr;
    uint32_t db_sel;
};

#endif /* HW_MISC_SF32LB52X_EFUSE_H */

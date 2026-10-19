/*
 * SiFli SF32LB52X CRC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_CRC_H
#define HW_MISC_SF32LB52X_CRC_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_CRC "sifli.sf32lb52x-crc"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XCrcState, SF32LB52X_CRC)

#define SF32LB52X_CRC_SIZE 0x1000

/* 32-bit registers in one 4-byte slot each: DR, SR, CR, RSVD1, INIT, POL */
#define SF32LB52X_CRC_R_MAX 6

struct SF32LB52XCrcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[SF32LB52X_CRC_R_MAX];
    uint32_t regs[SF32LB52X_CRC_R_MAX];

    /* Internal CRC datapath register (raw value, before REV_OUT) */
    uint32_t crc;

    bool resetting;
};

#endif /* HW_MISC_SF32LB52X_CRC_H */

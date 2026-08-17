/*
 * Rockchip RK806 PMIC model (SPI interface)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_RK806_H
#define HW_SSI_RK806_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_RK806 "rk806"
OBJECT_DECLARE_SIMPLE_TYPE(RK806State, RK806)

#define RK806_REG_COUNT 256

struct RK806State {
    SSIPeripheral parent_obj;

    uint8_t regs[RK806_REG_COUNT];

    /* SPI frame parsing. */
    enum {
        RK806_FRAME_CMD,
        RK806_FRAME_ADDR,
        RK806_FRAME_REG_H,
        RK806_FRAME_DATA,
        RK806_FRAME_READ,
    } frame_state;
    uint8_t frame_cmd;
    uint8_t frame_addr;
    unsigned int frame_remain;
};

#endif /* HW_SSI_RK806_H */

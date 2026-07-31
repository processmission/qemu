/*
 * STC8G1K08A I2C controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_STC8G_I2C_H
#define HW_I2C_STC8G_I2C_H

#include "qom/object.h"

#define TYPE_STC8G_I2C "stc8g-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gI2CState, STC8G_I2C)

#define STC8G_I2C_MMIO_REGS 9

enum Stc8gI2CSlaveEvent {
    STC8G_I2C_SLAVE_START = 1,
    STC8G_I2C_SLAVE_RECEIVE,
    STC8G_I2C_SLAVE_TRANSMIT,
    STC8G_I2C_SLAVE_STOP,
};

#endif

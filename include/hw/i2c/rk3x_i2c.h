/*
 * Rockchip rk3x I2C controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_RK3X_I2C_H
#define HW_I2C_RK3X_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_RK3X_I2C "rockchip-rk3x-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Rk3xI2CState, RK3X_I2C)

#define RK3X_I2C_MMIO_SIZE 0x1000

#endif /* HW_I2C_RK3X_I2C_H */

/*
 * Rockchip RK3588 secure OTP
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NVRAM_RK3588_SECURE_OTP_H
#define HW_NVRAM_RK3588_SECURE_OTP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_SECURE_OTP "rk3588-secure-otp"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588SecureOTPState, RK3588_SECURE_OTP)

#define RK3588_SECURE_OTP_MMIO_SIZE 0x1000

#endif /* HW_NVRAM_RK3588_SECURE_OTP_H */

/*
 * Rockchip RK3588 ATF DDR runtime descriptor
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_ATF_DDR_H
#define HW_MISC_RK3588_ATF_DDR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_ATF_DDR "rk3588-atf-ddr"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588ATFDDRState, RK3588_ATF_DDR)

#define RK3588_ATF_DDR_RUNTIME_BASE 0x0008d000ULL
#define RK3588_ATF_DDR_RUNTIME_SIZE 0x8000

#endif /* HW_MISC_RK3588_ATF_DDR_H */

/*
 * Rockchip RK3588 PMU General Register Files
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_GRF_H
#define HW_MISC_RK3588_GRF_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_GRF "rk3588-grf"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588GRFState, RK3588_GRF)

#define RK3588_GRF_MMIO_SIZE 0x1000

enum {
    RK3588_GRF_MMIO_PMU0,
    RK3588_GRF_MMIO_PMU1,
    RK3588_GRF_MMIO_SYS,
    RK3588_GRF_MMIO_COUNT,
};

#endif /* HW_MISC_RK3588_GRF_H */

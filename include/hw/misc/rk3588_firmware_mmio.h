/*
 * Rockchip RK3588 vendor firmware MMIO compatibility region
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_FIRMWARE_MMIO_H
#define HW_MISC_RK3588_FIRMWARE_MMIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_FIRMWARE_MMIO "rk3588-firmware-mmio"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588FirmwareMMIOState, RK3588_FIRMWARE_MMIO)

#define RK3588_FIRMWARE_MMIO_BASE 0xf7000000ULL
#define RK3588_FIRMWARE_MMIO_SIZE 0x08000000

#endif /* HW_MISC_RK3588_FIRMWARE_MMIO_H */

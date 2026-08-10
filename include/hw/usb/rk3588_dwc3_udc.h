/*
 * Rockchip RK3588 DWC3 device controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_RK3588_DWC3_UDC_H
#define HW_USB_RK3588_DWC3_UDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_DWC3_UDC "rk3588-dwc3-udc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588DWC3UDCState, RK3588_DWC3_UDC)

#define RK3588_DWC3_UDC_MMIO_SIZE 0x400000

#endif

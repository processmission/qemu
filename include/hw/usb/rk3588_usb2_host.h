/*
 * Rockchip RK3588 USB2 host register shim
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_RK3588_USB2_HOST_H
#define HW_USB_RK3588_USB2_HOST_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_USB2_HOST "rk3588-usb2-host"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588USB2HostState, RK3588_USB2_HOST)

#define RK3588_USB2_HOST_MMIO_SIZE 0x40000

enum {
    RK3588_USB2_HOST_EHCI0,
    RK3588_USB2_HOST_OHCI0,
    RK3588_USB2_HOST_EHCI1,
    RK3588_USB2_HOST_OHCI1,
    RK3588_USB2_HOST_MMIO_COUNT,
};

void rk3588_usb2_host_set_active(RK3588USB2HostState *s, bool active);

#endif

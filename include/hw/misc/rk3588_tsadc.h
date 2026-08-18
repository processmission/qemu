/*
 * RK3588 TSADC (thermal sensor ADC) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_TSADC_H
#define HW_MISC_RK3588_TSADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_TSADC "rk3588-tsadc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588TSADCState, RK3588_TSADC)

#define RK3588_TSADC_MMIO_SIZE 0x400
#define RK3588_TSADC_NUM_CHANNELS 7

/* Room-temperature ADC code from the rk3588 sensor table (~25 C). */
#define RK3588_TSADC_ROOM_CODE 285

struct RK3588TSADCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[RK3588_TSADC_MMIO_SIZE / 4];
};

#endif /* HW_MISC_RK3588_TSADC_H */

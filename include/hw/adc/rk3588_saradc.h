/*
 * RK3588 SARADC (v2 controller) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ADC_RK3588_SARADC_H
#define HW_ADC_RK3588_SARADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_SARADC "rk3588-saradc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588SARADCState, RK3588_SARADC)

#define RK3588_SARADC_NUM_CHANNELS 8
#define RK3588_SARADC_MMIO_SIZE    0x400

/* The v2 controller converts 12 bits, unlike the 10-bit rk3566 block. */
#define RK3588_SARADC_RESOLUTION   12
#define RK3588_SARADC_DATA_MASK    ((1U << RK3588_SARADC_RESOLUTION) - 1)

struct RK3588SARADCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq irq;

    uint32_t regs[RK3588_SARADC_MMIO_SIZE / 4];

    /*
     * Channel of the conversion in flight.  CONV_CON stays writable while
     * the sample is being taken, so the selection is latched when the
     * conversion starts and the result is stored according to that copy,
     * not to whatever the guest has programmed in the meantime.
     */
    uint32_t conv_ch;

    /* Board wiring, exposed as properties. */
    uint32_t inputs_uv[RK3588_SARADC_NUM_CHANNELS];
    uint32_t reference_uv;
    uint32_t clock_hz;
};

#endif /* HW_ADC_RK3588_SARADC_H */

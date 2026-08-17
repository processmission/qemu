/*
 * RK3588 TRNGv1 model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_RNG_H
#define HW_MISC_RK3588_RNG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_RNG "rk3588-rng"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588RNGState, RK3588_RNG)

#define RK3588_RNG_MMIO_SIZE 0x200

struct RK3588RNGState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[RK3588_RNG_MMIO_SIZE / 4];
    uint32_t rand_words[8];
    bool rand_ready;
};

#endif /* HW_MISC_RK3588_RNG_H */

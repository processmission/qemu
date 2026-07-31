/*
 * STC8G1K08A interrupt controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_STC8G_INTC_H
#define HW_INTC_STC8G_INTC_H

#include "qom/object.h"

#define TYPE_STC8G_INTC "stc8g-intc"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gIntcState, STC8G_INTC)

#define STC8G_INTC_NUM_SOURCES 8

enum Stc8gIntcMMIO {
    STC8G_INTC_MMIO_IE2,
    STC8G_INTC_MMIO_IP2,
    STC8G_INTC_MMIO_IP2H,
    STC8G_INTC_MMIO_AUXINTIF,
};

enum Stc8gIntcSource {
    STC8G_INTC_ADC,
    STC8G_INTC_LVD,
    STC8G_INTC_PCA,
    STC8G_INTC_SPI,
    STC8G_INTC_INT2,
    STC8G_INTC_INT3,
    STC8G_INTC_INT4,
    STC8G_INTC_I2C,
};

#endif

/*
 * STC8G1K08A ADC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ADC_STC8G_ADC_H
#define HW_ADC_STC8G_ADC_H

#include "qom/object.h"

#define TYPE_STC8G_ADC "stc8g-adc"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gADCState, STC8G_ADC)

enum Stc8gADCMMIO {
    STC8G_ADC_MMIO_CONTR,
    STC8G_ADC_MMIO_RES,
    STC8G_ADC_MMIO_RESL,
    STC8G_ADC_MMIO_CFG,
    STC8G_ADC_MMIO_TIM,
};

#endif

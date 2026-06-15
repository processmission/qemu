/*
 * K230 ADC register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_ADC_H
#define HW_MISC_K230_ADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_ADC "riscv.k230.adc"
OBJECT_DECLARE_SIMPLE_TYPE(K230AdcState, K230_ADC)

#define K230_ADC_SIZE 0x1000

struct K230AdcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_ADC_SIZE];
};

#endif /* HW_MISC_K230_ADC_H */

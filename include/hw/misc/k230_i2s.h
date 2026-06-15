/*
 * K230 I2S / WS2812 registers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_K230_I2S_H
#define HW_MISC_K230_I2S_H

#include "hw/core/register.h"
#include "hw/misc/k230_regs.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_I2S "riscv.k230.i2s"
OBJECT_DECLARE_SIMPLE_TYPE(K230I2SState, K230_I2S)

#define K230_I2S_SIZE 0x1000
#define K230_I2S_R_MAX (K230_I2S_SIZE / sizeof(uint32_t))
#define K230_I2S_WS2812_SAMPLES 4

struct K230I2SState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    RegisterInfoArray *reg_array;
    K230RegsState *compat_regs;
    bool state_loaded;
    bool compat_imported;
    RegisterInfo regs_info[K230_I2S_R_MAX];
    uint32_t regs[K230_I2S_R_MAX];
    uint32_t ws2812_byte_count;
    uint32_t ws2812_byte[K230_I2S_WS2812_SAMPLES];
    uint32_t ws2812_padding_count;
    uint32_t ws2812_invalid_count;
};

#endif /* HW_MISC_K230_I2S_H */

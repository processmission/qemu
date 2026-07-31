/*
 * STC32G144K246 SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MCS51_STC32G_H
#define HW_MCS51_STC32G_H

#include "hw/char/stc32g_uart.h"
#include "hw/core/sysbus.h"
#include "hw/gpio/stc32g_gpio.h"
#include "hw/timer/stc32g_timer.h"
#include "qemu/units.h"
#include "target/mcs51/cpu.h"
#include "qom/object.h"

#define TYPE_STC32G_SOC "stc32g-soc"
OBJECT_DECLARE_SIMPLE_TYPE(Stc32gSoCState, STC32G_SOC)

#define STC32G_EDATA_BASE 0x000000u
#define STC32G_EDATA_SIZE (16 * KiB)
#define STC32G_XDATA_BASE 0x010000u
#define STC32G_XDATA_SIZE (128 * KiB)
#define STC32G_EXEC_DATA_BASE 0x030000u
#define STC32G_EXEC_CODE_BASE 0x800000u
#define STC32G_EXEC_RAM_SIZE (4 * KiB)
#define STC32G_FLASH_BASE 0xfc2800u
#define STC32G_FLASH_SIZE (246 * KiB)

struct Stc32gSoCState {
    SysBusDevice parent_obj;

    MCS251CPU cpu;
    MemoryRegion edata;
    MemoryRegion xdata;
    MemoryRegion exec_ram;
    MemoryRegion exec_data_alias;
    MemoryRegion exec_code_alias;
    MemoryRegion flash;
    DeviceState *timer;
    DeviceState *uart;
    DeviceState *gpio;
    DeviceState *dsp;
    DeviceState *tfpu;
};

#endif

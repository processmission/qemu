/*
 * STC8G1K08A SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MCS51_STC8G_H
#define HW_MCS51_STC8G_H

#include "hw/core/sysbus.h"
#include "qemu/units.h"
#include "target/mcs51/cpu.h"
#include "qom/object.h"

#define TYPE_STC8G_SOC "stc8g-soc"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gSoCState, STC8G_SOC)

#define STC8G_FLASH_BASE MCS51_CODE_PHYS_BASE
#define STC8G_FLASH_SIZE (8 * KiB)
#define STC8G_EEPROM_BASE (STC8G_FLASH_BASE + STC8G_FLASH_SIZE)
#define STC8G_EEPROM_SIZE (4 * KiB)
#define STC8G_IDATA_BASE MCS51_IDATA_PHYS_BASE
#define STC8G_IDATA_SIZE 256
#define STC8G_XDATA_BASE MCS51_XDATA_PHYS_BASE
#define STC8G_XDATA_SIZE (1 * KiB)

struct Stc8gSoCState {
    SysBusDevice parent_obj;

    MCS51CPU cpu;
    MemoryRegion flash;
    MemoryRegion idata;
    MemoryRegion xdata;
    DeviceState *adc;
    DeviceState *gpio;
    DeviceState *i2c;
    DeviceState *iap;
    DeviceState *intc;
    DeviceState *lvd;
    DeviceState *mdu;
    DeviceState *pca;
    DeviceState *spi;
    DeviceState *sysctrl;
    DeviceState *timer;
    DeviceState *uart;
    DeviceState *wdt;
};

#endif

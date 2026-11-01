/*
 * Rockchip RK3566 SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RK3566_H
#define HW_ARM_RK3566_H

#include "qom/object.h"
#include "target/arm/cpu.h"
#include "hw/char/dw8250.h"
#include "hw/core/qdev.h"
#include "hw/cpu/cluster.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/misc/rk3588_scmi.h"
#include "system/memory.h"

#define TYPE_RK3566_SOC "rockchip.rk3566.soc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566SoCState, RK3566_SOC)

#define RK3566_NUM_CPUS  4
#define RK3566_NUM_IRQS  320
#define RK3566_CPUS      RK3566_NUM_CPUS
#define RK3566_IRQS      RK3566_NUM_IRQS
#define RK3566_NUM_UARTS 4
#define RK3566_NUM_I2C   4
#define RK3566_NUM_SPI   4
#define RK3566_NUM_GPIO  5

struct RK3566SoCState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    ARMCPU cpu[RK3566_NUM_CPUS];
    CPUClusterState cluster;
    GICv3State gic;
    DW8250State *uart[RK3566_NUM_UARTS];
    DeviceState *its;
    DeviceState *cru;
    DeviceState *pmucru;
    DeviceState *dma[2];
    DeviceState *gpio[RK3566_NUM_GPIO];
    DeviceState *i2c[RK3566_NUM_I2C];
    DeviceState *spi[RK3566_NUM_SPI];
    DeviceState *isp;
    DeviceState *venc_mmu[2];
    RK3588SCMIState *scmi;
    DeviceState *sdhci;
    DeviceState *sdio;
    DeviceState *usb;
    MemoryRegion *memory;
    MemoryRegion debug;
    uint32_t debug_regs[0x10000 / 4];
    MemoryRegion dma_alias[2];
    MemoryRegion venc_mmu_window[2];
    MemoryRegion qos[24];
    uint32_t qos_regs[24][8];
    char *camera_chardev;
};

#endif /* HW_ARM_RK3566_H */

/*
 * Rockchip SPI controller (rk3066/rk3588) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_ROCKCHIP_SPI_H
#define HW_SSI_ROCKCHIP_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_SPI "rockchip-spi"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipSPIState, ROCKCHIP_SPI)

#define ROCKCHIP_SPI_MMIO_SIZE 0x1000
#define ROCKCHIP_SPI_FIFO_DEPTH 64

struct RockchipSPIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *spi;
    qemu_irq irq;

    uint32_t ctrlr0;
    uint32_t ctrlr1;
    uint32_t ssi_enr;
    uint32_t baudr;
    uint32_t txftlr;
    uint32_t rxftlr;
    uint32_t imr;
    uint32_t isr;

    uint8_t rx_fifo[ROCKCHIP_SPI_FIFO_DEPTH];
    unsigned int rx_level;
    unsigned int rx_pos;
};

#endif /* HW_SSI_ROCKCHIP_SPI_H */

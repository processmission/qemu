/*
 * Synopsys DesignWare Ethernet MAC dwmac-4.20a (GMAC4).
 *
 * Copyright (c) 2026 Chao Liu
 *
 * RK3588 wires this for its GMAC node (snps,dwmac-4.20a /
 * rockchip,rk3588-gmac).
 * Register map, descriptor layout, and IRQ semantics come from the
 * dwmac4 / dwmac4_dma / dwmac4_descs headers in the Linux stmmac driver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Local-only model; not for upstream.
 */

#ifndef HW_NET_DWMAC4_H
#define HW_NET_DWMAC4_H

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "net/net.h"

/*
 * MAC_VERSION (GMAC4_VERSION @ 0x110) synth-id. Linux stmmac reads SNPSVER
 * (bits[7:0]) and routes through stmmac_hw[]: min_id=DWMAC_CORE_5_10 (0x51)
 * selects dwmac510_ops / dwmac410_dma_ops / dwmac4_desc_ops. Real RK3588
 * silicon reports 0x51; the contract pins the model to 0x51.
 */
#define DWMAC4_SNPSVER                 0x51u
#define DWMAC4_VERSION_RESET          (DWMAC4_SNPSVER & 0xff)

/* MAC register bank window. Covers 0x000..0x3ff (some slots beyond 0x300). */
#define DWMAC4_MAC_REG_SIZE           0x400
#define DWMAC4_MAC_NR_REGS            (DWMAC4_MAC_REG_SIZE / sizeof(uint32_t))

/*
 * DMA + per-channel register window. We model 0x1000..0x11ff which covers
 * the global DMA block plus channels 0 and 1 (stride 0x80). Chan 0 is what
 * the stmmac driver uses for the primary RX/TX queue on RK3588.
 */
#define DWMAC4_DMA_REG_BASE           0x1000
#define DWMAC4_DMA_REG_SIZE           0x200
#define DWMAC4_DMA_NR_REGS            (DWMAC4_DMA_REG_SIZE / sizeof(uint32_t))

#define DWMAC4_DMA_CHAN_STRIDE        0x80
#define DWMAC4_NR_CHANNELS            2

/* Full MMIO window reserved for the device (matches the 64 KiB DT reg). */
#define DWMAC4_MMIO_SIZE              0x10000

typedef struct DWMAC4State {
    SysBusDevice parent;

    MemoryRegion iomem;
    qemu_irq sb_irq;            /* macirq line (SPI 227 on RK3588) */

    NICState *nic;
    NICConf conf;

    /* MAC + DMA register banks, accessed via registerinfo. */
    uint32_t mac_regs[DWMAC4_MAC_NR_REGS];
    RegisterInfo mac_regs_info[DWMAC4_MAC_NR_REGS];
    RegisterInfoArray *mac_reg_array;
    uint32_t dma_regs[DWMAC4_DMA_NR_REGS];
    RegisterInfo dma_regs_info[DWMAC4_DMA_NR_REGS];
    RegisterInfoArray *dma_reg_array;

    /* Per-channel descriptor-ring cursor state (not part of the reg bank). */
    uint32_t tx_desc_cur[DWMAC4_NR_CHANNELS];
    uint32_t rx_desc_cur[DWMAC4_NR_CHANNELS];

    /* MDIO clause-22 PHY scratch (minimal: link up, full-duplex 1G). */
    uint16_t phy_regs[32];
} DWMAC4State;

#define TYPE_DWMAC4 "dwmac4"
OBJECT_DECLARE_SIMPLE_TYPE(DWMAC4State, DWMAC4)

#endif /* HW_NET_DWMAC4_H */

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
 * MAC_VERSION (GMAC4_VERSION @ 0x110) contains the Synopsys version in
 * bits[7:0] and a user version in bits[15:8].  Boards select their reported
 * version through properties; the defaults retain the original 0x51 model.
 */
#define DWMAC4_DEFAULT_SNPS_VERSION    0x51u
#define DWMAC4_DEFAULT_DMA_WIDTH       32u
#define DWMAC4_DEFAULT_FIFO_SIZE       4096u

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
    uint64_t tx_desc_cur[DWMAC4_NR_CHANNELS];
    uint64_t rx_desc_cur[DWMAC4_NR_CHANNELS];

    /* MDIO clause-22 PHY scratch (minimal: link up, full-duplex 1G). */
    uint16_t phy_regs[32];
    uint16_t phy_page;

    /* Board-selectable synthesis and PHY identity. */
    uint8_t snps_version;
    uint8_t user_version;
    uint8_t dma_width;
    uint8_t phy_addr;
    uint16_t phy_id1;
    uint16_t phy_id2;
    uint32_t tx_fifo_size;
    uint32_t rx_fifo_size;
    bool tso;
} DWMAC4State;

#define TYPE_DWMAC4 "dwmac4"
OBJECT_DECLARE_SIMPLE_TYPE(DWMAC4State, DWMAC4)

#endif /* HW_NET_DWMAC4_H */

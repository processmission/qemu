/*
 * Rockchip SFC (SPI Flash Controller) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_ROCKCHIP_SFC_H
#define HW_SSI_ROCKCHIP_SFC_H

#include "exec/hwaddr.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_SFC "rockchip-sfc"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipSFCState, ROCKCHIP_SFC)

#define ROCKCHIP_SFC_MMIO_SIZE 0x4000

/* Register offsets (SFC_* in drivers/spi/spi-rockchip-sfc.c). */
#define ROCKCHIP_SFC_CTRL        0x000
#define ROCKCHIP_SFC_IMR         0x004
#define ROCKCHIP_SFC_ICLR        0x008
#define ROCKCHIP_SFC_FTLR        0x00c
#define ROCKCHIP_SFC_RCVR        0x010
#define ROCKCHIP_SFC_AX          0x014
#define ROCKCHIP_SFC_ABIT        0x018
#define ROCKCHIP_SFC_ISR         0x01c
#define ROCKCHIP_SFC_FSR         0x020
#define ROCKCHIP_SFC_SR          0x024
#define ROCKCHIP_SFC_RISR        0x028
#define ROCKCHIP_SFC_VER         0x02c
#define ROCKCHIP_SFC_DLL_CTRL0   0x03c
#define ROCKCHIP_SFC_DMA_TRIGGER 0x080
#define ROCKCHIP_SFC_DMA_ADDR    0x084
#define ROCKCHIP_SFC_LEN_CTRL    0x088
#define ROCKCHIP_SFC_LEN_EXT     0x08c
#define ROCKCHIP_SFC_CMD         0x100
#define ROCKCHIP_SFC_ADDR        0x104
#define ROCKCHIP_SFC_DATA        0x108

/* SFC_CMD fields. */
#define ROCKCHIP_SFC_CMD_IDX_MASK       0xff
#define ROCKCHIP_SFC_CMD_DUMMY_SHIFT    8
#define ROCKCHIP_SFC_CMD_DUMMY_MASK     0x0f
#define ROCKCHIP_SFC_CMD_DIR_SHIFT      12
#define ROCKCHIP_SFC_CMD_DIR_RD         0
#define ROCKCHIP_SFC_CMD_DIR_WR         1
#define ROCKCHIP_SFC_CMD_ADDR_SHIFT     14
#define ROCKCHIP_SFC_CMD_ADDR_0BITS     0
#define ROCKCHIP_SFC_CMD_ADDR_24BITS    1
#define ROCKCHIP_SFC_CMD_ADDR_32BITS    2
#define ROCKCHIP_SFC_CMD_ADDR_XBITS     3
#define ROCKCHIP_SFC_CMD_TRAN_BYTES_SHIFT 16
#define ROCKCHIP_SFC_CMD_CS_SHIFT       30

/* SFC_FSR fields. */
#define ROCKCHIP_SFC_FSR_TX_IS_FULL     BIT(0)
#define ROCKCHIP_SFC_FSR_TX_IS_EMPTY    BIT(1)
#define ROCKCHIP_SFC_FSR_RX_IS_EMPTY    BIT(2)
#define ROCKCHIP_SFC_FSR_RX_IS_FULL     BIT(3)
#define ROCKCHIP_SFC_FSR_TXLV_MASK      (0x1f << 8)
#define ROCKCHIP_SFC_FSR_TXLV_SHIFT     8
#define ROCKCHIP_SFC_FSR_RXLV_MASK      (0x1f << 16)
#define ROCKCHIP_SFC_FSR_RXLV_SHIFT     16

/* SFC_RISR / SFC_IMR fields. */
#define ROCKCHIP_SFC_RISR_DMA           BIT(7)

#define ROCKCHIP_SFC_VER_RK3588         0x4

/* SFC v4 reports an effectively unlimited per-transfer size. */
#define ROCKCHIP_SFC_MAX_IOSIZE_V4      0xffffffffU

/* Guard against a guest requesting an absurd transfer size. */
#define ROCKCHIP_SFC_MAX_BUFFER         (32 * MiB)

/* Modeled FIFO depth in 32-bit words. */
#define ROCKCHIP_SFC_FIFO_DEPTH         32

typedef struct RockchipSFCOp {
    uint32_t opcode;
    uint32_t addr;
    uint8_t addr_nbytes;
    uint8_t dummy_bytes;
    uint8_t dir;
    uint8_t cs;
    uint32_t len;
    bool cmd_sent;
    bool addr_sent;
    bool pending;
} RockchipSFCOp;

struct RockchipSFCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *spi;
    qemu_irq irq;
    qemu_irq *cs_lines;

    uint32_t ctrl;
    uint32_t imr;
    uint32_t risr;
    uint32_t ftlr;
    uint32_t ax;
    uint32_t abit;
    uint32_t len_ctrl;
    uint32_t len_ext;
    uint32_t dma_addr;
    uint32_t cmd_reg;
    uint32_t addr_reg;

    /* Pending SPI-memory operation. */
    RockchipSFCOp op;

    /* Read data pulled from the flash at command time, served via DATA. */
    uint8_t *rx;
    uint32_t rx_len;
    uint32_t rx_pos;

    /* Write data buffered from DATA / DMA, pushed into the flash. */
    uint8_t *tx;
    uint32_t tx_len;
};

#endif /* HW_SSI_ROCKCHIP_SFC_H */

/*
 * K230 DesignWare SSI controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_K230_SPI_H
#define HW_SSI_K230_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "system/block-backend.h"
#include "qom/object.h"

#define TYPE_K230_SPI "riscv.k230.spi"
OBJECT_DECLARE_SIMPLE_TYPE(K230SpiState, K230_SPI)

#define K230_SPI_SIZE 0x1000
#define K230_SPI_REG_COUNT (K230_SPI_SIZE / 4)
#define K230_SPI_FIFO_DEPTH 32
#define K230_SPI_RX_BUFFER_SIZE 65536
#define K230_SPI_IRQ_COUNT 9

struct K230SpiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq[K230_SPI_IRQ_COUNT];
    qemu_irq flash_cs;
    SSIBus *ssi;
    DeviceState *flash;
    BlockBackend *blk;

    uint32_t regs[K230_SPI_REG_COUNT];
    uint8_t tx_fifo[K230_SPI_FIFO_DEPTH];
    uint8_t rx_buf[K230_SPI_RX_BUFFER_SIZE];
    uint32_t tx_len;
    uint32_t rx_len;
    uint32_t rx_pos;
    bool idma_active;
};

#endif /* HW_SSI_K230_SPI_H */

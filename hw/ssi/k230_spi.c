/*
 * K230 DesignWare SSI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/ssi/k230_spi.h"

#define K230_SPI_CTRLR0         0x000
#define K230_SPI_CTRLR1         0x004
#define K230_SPI_SSIENR         0x008
#define K230_SPI_SER            0x010
#define K230_SPI_TXFTLR         0x018
#define K230_SPI_RXFTLR         0x01c
#define K230_SPI_TXFLR          0x020
#define K230_SPI_RXFLR          0x024
#define K230_SPI_SR             0x028
#define K230_SPI_IMR            0x02c
#define K230_SPI_ISR            0x030
#define K230_SPI_RISR           0x034
#define K230_SPI_TXOICR         0x038
#define K230_SPI_RXOICR         0x03c
#define K230_SPI_RXUICR         0x040
#define K230_SPI_MSTICR         0x044
#define K230_SPI_ICR            0x048
#define K230_SPI_DMACR          0x04c
#define K230_SPI_DMATDLR        0x050
#define K230_SPI_DMARDLR        0x054
#define K230_SPI_IDR            0x058
#define K230_SPI_VERSION        0x05c
#define K230_SPI_DR             0x060
#define K230_SPI_RX_SAMPLE_DLY  0x0f0
#define K230_SPI_SPI_CTRLR0     0x0f4
#define K230_SPI_DDR_DRIVE_EDGE 0x0f8
#define K230_SPI_SPIDR          0x120
#define K230_SPI_SPIAR          0x124
#define K230_SPI_AXIAR0         0x128
#define K230_SPI_AXIAR1         0x12c
#define K230_SPI_DONECR         0x134

#define K230_SPI_SR_TF_NOT_FULL BIT(1)
#define K230_SPI_SR_TF_EMPTY    BIT(2)
#define K230_SPI_SR_RF_NOT_EMPTY BIT(3)

#define K230_SPI_INT_TXEI       BIT(0)
#define K230_SPI_INT_TXOI       BIT(1)
#define K230_SPI_INT_RXUI       BIT(2)
#define K230_SPI_INT_RXOI       BIT(3)
#define K230_SPI_INT_RXFI       BIT(4)
#define K230_SPI_INT_MSTI       BIT(5)
#define K230_SPI_INT_DONE       BIT(11)

#define K230_SPI_DMACR_IDMAE    BIT(2)

#define K230_SPI_TMOD_SHIFT     10
#define K230_SPI_TMOD_MASK      (0x3 << K230_SPI_TMOD_SHIFT)
#define K230_SPI_TMOD_TO        1
#define K230_SPI_TMOD_RO        2
#define K230_SPI_TMOD_EPROMREAD 3

#define K230_SPI_CTRL0_INST_L_SHIFT 8
#define K230_SPI_CTRL0_INST_L_MASK  (0x3 << K230_SPI_CTRL0_INST_L_SHIFT)
#define K230_SPI_CTRL0_ADDR_L_SHIFT 2
#define K230_SPI_CTRL0_ADDR_L_MASK  (0xf << K230_SPI_CTRL0_ADDR_L_SHIFT)
#define K230_SPI_CTRL0_WAIT_SHIFT   11
#define K230_SPI_CTRL0_WAIT_MASK    (0x1f << K230_SPI_CTRL0_WAIT_SHIFT)

static uint32_t k230_spi_reg(K230SpiState *s, hwaddr addr)
{
    return s->regs[addr / 4];
}

static void k230_spi_set_reg(K230SpiState *s, hwaddr addr, uint32_t val)
{
    s->regs[addr / 4] = val;
}

static uint32_t k230_spi_rx_avail(K230SpiState *s)
{
    return s->rx_len - s->rx_pos;
}

static uint32_t k230_spi_rx_level(K230SpiState *s)
{
    return MIN(k230_spi_rx_avail(s), K230_SPI_FIFO_DEPTH);
}

static void k230_spi_update_irq(K230SpiState *s)
{
    uint32_t isr = k230_spi_reg(s, K230_SPI_RISR) &
                   k230_spi_reg(s, K230_SPI_IMR);

    qemu_set_irq(s->irq[0], isr ? 1 : 0);
}

static void k230_spi_clear_irqs(K230SpiState *s, uint32_t mask)
{
    k230_spi_set_reg(s, K230_SPI_RISR,
                     k230_spi_reg(s, K230_SPI_RISR) & ~mask);
    k230_spi_update_irq(s);
}

static uint8_t k230_spi_flash_transfer(K230SpiState *s, uint8_t val)
{
    return ssi_transfer(s->ssi, val) & 0xff;
}

static void k230_spi_flash_select(K230SpiState *s, bool select)
{
    qemu_set_irq(s->flash_cs, select ? 0 : 1);
}

static void k230_spi_fifo_reset(K230SpiState *s)
{
    s->tx_len = 0;
    s->rx_len = 0;
    s->rx_pos = 0;
}

static unsigned int k230_spi_tmode(K230SpiState *s)
{
    return (k230_spi_reg(s, K230_SPI_CTRLR0) & K230_SPI_TMOD_MASK) >>
           K230_SPI_TMOD_SHIFT;
}

static void k230_spi_standard_flush(K230SpiState *s)
{
    unsigned int tmode;
    unsigned int ndf;

    if (!(k230_spi_reg(s, K230_SPI_SSIENR) & 1) ||
        !k230_spi_reg(s, K230_SPI_SER) || !s->tx_len) {
        return;
    }

    tmode = k230_spi_tmode(s);
    k230_spi_flash_select(s, true);
    for (int i = 0; i < s->tx_len; i++) {
        k230_spi_flash_transfer(s, s->tx_fifo[i]);
    }
    s->tx_len = 0;

    if (tmode == K230_SPI_TMOD_RO ||
        tmode == K230_SPI_TMOD_EPROMREAD) {
        ndf = (k230_spi_reg(s, K230_SPI_CTRLR1) & 0xffff) + 1;
        s->rx_len = MIN(ndf, K230_SPI_RX_BUFFER_SIZE);
        s->rx_pos = 0;

        for (int i = 0; i < s->rx_len; i++) {
            s->rx_buf[i] = k230_spi_flash_transfer(s, 0);
        }
    }
}

static unsigned int k230_spi_inst_bytes(uint32_t spi_ctrlr0)
{
    switch ((spi_ctrlr0 & K230_SPI_CTRL0_INST_L_MASK) >>
            K230_SPI_CTRL0_INST_L_SHIFT) {
    case 3:
        return 2;
    case 2:
        return 1;
    default:
        return 0;
    }
}

static unsigned int k230_spi_addr_bytes(uint32_t spi_ctrlr0)
{
    return ((spi_ctrlr0 & K230_SPI_CTRL0_ADDR_L_MASK) >>
            K230_SPI_CTRL0_ADDR_L_SHIFT) / 2;
}

static unsigned int k230_spi_dummy_bytes(uint32_t spi_ctrlr0)
{
    unsigned int wait_cycles = (spi_ctrlr0 & K230_SPI_CTRL0_WAIT_MASK) >>
                               K230_SPI_CTRL0_WAIT_SHIFT;

    return DIV_ROUND_UP(wait_cycles, 8);
}

static void k230_spi_transfer_cmd_addr(K230SpiState *s)
{
    uint32_t spidr = k230_spi_reg(s, K230_SPI_SPIDR);
    uint32_t spiar = k230_spi_reg(s, K230_SPI_SPIAR);
    uint32_t spi_ctrlr0 = k230_spi_reg(s, K230_SPI_SPI_CTRLR0);
    unsigned int inst_bytes = k230_spi_inst_bytes(spi_ctrlr0);
    unsigned int addr_bytes = k230_spi_addr_bytes(spi_ctrlr0);
    unsigned int dummy_bytes = k230_spi_dummy_bytes(spi_ctrlr0);

    for (int i = 0; i < inst_bytes; i++) {
        k230_spi_flash_transfer(s, extract32(spidr, i * 8, 8));
    }

    for (int i = 0; i < addr_bytes; i++) {
        k230_spi_flash_transfer(s, extract32(spiar, i * 8, 8));
    }

    for (int i = 0; i < dummy_bytes; i++) {
        k230_spi_flash_transfer(s, 0);
    }
}

static void k230_spi_finish_idma(K230SpiState *s)
{
    k230_spi_set_reg(s, K230_SPI_RISR,
                     k230_spi_reg(s, K230_SPI_RISR) | K230_SPI_INT_DONE);
    s->idma_active = false;
    k230_spi_update_irq(s);
}

static void k230_spi_try_idma(K230SpiState *s)
{
    uint32_t dmacr = k230_spi_reg(s, K230_SPI_DMACR);
    uint32_t ndf = (k230_spi_reg(s, K230_SPI_CTRLR1) & 0xffff) + 1;
    uint64_t dma_addr = k230_spi_reg(s, K230_SPI_AXIAR0) |
                        ((uint64_t)k230_spi_reg(s, K230_SPI_AXIAR1) << 32);
    g_autofree uint8_t *buf = NULL;
    bool is_read;

    if (s->idma_active || !(dmacr & K230_SPI_DMACR_IDMAE) ||
        !(k230_spi_reg(s, K230_SPI_SSIENR) & 1) ||
        !k230_spi_reg(s, K230_SPI_SER)) {
        return;
    }

    s->idma_active = true;
    buf = g_malloc0(ndf);
    is_read = k230_spi_tmode(s) == K230_SPI_TMOD_RO;

    k230_spi_flash_select(s, true);
    k230_spi_transfer_cmd_addr(s);

    if (is_read) {
        for (int i = 0; i < ndf; i++) {
            buf[i] = k230_spi_flash_transfer(s, 0);
        }
        dma_memory_write(&address_space_memory, dma_addr, buf, ndf,
                         MEMTXATTRS_UNSPECIFIED);
    } else {
        if (dma_memory_read(&address_space_memory, dma_addr, buf, ndf,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
            for (int i = 0; i < ndf; i++) {
                k230_spi_flash_transfer(s, buf[i]);
            }
        }
    }

    k230_spi_flash_select(s, false);
    k230_spi_finish_idma(s);
}

static uint64_t k230_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230SpiState *s = K230_SPI(opaque);

    if (addr >= K230_SPI_SIZE || size != 4) {
        return 0;
    }

    switch (addr) {
    case K230_SPI_TXFLR:
        if ((k230_spi_reg(s, K230_SPI_SSIENR) & 1) &&
            k230_spi_reg(s, K230_SPI_SER) && !s->tx_len) {
            return 1;
        }
        return s->tx_len;
    case K230_SPI_RXFLR:
        return k230_spi_rx_level(s);
    case K230_SPI_SR:
        return K230_SPI_SR_TF_NOT_FULL | K230_SPI_SR_TF_EMPTY |
               (k230_spi_rx_avail(s) ? K230_SPI_SR_RF_NOT_EMPTY : 0);
    case K230_SPI_ISR:
        return k230_spi_reg(s, K230_SPI_RISR) &
               k230_spi_reg(s, K230_SPI_IMR);
    case K230_SPI_DR:
        if (k230_spi_rx_avail(s)) {
            return s->rx_buf[s->rx_pos++];
        }
        return 0xff;
    case K230_SPI_TXOICR:
        k230_spi_clear_irqs(s, K230_SPI_INT_TXOI);
        return 1;
    case K230_SPI_RXOICR:
        k230_spi_clear_irqs(s, K230_SPI_INT_RXOI);
        return 1;
    case K230_SPI_RXUICR:
        k230_spi_clear_irqs(s, K230_SPI_INT_RXUI);
        return 1;
    case K230_SPI_MSTICR:
        k230_spi_clear_irqs(s, K230_SPI_INT_MSTI);
        return 1;
    case K230_SPI_ICR:
        k230_spi_clear_irqs(s, K230_SPI_INT_TXOI | K230_SPI_INT_RXUI |
                            K230_SPI_INT_RXOI | K230_SPI_INT_MSTI |
                            K230_SPI_INT_DONE);
        return 1;
    case K230_SPI_DONECR:
        k230_spi_clear_irqs(s, K230_SPI_INT_DONE);
        return 1;
    default:
        return k230_spi_reg(s, addr);
    }
}

static void k230_spi_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    K230SpiState *s = K230_SPI(opaque);
    uint32_t value = val;

    if (addr >= K230_SPI_SIZE || size != 4) {
        return;
    }

    switch (addr) {
    case K230_SPI_SSIENR:
        k230_spi_set_reg(s, addr, value & 1);
        if (!(value & 1)) {
            k230_spi_fifo_reset(s);
            k230_spi_flash_select(s, false);
        }
        break;
    case K230_SPI_SER:
        k230_spi_set_reg(s, addr, value);
        if (!value) {
            k230_spi_flash_select(s, false);
        } else {
            k230_spi_standard_flush(s);
        }
        break;
    case K230_SPI_TXFTLR:
        k230_spi_set_reg(s, addr, MIN(value, K230_SPI_FIFO_DEPTH - 1));
        break;
    case K230_SPI_DR:
        if (s->tx_len < K230_SPI_FIFO_DEPTH) {
            s->tx_fifo[s->tx_len++] = value & 0xff;
        }
        if (k230_spi_reg(s, K230_SPI_SER)) {
            k230_spi_standard_flush(s);
        }
        break;
    case K230_SPI_DMACR:
        k230_spi_set_reg(s, addr, value);
        if (!(value & K230_SPI_DMACR_IDMAE)) {
            s->idma_active = false;
        }
        break;
    case K230_SPI_IMR:
        k230_spi_set_reg(s, addr, value);
        k230_spi_update_irq(s);
        break;
    default:
        k230_spi_set_reg(s, addr, value);
        break;
    }

    k230_spi_try_idma(s);
}

static const MemoryRegionOps k230_spi_ops = {
    .read = k230_spi_read,
    .write = k230_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void k230_spi_reset(DeviceState *dev)
{
    K230SpiState *s = K230_SPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    k230_spi_fifo_reset(s);
    s->idma_active = false;

    k230_spi_set_reg(s, K230_SPI_TXFTLR, 0);
    k230_spi_set_reg(s, K230_SPI_RXFTLR, 0);
    k230_spi_set_reg(s, K230_SPI_IDR, 0xffffffff);
    k230_spi_set_reg(s, K230_SPI_VERSION, 0x3430332a);
    k230_spi_flash_select(s, false);
    k230_spi_update_irq(s);
}

static const VMStateDescription vmstate_k230_spi = {
    .name = TYPE_K230_SPI,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, K230SpiState, K230_SPI_REG_COUNT),
        VMSTATE_UINT8_ARRAY(tx_fifo, K230SpiState, K230_SPI_FIFO_DEPTH),
        VMSTATE_UINT8_ARRAY(rx_buf, K230SpiState, K230_SPI_RX_BUFFER_SIZE),
        VMSTATE_UINT32(tx_len, K230SpiState),
        VMSTATE_UINT32(rx_len, K230SpiState),
        VMSTATE_UINT32(rx_pos, K230SpiState),
        VMSTATE_BOOL(idma_active, K230SpiState),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_spi_realize(DeviceState *dev, Error **errp)
{
    K230SpiState *s = K230_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    for (int i = 0; i < K230_SPI_IRQ_COUNT; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    s->ssi = ssi_create_bus(dev, "spi");
    s->flash = qdev_new("w25q256");
    if (s->blk) {
        if (!qdev_prop_set_drive_err(s->flash, "drive", s->blk, errp)) {
            return;
        }
    }
    ssi_realize_and_unref(s->flash, s->ssi, &error_fatal);
    s->flash_cs = qdev_get_gpio_in_named(s->flash, SSI_GPIO_CS, 0);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_spi_ops, s,
                          TYPE_K230_SPI, K230_SPI_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static const Property k230_spi_properties[] = {
    DEFINE_PROP_DRIVE("drive", K230SpiState, blk),
};

static void k230_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = k230_spi_realize;
    device_class_set_legacy_reset(dc, k230_spi_reset);
    device_class_set_props(dc, k230_spi_properties);
    dc->vmsd = &vmstate_k230_spi;
    dc->desc = "K230 DesignWare SSI controller";
}

static const TypeInfo k230_spi_type_info = {
    .name = TYPE_K230_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SpiState),
    .class_init = k230_spi_class_init,
};

static void k230_spi_register_types(void)
{
    type_register_static(&k230_spi_type_info);
}

type_init(k230_spi_register_types)

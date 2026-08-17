/*
 * Rockchip SPI controller (rk3066/rk3588) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register-level model for the spi-rockchip driver in transmit/receive
 * modes.  Transfers are executed synchronously against a child SSI bus
 * (the RK806 PMIC on SPI2): TXDR writes clock bytes out in full-duplex
 * mode and fill the RX FIFO; read-only transfers are synthesized when
 * the chip is enabled with TMOD=RO and CTRLR1 set.  The INT_RF_FULL
 * interrupt drives the driver's PIO/IRQ completion path.
 *
 * Register map (drivers/spi/spi-rockchip.c):
 *   CTRLR0 0x00  CTRLR1 0x04  SSIENR 0x08  SER 0x0c  BAUDR 0x10
 *   TXFTLR 0x14  RXFTLR 0x18  TXFLR 0x1c  RXFLR 0x20  SR 0x24
 *   IPR 0x28  IMR 0x2c  ISR 0x30  RISR 0x34  ICR 0x38
 *   DMACR 0x3c  DMATDLR 0x40  DMARDLR 0x44  VERSION 0x48
 *   TXDR 0x400  RXDR 0x800
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/ssi/rockchip_spi.h"
#include "migration/vmstate.h"

#define SPI_CTRLR0 0x00
#define SPI_CTRLR1 0x04
#define SPI_SSIENR 0x08
#define SPI_SER    0x0c
#define SPI_BAUDR  0x10
#define SPI_TXFTLR 0x14
#define SPI_RXFTLR 0x18
#define SPI_TXFLR  0x1c
#define SPI_RXFLR  0x20
#define SPI_SR     0x24
#define SPI_IPR    0x28
#define SPI_IMR    0x2c
#define SPI_ISR    0x30
#define SPI_RISR   0x34
#define SPI_ICR    0x38
#define SPI_DMACR  0x3c
#define SPI_DMATDLR 0x40
#define SPI_DMARDLR 0x44
#define SPI_VERSION 0x48
#define SPI_TXDR   0x400
#define SPI_RXDR   0x800

#define CR0_XFM_SHIFT 18
#define CR0_XFM_TR 0
#define CR0_XFM_TO 1
#define CR0_XFM_RO 2

#define SR_TF_EMPTY (1 << 2)
#define SR_RF_EMPTY (1 << 3)
#define SR_RF_FULL  (1 << 4)

#define INT_TF_EMPTY  (1 << 0)
#define INT_RF_FULL   (1 << 4)

#define SPI_VERSION_RK3588 0x00110002

static void rockchip_spi_update_irq(RockchipSPIState *s)
{
    qemu_set_irq(s->irq, (s->isr & s->imr) != 0);
}

static void rockchip_spi_update_status(RockchipSPIState *s)
{
    unsigned int rxftlr = (s->rxftlr + 1) & 0x3f;

    s->isr = INT_TF_EMPTY;
    if (s->rx_level > rxftlr) {
        s->isr |= INT_RF_FULL;
    }
    rockchip_spi_update_irq(s);
}

/* Push one received byte into the RX FIFO. */
static void rockchip_spi_rx_push(RockchipSPIState *s, uint8_t byte)
{
    if (s->rx_level >= ROCKCHIP_SPI_FIFO_DEPTH) {
        return;
    }
    s->rx_fifo[(s->rx_pos + s->rx_level) % ROCKCHIP_SPI_FIFO_DEPTH] = byte;
    s->rx_level++;
}

static uint8_t rockchip_spi_rx_pop(RockchipSPIState *s)
{
    uint8_t byte = 0;

    if (s->rx_level) {
        byte = s->rx_fifo[s->rx_pos];
        s->rx_pos = (s->rx_pos + 1) % ROCKCHIP_SPI_FIFO_DEPTH;
        s->rx_level--;
    }
    return byte;
}

/* TXDR write: clock one byte out (and receive one in full-duplex mode). */
static void rockchip_spi_tx_write(RockchipSPIState *s, uint8_t byte)
{
    uint8_t rx = ssi_transfer(s->spi, byte);
    unsigned int xfm = (s->ctrlr0 >> CR0_XFM_SHIFT) & 0x3;

    if (xfm == CR0_XFM_TR) {
        rockchip_spi_rx_push(s, rx);
    }
    rockchip_spi_update_status(s);
}

/*
 * Read-only transfer: the controller generates the clocks itself after
 * being enabled; synthesize the whole transfer into the RX FIFO.
 */
static void rockchip_spi_synth_read(RockchipSPIState *s)
{
    unsigned int words = (s->ctrlr1 + 1) & 0xffff;
    unsigned int i;

    if (words > ROCKCHIP_SPI_FIFO_DEPTH) {
        words = ROCKCHIP_SPI_FIFO_DEPTH;
    }
    for (i = 0; i < words; i++) {
        rockchip_spi_rx_push(s, ssi_transfer(s->spi, 0));
    }
    rockchip_spi_update_status(s);
}

static uint64_t rockchip_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipSPIState *s = opaque;
    uint32_t value = 0;

    switch (offset) {
    case SPI_CTRLR0:
        value = s->ctrlr0;
        break;
    case SPI_CTRLR1:
        value = s->ctrlr1;
        break;
    case SPI_SSIENR:
        value = s->ssi_enr;
        break;
    case SPI_BAUDR:
        value = s->baudr;
        break;
    case SPI_TXFTLR:
        value = s->txftlr;
        break;
    case SPI_RXFTLR:
        value = s->rxftlr;
        break;
    case SPI_TXFLR:
        /* The model drains the TX FIFO synchronously. */
        value = 0;
        break;
    case SPI_RXFLR:
        value = s->rx_level;
        break;
    case SPI_SR:
        value = SR_TF_EMPTY;
        if (s->rx_level == 0) {
            value |= SR_RF_EMPTY;
        }
        if (s->rx_level >= ROCKCHIP_SPI_FIFO_DEPTH) {
            value |= SR_RF_FULL;
        }
        break;
    case SPI_IMR:
        value = s->imr;
        break;
    case SPI_ISR:
    case SPI_RISR:
        value = s->isr;
        break;
    case SPI_VERSION:
        value = SPI_VERSION_RK3588;
        break;
    case SPI_RXDR:
        value = rockchip_spi_rx_pop(s);
        rockchip_spi_update_status(s);
        break;
    default:
        break;
    }

    return value;
}

static void rockchip_spi_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RockchipSPIState *s = opaque;

    switch (offset) {
    case SPI_CTRLR0:
        s->ctrlr0 = value;
        break;
    case SPI_CTRLR1:
        s->ctrlr1 = value;
        break;
    case SPI_SSIENR:
        s->ssi_enr = value;
        if (value & 1) {
            unsigned int xfm = (s->ctrlr0 >> CR0_XFM_SHIFT) & 0x3;

            if (xfm == CR0_XFM_RO) {
                rockchip_spi_synth_read(s);
            }
        }
        break;
    case SPI_SER:
    case SPI_BAUDR:
        s->baudr = value;
        break;
    case SPI_TXFTLR:
        s->txftlr = value;
        break;
    case SPI_RXFTLR:
        s->rxftlr = value;
        break;
    case SPI_IMR:
        s->imr = value;
        rockchip_spi_update_irq(s);
        break;
    case SPI_ICR:
        /* Clear latched status; RF_FULL is level-derived. */
        rockchip_spi_update_status(s);
        break;
    case SPI_DMACR:
    case SPI_DMATDLR:
    case SPI_DMARDLR:
    case SPI_IPR:
        break;
    case SPI_TXDR:
        rockchip_spi_tx_write(s, value & 0xff);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%"
                      HWADDR_PRIx " = 0x%" PRIx64 "\n", __func__,
                      offset, value);
        break;
    }
}

static const MemoryRegionOps rockchip_spi_ops = {
    .read = rockchip_spi_read,
    .write = rockchip_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rockchip_spi_realize(DeviceState *dev, Error **errp)
{
    RockchipSPIState *s = ROCKCHIP_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->spi = ssi_create_bus(dev, "spi");
    sysbus_init_irq(sbd, &s->irq);
    memory_region_init_io(&s->iomem, OBJECT(dev), &rockchip_spi_ops, s,
                          "rockchip-spi", ROCKCHIP_SPI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rockchip_spi_reset(DeviceState *dev)
{
    RockchipSPIState *s = ROCKCHIP_SPI(dev);

    s->ctrlr0 = 0;
    s->ctrlr1 = 0;
    s->ssi_enr = 0;
    s->baudr = 0;
    s->txftlr = 0;
    s->rxftlr = 0;
    s->imr = 0;
    s->isr = 0;
    s->rx_level = 0;
    s->rx_pos = 0;
}

static const VMStateDescription vmstate_rockchip_spi = {
    .name = "rockchip-spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrlr0, RockchipSPIState),
        VMSTATE_UINT32(ctrlr1, RockchipSPIState),
        VMSTATE_UINT32(ssi_enr, RockchipSPIState),
        VMSTATE_UINT32(baudr, RockchipSPIState),
        VMSTATE_UINT32(txftlr, RockchipSPIState),
        VMSTATE_UINT32(rxftlr, RockchipSPIState),
        VMSTATE_UINT32(imr, RockchipSPIState),
        VMSTATE_UINT32(isr, RockchipSPIState),
        VMSTATE_UINT8_ARRAY(rx_fifo, RockchipSPIState, ROCKCHIP_SPI_FIFO_DEPTH),
        VMSTATE_UINT32(rx_level, RockchipSPIState),
        VMSTATE_UINT32(rx_pos, RockchipSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void rockchip_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rockchip_spi_realize;
    device_class_set_legacy_reset(dc, rockchip_spi_reset);
    dc->vmsd = &vmstate_rockchip_spi;
    dc->user_creatable = false;
}

static const TypeInfo rockchip_spi_info = {
    .name = TYPE_ROCKCHIP_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipSPIState),
    .class_init = rockchip_spi_class_init,
};

static void rockchip_spi_register_types(void)
{
    type_register_static(&rockchip_spi_info);
}

type_init(rockchip_spi_register_types)

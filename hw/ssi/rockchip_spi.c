/*
 * Rockchip SPI controller with FIFO, interrupt and PL330 DMA handshakes.
 * Copyright (c) 2026 Process Mission
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/ssi/rockchip_spi.h"
#include "migration/vmstate.h"
#include "system/qtest.h"

enum {
    SPI_CTRLR0 = 0x00, SPI_CTRLR1 = 0x04, SPI_SSIENR = 0x08,
    SPI_SER = 0x0c, SPI_BAUDR = 0x10, SPI_TXFTLR = 0x14,
    SPI_RXFTLR = 0x18, SPI_TXFLR = 0x1c, SPI_RXFLR = 0x20,
    SPI_SR = 0x24, SPI_IPR = 0x28, SPI_IMR = 0x2c,
    SPI_ISR = 0x30, SPI_RISR = 0x34, SPI_ICR = 0x38,
    SPI_DMACR = 0x3c, SPI_DMATDLR = 0x40, SPI_DMARDLR = 0x44,
    SPI_VERSION = 0x48, SPI_TXDR = 0x400, SPI_RXDR = 0x800,
};

#define CR0_XFM(s) (((s)->ctrlr0 >> 18) & 3)
#define CR0_XFM_TR 0
#define CR0_XFM_TO 1
#define CR0_XFM_RO 2
#define SR_BUSY BIT(0)
#define SR_TF_FULL BIT(1)
#define SR_TF_EMPTY BIT(2)
#define SR_RF_EMPTY BIT(3)
#define SR_RF_FULL BIT(4)
#define INT_TF_EMPTY BIT(0)
#define INT_TF_OVER BIT(1)
#define INT_RF_UNDER BIT(2)
#define INT_RF_OVER BIT(3)
#define INT_RF_FULL BIT(4)
#define INT_ERRORS (INT_TF_OVER | INT_RF_UNDER | INT_RF_OVER)
#define DMA_RX_ENABLE BIT(0)
#define DMA_TX_ENABLE BIT(1)
#define SPI_VERSION_RK3588 0x00110002

static unsigned spi_frame_bits(RockchipSPIState *s)
{
    unsigned dfs = s->ctrlr0 & 3;

    return dfs == 2 ? 16 : dfs == 1 ? 8 : 4;
}

static uint64_t spi_clock_hz(RockchipSPIState *s)
{
    return clock_has_source(s->clock) ? clock_get_hz(s->clock) : s->clock_hz;
}

static bool spi_can_transfer(RockchipSPIState *s)
{
    if (!(s->ssi_enr & 1) || !s->ser || !spi_clock_hz(s)) {
        return false;
    }
    if (CR0_XFM(s) != CR0_XFM_TO && s->rx_level == ROCKCHIP_SPI_FIFO_DEPTH) {
        return false;
    }
    return CR0_XFM(s) == CR0_XFM_RO ? s->remaining != 0 : s->tx_level != 0;
}

static void rockchip_spi_schedule(RockchipSPIState *s)
{
    uint64_t delay;
    unsigned count;

    if (!spi_can_transfer(s) || timer_pending(s->timer)) {
        return;
    }
    /* Batch wire clocks only up to the next observable IRQ or DMA boundary. */
    count = CR0_XFM(s) == CR0_XFM_RO ? s->remaining : s->tx_level;
    count = MIN(count, ROCKCHIP_SPI_FIFO_DEPTH);
    if (CR0_XFM(s) != CR0_XFM_TO) {
        count = MIN(count, ROCKCHIP_SPI_FIFO_DEPTH - s->rx_level);
        if ((s->dmacr & DMA_RX_ENABLE) && s->rx_level <= s->dmardlr) {
            count = MIN(count, s->dmardlr + 1 - s->rx_level);
        }
        if ((s->imr & INT_RF_FULL) && s->rx_level <= s->rxftlr) {
            count = MIN(count, s->rxftlr + 1 - s->rx_level);
        }
    }
    if (CR0_XFM(s) != CR0_XFM_RO) {
        if ((s->dmacr & DMA_TX_ENABLE) && s->tx_level > s->dmatdlr) {
            count = MIN(count, s->tx_level - s->dmatdlr);
        }
        if ((s->imr & INT_TF_EMPTY) && s->tx_level > s->txftlr) {
            count = MIN(count, s->tx_level - s->txftlr);
        }
    }
    s->scheduled_frames = count;
    delay = (uint64_t)count * spi_frame_bits(s) * MAX(s->baudr & 0xfffe, 2) *
            NANOSECONDS_PER_SECOND / spi_clock_hz(s);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MAX(delay, 1));
}

static void rockchip_spi_update(RockchipSPIState *s)
{
    bool enabled = (s->ssi_enr & 1) != 0;

    s->isr &= INT_ERRORS;
    if (s->tx_level <= s->txftlr) {
        s->isr |= INT_TF_EMPTY;
    }
    if (s->rx_level > s->rxftlr) {
        s->isr |= INT_RF_FULL;
    }
    qemu_set_irq(s->irq, (s->isr & s->imr) != 0);
    qemu_set_irq(s->dma_busy[0], !(enabled && (s->dmacr & DMA_TX_ENABLE) &&
                                  s->tx_level <= s->dmatdlr));
    qemu_set_irq(s->dma_busy[1], !(enabled && (s->dmacr & DMA_RX_ENABLE) &&
                                  s->rx_level > s->dmardlr));
    for (unsigned i = 0; i < ARRAY_SIZE(s->cs); i++) {
        bool active_high = (s->ctrlr0 >> (23 + i)) & 1;
        bool selected = (s->ser >> i) & 1;

        qemu_set_irq(s->cs[i], selected == active_high);
    }
    rockchip_spi_schedule(s);
}

static void rockchip_spi_transfer(void *opaque)
{
    RockchipSPIState *s = opaque;
    unsigned count = s->scheduled_frames;

    s->scheduled_frames = 0;
    for (unsigned i = 0; i < count && spi_can_transfer(s); i++) {
        uint16_t tx = 0, rx;
        if (CR0_XFM(s) == CR0_XFM_RO) {
            s->remaining--;
        } else {
            tx = s->tx_fifo[s->tx_pos];
            s->tx_pos = (s->tx_pos + 1) % ROCKCHIP_SPI_FIFO_DEPTH;
            s->tx_level--;
        }
        rx = ssi_transfer(s->spi, tx) & ((1U << spi_frame_bits(s)) - 1);
        if (CR0_XFM(s) != CR0_XFM_TO) {
            s->rx_fifo[(s->rx_pos + s->rx_level) %
                        ROCKCHIP_SPI_FIFO_DEPTH] = rx;
            s->rx_level++;
        }
    }
    rockchip_spi_update(s);
}

static uint32_t rockchip_spi_status(RockchipSPIState *s)
{
    uint32_t value = 0;

    if ((s->ssi_enr & 1) &&
        (s->tx_level || (CR0_XFM(s) == CR0_XFM_RO && s->remaining))) {
        value |= SR_BUSY;
    }
    if (!s->tx_level) {
        value |= SR_TF_EMPTY;
    }
    if (s->tx_level == ROCKCHIP_SPI_FIFO_DEPTH) {
        value |= SR_TF_FULL;
    }
    if (!s->rx_level) {
        value |= SR_RF_EMPTY;
    }
    if (s->rx_level == ROCKCHIP_SPI_FIFO_DEPTH) {
        value |= SR_RF_FULL;
    }
    return value;
}

static uint64_t rockchip_spi_pre_write(RegisterInfo *reg, uint64_t val)
{
    RockchipSPIState *s = ROCKCHIP_SPI(reg->opaque);

    switch (reg->access->addr) {
    case SPI_CTRLR1:
        return val & 0xffff;
    case SPI_SSIENR:
        if (!(s->ssi_enr & 1) && (val & 1)) {
            s->remaining = s->ctrlr1 + 1;
        }
        return val & 1;
    case SPI_SER:
        return val & 3;
    case SPI_BAUDR:
        return val & 0xfffe;
    case SPI_TXFTLR:
    case SPI_RXFTLR:
    case SPI_DMATDLR:
    case SPI_DMARDLR:
        return MIN(val, ROCKCHIP_SPI_FIFO_DEPTH - 1);
    case SPI_DMACR:
        return val & 3;
    default:
        return val;
    }
}

static void rockchip_spi_post_write(RegisterInfo *reg, uint64_t val)
{
    RockchipSPIState *s = ROCKCHIP_SPI(reg->opaque);

    if (device_is_in_reset(DEVICE(s))) {
        return;
    }
    switch (reg->access->addr) {
    case SPI_SSIENR:
        if (!(val & 1)) {
            timer_del(s->timer);
            s->tx_level = s->rx_level = 0;
            s->tx_pos = s->rx_pos = 0;
            s->remaining = 0;
            s->scheduled_frames = 0;
            s->isr = 0;
        }
        break;
    case SPI_ICR:
        s->isr &= ~(val & INT_ERRORS);
        break;
    default:
        break;
    }
    rockchip_spi_update(s);
}

static uint64_t rockchip_spi_post_read(RegisterInfo *reg, uint64_t val)
{
    RockchipSPIState *s = ROCKCHIP_SPI(reg->opaque);

    switch (reg->access->addr) {
    case SPI_TXFLR:
        return s->tx_level;
    case SPI_RXFLR:
        return s->rx_level;
    case SPI_SR:
        return rockchip_spi_status(s);
    case SPI_ISR:
        return s->isr & s->imr;
    case SPI_RISR:
        return s->isr;
    case SPI_ICR:
        return 0;
    case SPI_VERSION:
        return SPI_VERSION_RK3588;
    default:
        return val;
    }
}

static const RegisterAccessInfo rockchip_spi_regs_info[] = {
    { .name = "CTRLR0", .addr = SPI_CTRLR0,
      .post_write = rockchip_spi_post_write },
    { .name = "CTRLR1", .addr = SPI_CTRLR1,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "SSIENR", .addr = SPI_SSIENR,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "SER", .addr = SPI_SER,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "BAUDR", .addr = SPI_BAUDR,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "TXFTLR", .addr = SPI_TXFTLR,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "RXFTLR", .addr = SPI_RXFTLR,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "TXFLR", .addr = SPI_TXFLR, .ro = UINT32_MAX,
      .post_read = rockchip_spi_post_read,
      .post_write = rockchip_spi_post_write },
    { .name = "RXFLR", .addr = SPI_RXFLR, .ro = UINT32_MAX,
      .post_read = rockchip_spi_post_read,
      .post_write = rockchip_spi_post_write },
    { .name = "SR", .addr = SPI_SR, .ro = UINT32_MAX,
      .post_read = rockchip_spi_post_read,
      .post_write = rockchip_spi_post_write },
    { .name = "IPR", .addr = SPI_IPR,
      .post_write = rockchip_spi_post_write },
    { .name = "IMR", .addr = SPI_IMR,
      .post_write = rockchip_spi_post_write },
    { .name = "ISR", .addr = SPI_ISR, .ro = UINT32_MAX,
      .post_read = rockchip_spi_post_read,
      .post_write = rockchip_spi_post_write },
    { .name = "RISR", .addr = SPI_RISR, .ro = UINT32_MAX,
      .post_read = rockchip_spi_post_read,
      .post_write = rockchip_spi_post_write },
    { .name = "ICR", .addr = SPI_ICR,
      .post_read = rockchip_spi_post_read,
      .post_write = rockchip_spi_post_write },
    { .name = "DMACR", .addr = SPI_DMACR,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "DMATDLR", .addr = SPI_DMATDLR,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "DMARDLR", .addr = SPI_DMARDLR,
      .pre_write = rockchip_spi_pre_write,
      .post_write = rockchip_spi_post_write },
    { .name = "VERSION", .addr = SPI_VERSION, .ro = UINT32_MAX,
      .post_read = rockchip_spi_post_read,
      .post_write = rockchip_spi_post_write },
};

static bool rockchip_spi_is_readonly(hwaddr offset)
{
    switch (offset) {
    case SPI_TXFLR:
    case SPI_RXFLR:
    case SPI_SR:
    case SPI_ISR:
    case SPI_RISR:
    case SPI_VERSION:
        return true;
    default:
        return false;
    }
}

static RegisterInfo *rockchip_spi_find_register(RegisterInfoArray *reg_array,
                                                hwaddr offset)
{
    for (unsigned i = 0; i < reg_array->num_elements; i++) {
        RegisterInfo *reg = reg_array->r[i];

        if (reg->access->addr == offset) {
            return reg;
        }
    }
    return NULL;
}

static void rockchip_spi_log_unimplemented(hwaddr offset, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP,
                  "%s: write at 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n",
                  TYPE_ROCKCHIP_SPI, offset, value);
}

static uint64_t rockchip_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RockchipSPIState *s = ROCKCHIP_SPI(register_array_get_owner(reg_array));
    uint32_t value;

    if (offset == SPI_RXDR) {
        value = 0;
        if (s->rx_level) {
            value = s->rx_fifo[s->rx_pos];
            s->rx_pos = (s->rx_pos + 1) % ROCKCHIP_SPI_FIFO_DEPTH;
            s->rx_level--;
        } else {
            s->isr |= INT_RF_UNDER;
        }
        rockchip_spi_update(s);
        return value;
    }

    if (rockchip_spi_find_register(reg_array, offset)) {
        return register_read_memory(reg_array, offset, size);
    }

    return 0;
}

static void rockchip_spi_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RockchipSPIState *s = ROCKCHIP_SPI(register_array_get_owner(reg_array));

    if (offset == SPI_TXDR) {
        if (s->tx_level == ROCKCHIP_SPI_FIFO_DEPTH) {
            s->isr |= INT_TF_OVER;
        } else {
            s->tx_fifo[(s->tx_pos + s->tx_level) %
                        ROCKCHIP_SPI_FIFO_DEPTH] = value;
            s->tx_level++;
        }
        rockchip_spi_update(s);
        return;
    }

    if (rockchip_spi_find_register(reg_array, offset)) {
        if (rockchip_spi_is_readonly(offset)) {
            rockchip_spi_log_unimplemented(offset, value);
        }
        /*
         * The previous model stored the whole register word for every
         * access width; keep that behaviour instead of merging sub-word
         * writes into the register file.
         */
        register_write_memory(reg_array, offset, value, 4);
        return;
    }

    rockchip_spi_log_unimplemented(offset, value);
    rockchip_spi_update(s);
}

static const MemoryRegionOps rockchip_spi_ops = {
    .read = rockchip_spi_read,
    .write = rockchip_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void rockchip_spi_clock_update(void *opaque, ClockEvent event)
{
    RockchipSPIState *s = opaque;

    timer_del(s->timer);
    rockchip_spi_schedule(s);
}

static void rockchip_spi_init(Object *obj)
{
    RockchipSPIState *s = ROCKCHIP_SPI(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rockchip_spi_transfer, s);
    s->clock = qdev_init_clock_in(DEVICE(s), "spiclk",
                                  rockchip_spi_clock_update, s, ClockUpdate);
    qdev_init_gpio_out_named(DEVICE(s), s->dma_busy, "dma-busy", 2);
    qdev_init_gpio_out_named(DEVICE(s), s->cs, "cs", 2);
}

static void rockchip_spi_realize(DeviceState *dev, Error **errp)
{
    RockchipSPIState *s = ROCKCHIP_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->spi = ssi_create_bus(dev, "spi");
    sysbus_init_irq(sbd, &s->irq);
    s->reg_array = register_init_block32(dev, rockchip_spi_regs_info,
                                         ARRAY_SIZE(rockchip_spi_regs_info),
                                         s->regs_info, s->regs,
                                         &rockchip_spi_ops, false,
                                         ROCKCHIP_SPI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
}

static void rockchip_spi_reset(DeviceState *dev)
{
    RockchipSPIState *s = ROCKCHIP_SPI(dev);
    unsigned i;

    timer_del(s->timer);
    for (i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    s->remaining = s->tx_level = s->tx_pos = s->rx_level = s->rx_pos = 0;
    s->scheduled_frames = 0;
    rockchip_spi_update(s);
}

static int rockchip_spi_post_load(void *opaque, int version_id)
{
    RockchipSPIState *s = opaque;

    if (s->tx_level > ROCKCHIP_SPI_FIFO_DEPTH ||
        s->rx_level > ROCKCHIP_SPI_FIFO_DEPTH ||
        s->tx_pos >= ROCKCHIP_SPI_FIFO_DEPTH ||
        s->rx_pos >= ROCKCHIP_SPI_FIFO_DEPTH ||
        s->scheduled_frames > ROCKCHIP_SPI_FIFO_DEPTH) {
        return -EINVAL;
    }
    /*
     * Recompute the interrupt outputs from the migrated registers.  The
     * qtest GIC input intercept aborts on a raise event for an IRQ above
     * MAX_IRQ, so skip the output update under qtest.
     */
    if (!qtest_enabled()) {
        rockchip_spi_update(s);
    }
    return 0;
}

static const VMStateDescription vmstate_rockchip_spi = {
    .name = TYPE_ROCKCHIP_SPI,
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = rockchip_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrlr0, RockchipSPIState),
        VMSTATE_UINT32(ctrlr1, RockchipSPIState),
        VMSTATE_UINT32(ssi_enr, RockchipSPIState),
        VMSTATE_UINT32(ser, RockchipSPIState),
        VMSTATE_UINT32(baudr, RockchipSPIState),
        VMSTATE_UINT32(txftlr, RockchipSPIState),
        VMSTATE_UINT32(rxftlr, RockchipSPIState),
        VMSTATE_UINT32(imr, RockchipSPIState),
        VMSTATE_UINT32(isr, RockchipSPIState),
        VMSTATE_UINT32(dmacr, RockchipSPIState),
        VMSTATE_UINT32(dmatdlr, RockchipSPIState),
        VMSTATE_UINT32(dmardlr, RockchipSPIState),
        VMSTATE_UINT32(ipr, RockchipSPIState),
        VMSTATE_UINT32(remaining, RockchipSPIState),
        VMSTATE_UINT32(scheduled_frames, RockchipSPIState),
        VMSTATE_UINT16_ARRAY(tx_fifo, RockchipSPIState,
                             ROCKCHIP_SPI_FIFO_DEPTH),
        VMSTATE_UINT16_ARRAY(rx_fifo, RockchipSPIState,
                             ROCKCHIP_SPI_FIFO_DEPTH),
        VMSTATE_UINT32(tx_level, RockchipSPIState),
        VMSTATE_UINT32(tx_pos, RockchipSPIState),
        VMSTATE_UINT32(rx_level, RockchipSPIState),
        VMSTATE_UINT32(rx_pos, RockchipSPIState),
        VMSTATE_TIMER_PTR(timer, RockchipSPIState),
        VMSTATE_CLOCK(clock, RockchipSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rockchip_spi_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", RockchipSPIState, clock_hz, 200000000),
};

static void rockchip_spi_finalize(Object *obj)
{
    timer_free(ROCKCHIP_SPI(obj)->timer);
}

static void rockchip_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rockchip_spi_realize;
    device_class_set_legacy_reset(dc, rockchip_spi_reset);
    device_class_set_props(dc, rockchip_spi_properties);
    dc->vmsd = &vmstate_rockchip_spi;
    dc->user_creatable = false;
}

static const TypeInfo rockchip_spi_info = {
    .name = TYPE_ROCKCHIP_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipSPIState),
    .instance_init = rockchip_spi_init,
    .instance_finalize = rockchip_spi_finalize,
    .class_init = rockchip_spi_class_init,
};

static void rockchip_spi_register_types(void)
{
    type_register_static(&rockchip_spi_info);
}

type_init(rockchip_spi_register_types)

/*
 * STC8G1K08A SPI
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/clock.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/stc8g_spi.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "trace.h"

REG8(SPSTAT, 0)
    FIELD(SPSTAT, SPIF, 7, 1)
    FIELD(SPSTAT, WCOL, 6, 1)
REG8(SPCTL, 0)
    FIELD(SPCTL, SSIG, 7, 1)
    FIELD(SPCTL, SPEN, 6, 1)
    FIELD(SPCTL, DORD, 5, 1)
    FIELD(SPCTL, MSTR, 4, 1)
    FIELD(SPCTL, CPOL, 3, 1)
    FIELD(SPCTL, CPHA, 2, 1)
    FIELD(SPCTL, SPR, 0, 2)
REG8(SPDAT, 0)

#define STC8G_SPI_REGS 3

enum Stc8gSPIRegister {
    STC8G_SPI_REG_STAT,
    STC8G_SPI_REG_CTL,
    STC8G_SPI_REG_DATA,
};

struct Stc8gSPIState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[STC8G_SPI_REGS];
    RegisterInfo regs_info[STC8G_SPI_REGS];
    uint8_t regs[STC8G_SPI_REGS];
    QEMUTimer *timer;
    Clock *sysclk;
    SSIBus *ssi;
    qemu_irq irq;
    uint32_t clock_frequency;
    uint8_t tx_data;
    uint64_t transfer_cycles;
    uint64_t clock_remainder;
    int64_t last_ns;
    bool transfer_active;
    bool ss_level;
};

static bool stc8g_spi_enabled(Stc8gSPIState *s)
{
    return FIELD_EX8(s->regs[STC8G_SPI_REG_CTL], SPCTL, SPEN);
}

static bool stc8g_spi_master(Stc8gSPIState *s)
{
    return FIELD_EX8(s->regs[STC8G_SPI_REG_CTL], SPCTL, MSTR);
}

static bool stc8g_spi_ss_ignored(Stc8gSPIState *s)
{
    return FIELD_EX8(s->regs[STC8G_SPI_REG_CTL], SPCTL, SSIG);
}

static bool stc8g_spi_slave_selected(Stc8gSPIState *s)
{
    return stc8g_spi_ss_ignored(s) || !s->ss_level;
}

static void stc8g_spi_update_irq(Stc8gSPIState *s)
{
    qemu_set_irq(s->irq, FIELD_EX8(s->regs[STC8G_SPI_REG_STAT],
                                   SPSTAT, SPIF));
}

static uint64_t stc8g_spi_transfer_cycles(Stc8gSPIState *s)
{
    unsigned divisor = 4 << FIELD_EX8(s->regs[STC8G_SPI_REG_CTL],
                                       SPCTL, SPR);

    return 8 * (uint64_t)divisor;
}

static void stc8g_spi_sync(Stc8gSPIState *s);
static void stc8g_spi_schedule(Stc8gSPIState *s);

static void stc8g_spi_clock_update(void *opaque, ClockEvent event)
{
    Stc8gSPIState *s = opaque;

    if (event == ClockPreUpdate) {
        stc8g_spi_sync(s);
        return;
    }
    s->clock_frequency = clock_get_hz(s->sysclk);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc8g_spi_schedule(s);
}

static void stc8g_spi_complete(void *opaque)
{
    Stc8gSPIState *s = opaque;
    uint8_t data = s->regs[STC8G_SPI_REG_DATA];

    s->transfer_active = false;
    if (stc8g_spi_enabled(s) && stc8g_spi_master(s)) {
        data = ssi_transfer(s->ssi, s->tx_data);
        s->regs[STC8G_SPI_REG_DATA] = data;
        s->regs[STC8G_SPI_REG_STAT] = FIELD_DP8(
            s->regs[STC8G_SPI_REG_STAT], SPSTAT, SPIF, 1);
        trace_stc8g_spi_complete(s->tx_data, data);
    }
    stc8g_spi_update_irq(s);
}

static void stc8g_spi_sync(Stc8gSPIState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->transfer_active && clock_is_enabled(s->sysclk)) {
        uint64_t cycles = mcs51_clock_elapsed_cycles(
            s->sysclk, now - s->last_ns, &s->clock_remainder);
        if (cycles >= s->transfer_cycles) {
            s->transfer_cycles = 0;
            s->last_ns = now;
            timer_del(s->timer);
            stc8g_spi_complete(s);
            return;
        }
        s->transfer_cycles -= cycles;
    }
    s->last_ns = now;
}

static void stc8g_spi_schedule(Stc8gSPIState *s)
{
    uint64_t delta;

    timer_del(s->timer);
    if (!s->transfer_active || !clock_is_enabled(s->sysclk)) {
        return;
    }
    delta = mcs51_clock_cycles_to_ns(s->sysclk, s->transfer_cycles,
                                     s->clock_remainder);
    timer_mod_ns(s->timer, s->last_ns + MAX(1ull, delta));
}

static void stc8g_spi_expire(void *opaque)
{
    Stc8gSPIState *s = opaque;

    stc8g_spi_sync(s);
    stc8g_spi_schedule(s);
}

static void stc8g_spi_start(Stc8gSPIState *s)
{
    s->tx_data = s->regs[STC8G_SPI_REG_DATA];
    s->transfer_cycles = stc8g_spi_transfer_cycles(s);
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->transfer_active = true;
    stc8g_spi_schedule(s);
    trace_stc8g_spi_start(s->tx_data,
                          mcs51_clock_cycles_to_ns(s->sysclk,
                                                   s->transfer_cycles, 0));
}

static uint64_t stc8g_spi_data_pre_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gSPIState *s = STC8G_SPI(reg->opaque);

    if (s->transfer_active) {
        s->regs[STC8G_SPI_REG_STAT] = FIELD_DP8(
            s->regs[STC8G_SPI_REG_STAT], SPSTAT, WCOL, 1);
        trace_stc8g_spi_collision(value);
        stc8g_spi_update_irq(s);
        return s->regs[STC8G_SPI_REG_DATA];
    }
    return value;
}

static void stc8g_spi_data_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gSPIState *s = STC8G_SPI(reg->opaque);

    if (stc8g_spi_enabled(s) && stc8g_spi_master(s) &&
        !s->transfer_active) {
        stc8g_spi_start(s);
    }
}

static void stc8g_spi_ctl_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gSPIState *s = STC8G_SPI(reg->opaque);

    if (!stc8g_spi_enabled(s)) {
        timer_del(s->timer);
        s->transfer_active = false;
        s->transfer_cycles = 0;
        s->clock_remainder = 0;
    } else if (!stc8g_spi_ss_ignored(s) && stc8g_spi_master(s) &&
               !s->ss_level) {
        s->regs[STC8G_SPI_REG_CTL] = FIELD_DP8(
            s->regs[STC8G_SPI_REG_CTL], SPCTL, MSTR, 0);
        s->regs[STC8G_SPI_REG_STAT] = FIELD_DP8(
            s->regs[STC8G_SPI_REG_STAT], SPSTAT, SPIF, 1);
        trace_stc8g_spi_mode_change(0);
    }
    stc8g_spi_update_irq(s);
}

static void stc8g_spi_stat_post_write(RegisterInfo *reg, uint64_t value)
{
    stc8g_spi_update_irq(STC8G_SPI(reg->opaque));
}

static const RegisterAccessInfo stc8g_spi_regs_info[] = {
    { .name = "SPSTAT", .addr = 0, .w1c = 0xc0, .rsvd = 0x3f,
      .post_write = stc8g_spi_stat_post_write },
    { .name = "SPCTL", .addr = 0, .reset = 0x04,
      .post_write = stc8g_spi_ctl_post_write },
    { .name = "SPDAT", .addr = 0,
      .pre_write = stc8g_spi_data_pre_write,
      .post_write = stc8g_spi_data_post_write },
};

static const MemoryRegionOps stc8g_spi_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_spi_set_ss(void *opaque, int n, int level)
{
    Stc8gSPIState *s = opaque;

    s->ss_level = !!level;
    if (stc8g_spi_enabled(s) && !stc8g_spi_ss_ignored(s) &&
        stc8g_spi_master(s) && !s->ss_level) {
        s->regs[STC8G_SPI_REG_CTL] = FIELD_DP8(
            s->regs[STC8G_SPI_REG_CTL], SPCTL, MSTR, 0);
        s->regs[STC8G_SPI_REG_STAT] = FIELD_DP8(
            s->regs[STC8G_SPI_REG_STAT], SPSTAT, SPIF, 1);
        trace_stc8g_spi_mode_change(0);
        stc8g_spi_update_irq(s);
    }
}

static void stc8g_spi_slave_receive(void *opaque, int n, int level)
{
    Stc8gSPIState *s = opaque;

    if (!stc8g_spi_enabled(s) || stc8g_spi_master(s) ||
        !stc8g_spi_slave_selected(s)) {
        return;
    }
    s->regs[STC8G_SPI_REG_DATA] = level;
    s->regs[STC8G_SPI_REG_STAT] = FIELD_DP8(
        s->regs[STC8G_SPI_REG_STAT], SPSTAT, SPIF, 1);
    trace_stc8g_spi_slave_receive(level & 0xff);
    stc8g_spi_update_irq(s);
}

static void stc8g_spi_reset(DeviceState *dev)
{
    Stc8gSPIState *s = STC8G_SPI(dev);
    unsigned index;

    timer_del(s->timer);
    s->transfer_active = false;
    s->tx_data = 0;
    s->transfer_cycles = 0;
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (index = 0; index < ARRAY_SIZE(stc8g_spi_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    stc8g_spi_update_irq(s);
}

static int stc8g_spi_post_load(void *opaque, int version_id)
{
    stc8g_spi_update_irq(STC8G_SPI(opaque));
    return 0;
}

static const VMStateDescription stc8g_spi_vmstate = {
    .name = "stc8g.spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gSPIState, STC8G_SPI_REGS),
        VMSTATE_UINT8(tx_data, Stc8gSPIState),
        VMSTATE_UINT64(transfer_cycles, Stc8gSPIState),
        VMSTATE_UINT64(clock_remainder, Stc8gSPIState),
        VMSTATE_INT64(last_ns, Stc8gSPIState),
        VMSTATE_BOOL(transfer_active, Stc8gSPIState),
        VMSTATE_BOOL(ss_level, Stc8gSPIState),
        VMSTATE_TIMER_PTR(timer, Stc8gSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_spi_properties[] = {
    DEFINE_PROP_UINT32("clock-frequency", Stc8gSPIState,
                       clock_frequency, 24000000),
};

static void stc8g_spi_realize(DeviceState *dev, Error **errp)
{
    Stc8gSPIState *s = STC8G_SPI(dev);

    if (!s->clock_frequency) {
        error_setg(errp, "stc8g-spi clock-frequency must be nonzero");
    }
}

static void stc8g_spi_init(Object *obj)
{
    Stc8gSPIState *s = STC8G_SPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_spi_regs_info) != STC8G_SPI_REGS);
    for (index = 0; index < ARRAY_SIZE(stc8g_spi_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_spi_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_spi_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stc8g_spi_expire, s);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc8g_spi_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    s->ssi = ssi_create_bus(DEVICE(obj), "ssi");
    s->ss_level = true;
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_spi_set_ss, "ss-in", 1);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_spi_slave_receive,
                            "slave-data", 1);
    sysbus_init_irq(sbd, &s->irq);
}

static void stc8g_spi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_spi_realize;
    device_class_set_legacy_reset(dc, stc8g_spi_reset);
    device_class_set_props(dc, stc8g_spi_properties);
    dc->vmsd = &stc8g_spi_vmstate;
}

static const TypeInfo stc8g_spi_type = {
    .name = TYPE_STC8G_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gSPIState),
    .instance_init = stc8g_spi_init,
    .class_init = stc8g_spi_class_init,
};

static void stc8g_spi_register_types(void)
{
    type_register_static(&stc8g_spi_type);
}

type_init(stc8g_spi_register_types)

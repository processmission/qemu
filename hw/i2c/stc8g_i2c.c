/*
 * STC8G1K08A I2C controller
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
#include "hw/i2c/i2c.h"
#include "hw/i2c/stc8g_i2c.h"
#include "hw/mcs51/clock.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "trace.h"

REG8(I2CCFG, 0)
    FIELD(I2CCFG, ENI2C, 7, 1)
    FIELD(I2CCFG, MSSL, 6, 1)
    FIELD(I2CCFG, MSSPEED, 0, 6)
REG8(I2CMSCR, 0)
    FIELD(I2CMSCR, EMSI, 7, 1)
    FIELD(I2CMSCR, MSCMD, 0, 4)
REG8(I2CMSST, 0)
    FIELD(I2CMSST, MSBUSY, 7, 1)
    FIELD(I2CMSST, MSIF, 6, 1)
    FIELD(I2CMSST, MSACKI, 1, 1)
    FIELD(I2CMSST, MSACKO, 0, 1)
REG8(I2CSLCR, 0)
    FIELD(I2CSLCR, ESTAI, 6, 1)
    FIELD(I2CSLCR, ERXI, 5, 1)
    FIELD(I2CSLCR, ETXI, 4, 1)
    FIELD(I2CSLCR, ESTOI, 3, 1)
    FIELD(I2CSLCR, SLRST, 0, 1)
REG8(I2CSLST, 0)
    FIELD(I2CSLST, SLBUSY, 7, 1)
    FIELD(I2CSLST, STAIF, 6, 1)
    FIELD(I2CSLST, RXIF, 5, 1)
    FIELD(I2CSLST, TXIF, 4, 1)
    FIELD(I2CSLST, STOIF, 3, 1)
    FIELD(I2CSLST, SLACKI, 1, 1)
    FIELD(I2CSLST, SLACKO, 0, 1)
REG8(I2CSLADR, 0)
REG8(I2CTXD, 0)
REG8(I2CRXD, 0)
REG8(I2CMSAUX, 0)
    FIELD(I2CMSAUX, WDTA, 0, 1)

enum Stc8gI2CRegister {
    STC8G_I2C_CFG,
    STC8G_I2C_MSCR,
    STC8G_I2C_MSST,
    STC8G_I2C_SLCR,
    STC8G_I2C_SLST,
    STC8G_I2C_SLADR,
    STC8G_I2C_TXD,
    STC8G_I2C_RXD,
    STC8G_I2C_MSAUX,
};

struct Stc8gI2CState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[STC8G_I2C_MMIO_REGS];
    RegisterInfo regs_info[STC8G_I2C_MMIO_REGS];
    uint8_t regs[STC8G_I2C_MMIO_REGS];
    QEMUTimer *timer;
    Clock *sysclk;
    I2CBus *i2c;
    qemu_irq irq;
    uint32_t clock_frequency;
    uint64_t remaining_cycles;
    uint64_t clock_remainder;
    int64_t last_ns;
    uint8_t pending_command;
    uint8_t slave_data;
    bool command_active;
    bool bus_active;
    bool expect_address;
    bool receive_direction;
    bool last_ack;
    bool sample_ack;
    bool slave_ack;
};

static bool stc8g_i2c_enabled(Stc8gI2CState *s)
{
    return FIELD_EX8(s->regs[STC8G_I2C_CFG], I2CCFG, ENI2C);
}

static bool stc8g_i2c_master(Stc8gI2CState *s)
{
    return FIELD_EX8(s->regs[STC8G_I2C_CFG], I2CCFG, MSSL);
}

static bool stc8g_i2c_slave_irq_pending(Stc8gI2CState *s)
{
    uint8_t flags = s->regs[STC8G_I2C_SLST] & 0x78;
    uint8_t enables = s->regs[STC8G_I2C_SLCR] & 0x78;

    return flags & enables;
}

static void stc8g_i2c_update_irq(Stc8gI2CState *s)
{
    bool pending;

    if (stc8g_i2c_master(s)) {
        pending = FIELD_EX8(s->regs[STC8G_I2C_MSCR], I2CMSCR, EMSI) &&
                  FIELD_EX8(s->regs[STC8G_I2C_MSST], I2CMSST, MSIF);
    } else {
        pending = stc8g_i2c_slave_irq_pending(s);
    }
    qemu_set_irq(s->irq, stc8g_i2c_enabled(s) && pending);
}

static void stc8g_i2c_set_master_flag(Stc8gI2CState *s, bool set)
{
    s->regs[STC8G_I2C_MSST] = FIELD_DP8(
        s->regs[STC8G_I2C_MSST], I2CMSST, MSIF, set);
}

static void stc8g_i2c_set_master_busy(Stc8gI2CState *s, bool busy)
{
    s->regs[STC8G_I2C_MSST] = FIELD_DP8(
        s->regs[STC8G_I2C_MSST], I2CMSST, MSBUSY, busy);
}

static void stc8g_i2c_set_master_ack(Stc8gI2CState *s, bool nack)
{
    s->regs[STC8G_I2C_MSST] = FIELD_DP8(
        s->regs[STC8G_I2C_MSST], I2CMSST, MSACKI, nack);
}

static uint64_t stc8g_i2c_command_cycles(Stc8gI2CState *s,
                                         unsigned clocks)
{
    unsigned speed = FIELD_EX8(s->regs[STC8G_I2C_CFG], I2CCFG, MSSPEED);

    return (uint64_t)clocks * 2 * (speed * 2 + 4);
}

static void stc8g_i2c_sync(Stc8gI2CState *s);
static void stc8g_i2c_schedule(Stc8gI2CState *s);

static void stc8g_i2c_clock_update(void *opaque, ClockEvent event)
{
    Stc8gI2CState *s = opaque;

    if (event == ClockPreUpdate) {
        stc8g_i2c_sync(s);
        return;
    }
    s->clock_frequency = clock_get_hz(s->sysclk);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc8g_i2c_schedule(s);
}

static unsigned stc8g_i2c_command_clocks(unsigned command)
{
    switch (command) {
    case 1:
    case 6:
        return 2;
    case 2:
    case 4:
        return 8;
    case 3:
    case 5:
        return 1;
    case 9:
        return 11;
    case 10:
        return 9;
    case 11:
    case 12:
        return 9;
    default:
        return 0;
    }
}

static void stc8g_i2c_start(Stc8gI2CState *s)
{
    stc8g_i2c_set_master_busy(s, true);
    s->expect_address = true;
}

static void stc8g_i2c_send(Stc8gI2CState *s)
{
    uint8_t data = s->regs[STC8G_I2C_TXD];
    int ret;

    if (s->expect_address) {
        ret = i2c_start_transfer(s->i2c, data >> 1, data & 1);
        s->bus_active = !ret;
        s->receive_direction = data & 1;
        s->expect_address = false;
    } else if (!s->bus_active || s->receive_direction) {
        ret = -1;
    } else {
        ret = i2c_send(s->i2c, data);
    }
    s->last_ack = ret;
}

static void stc8g_i2c_receive(Stc8gI2CState *s)
{
    s->regs[STC8G_I2C_RXD] = s->bus_active && s->receive_direction ?
                               i2c_recv(s->i2c) : 0xff;
}

static void stc8g_i2c_send_ack(Stc8gI2CState *s, bool nack)
{
    if (s->bus_active && s->receive_direction && nack) {
        i2c_nack(s->i2c);
    }
}

static void stc8g_i2c_stop(Stc8gI2CState *s)
{
    if (s->bus_active) {
        i2c_end_transfer(s->i2c);
    }
    s->bus_active = false;
    s->expect_address = false;
    s->receive_direction = false;
    stc8g_i2c_set_master_busy(s, false);
}

static void stc8g_i2c_execute_command(Stc8gI2CState *s, unsigned command)
{
    switch (command) {
    case 1:
        stc8g_i2c_start(s);
        break;
    case 2:
        stc8g_i2c_send(s);
        break;
    case 3:
        s->sample_ack = true;
        break;
    case 4:
        stc8g_i2c_receive(s);
        break;
    case 5:
        stc8g_i2c_send_ack(s, FIELD_EX8(s->regs[STC8G_I2C_MSST],
                                         I2CMSST, MSACKO));
        break;
    case 6:
        stc8g_i2c_stop(s);
        break;
    case 9:
        stc8g_i2c_start(s);
        stc8g_i2c_send(s);
        s->sample_ack = true;
        break;
    case 10:
        stc8g_i2c_send(s);
        s->sample_ack = true;
        break;
    case 11:
        stc8g_i2c_receive(s);
        stc8g_i2c_send_ack(s, false);
        break;
    case 12:
        stc8g_i2c_receive(s);
        stc8g_i2c_send_ack(s, true);
        break;
    default:
        break;
    }
}

static void stc8g_i2c_complete(void *opaque)
{
    Stc8gI2CState *s = opaque;

    s->command_active = false;
    if (s->sample_ack) {
        stc8g_i2c_set_master_ack(s, s->last_ack);
    }
    stc8g_i2c_set_master_flag(s, true);
    trace_stc8g_i2c_complete(s->pending_command,
                             s->regs[STC8G_I2C_MSST]);
    stc8g_i2c_update_irq(s);
}

static void stc8g_i2c_sync(Stc8gI2CState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->command_active && clock_is_enabled(s->sysclk)) {
        uint64_t cycles = mcs51_clock_elapsed_cycles(
            s->sysclk, now - s->last_ns, &s->clock_remainder);
        if (cycles >= s->remaining_cycles) {
            s->remaining_cycles = 0;
            s->last_ns = now;
            timer_del(s->timer);
            stc8g_i2c_complete(s);
            return;
        }
        s->remaining_cycles -= cycles;
    }
    s->last_ns = now;
}

static void stc8g_i2c_schedule(Stc8gI2CState *s)
{
    uint64_t delta;

    timer_del(s->timer);
    if (!s->command_active || !clock_is_enabled(s->sysclk)) {
        return;
    }
    delta = mcs51_clock_cycles_to_ns(s->sysclk, s->remaining_cycles,
                                     s->clock_remainder);
    timer_mod_ns(s->timer, s->last_ns + MAX(1ull, delta));
}

static void stc8g_i2c_expire(void *opaque)
{
    Stc8gI2CState *s = opaque;

    stc8g_i2c_sync(s);
    stc8g_i2c_schedule(s);
}

static void stc8g_i2c_issue_command(Stc8gI2CState *s, unsigned command)
{
    unsigned clocks = stc8g_i2c_command_clocks(command);

    if (!clock_is_enabled(s->sysclk) || !stc8g_i2c_enabled(s) ||
        !stc8g_i2c_master(s) || !clocks) {
        return;
    }
    s->pending_command = command;
    s->command_active = true;
    s->sample_ack = false;
    s->remaining_cycles = stc8g_i2c_command_cycles(s, clocks);
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    stc8g_i2c_set_master_flag(s, false);
    stc8g_i2c_execute_command(s, command);
    stc8g_i2c_schedule(s);
    trace_stc8g_i2c_command(command,
                            mcs51_clock_cycles_to_ns(s->sysclk,
                                                     s->remaining_cycles, 0));
    stc8g_i2c_update_irq(s);
}

static uint64_t stc8g_i2c_status_pre_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gI2CState *s = STC8G_I2C(reg->opaque);

    if (FIELD_EX8(value, I2CMSST, MSIF)) {
        value = FIELD_DP8(value, I2CMSST, MSIF,
                          FIELD_EX8(s->regs[STC8G_I2C_MSST],
                                    I2CMSST, MSIF));
    }
    return value;
}

static uint64_t stc8g_i2c_slave_status_pre_write(RegisterInfo *reg,
                                                   uint64_t value)
{
    Stc8gI2CState *s = STC8G_I2C(reg->opaque);
    unsigned bit;

    for (bit = 3; bit <= 6; bit++) {
        if (extract8(value, bit, 1)) {
            value = deposit32(value, bit, 1,
                              extract8(s->regs[STC8G_I2C_SLST], bit, 1));
        }
    }
    return value;
}

static void stc8g_i2c_cfg_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gI2CState *s = STC8G_I2C(reg->opaque);

    if (!stc8g_i2c_enabled(s) || !stc8g_i2c_master(s)) {
        timer_del(s->timer);
        s->command_active = false;
        s->remaining_cycles = 0;
        s->clock_remainder = 0;
        stc8g_i2c_stop(s);
    }
    stc8g_i2c_update_irq(s);
}

static void stc8g_i2c_mscr_post_write(RegisterInfo *reg, uint64_t value)
{
    stc8g_i2c_issue_command(STC8G_I2C(reg->opaque),
                            FIELD_EX8(value, I2CMSCR, MSCMD));
}

static void stc8g_i2c_status_post_write(RegisterInfo *reg, uint64_t value)
{
    stc8g_i2c_update_irq(STC8G_I2C(reg->opaque));
}

static void stc8g_i2c_slcr_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gI2CState *s = STC8G_I2C(reg->opaque);

    if (FIELD_EX8(value, I2CSLCR, SLRST)) {
        s->regs[STC8G_I2C_SLST] = 0;
        s->regs[STC8G_I2C_SLCR] = FIELD_DP8(
            s->regs[STC8G_I2C_SLCR], I2CSLCR, SLRST, 0);
    }
    stc8g_i2c_update_irq(s);
}

static void stc8g_i2c_txd_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gI2CState *s = STC8G_I2C(reg->opaque);

    if (FIELD_EX8(s->regs[STC8G_I2C_MSAUX], I2CMSAUX, WDTA)) {
        stc8g_i2c_issue_command(s, 10);
    }
}

static const RegisterAccessInfo stc8g_i2c_regs_info[] = {
    { .name = "I2CCFG", .addr = 0,
      .post_write = stc8g_i2c_cfg_post_write },
    { .name = "I2CMSCR", .addr = 0, .rsvd = 0x70,
      .post_write = stc8g_i2c_mscr_post_write },
    { .name = "I2CMSST", .addr = 0, .ro = 0x82, .rsvd = 0x3c,
      .pre_write = stc8g_i2c_status_pre_write,
      .post_write = stc8g_i2c_status_post_write },
    { .name = "I2CSLCR", .addr = 0, .rsvd = 0x86,
      .post_write = stc8g_i2c_slcr_post_write },
    { .name = "I2CSLST", .addr = 0, .ro = 0x82, .rsvd = 0x04,
      .pre_write = stc8g_i2c_slave_status_pre_write,
      .post_write = stc8g_i2c_status_post_write },
    { .name = "I2CSLADR", .addr = 0 },
    { .name = "I2CTXD", .addr = 0,
      .post_write = stc8g_i2c_txd_post_write },
    { .name = "I2CRXD", .addr = 0, .ro = 0xff },
    { .name = "I2CMSAUX", .addr = 0, .rsvd = 0xfe },
};

static const MemoryRegionOps stc8g_i2c_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_i2c_slave_event(void *opaque, int n, int level)
{
    Stc8gI2CState *s = opaque;

    if (!stc8g_i2c_enabled(s) || stc8g_i2c_master(s)) {
        return;
    }
    switch (level) {
    case STC8G_I2C_SLAVE_START:
        s->regs[STC8G_I2C_SLST] = FIELD_DP8(
            s->regs[STC8G_I2C_SLST], I2CSLST, SLBUSY, 1);
        s->regs[STC8G_I2C_SLST] = FIELD_DP8(
            s->regs[STC8G_I2C_SLST], I2CSLST, STAIF, 1);
        break;
    case STC8G_I2C_SLAVE_RECEIVE:
        s->regs[STC8G_I2C_RXD] = s->slave_data;
        s->regs[STC8G_I2C_SLST] = FIELD_DP8(
            s->regs[STC8G_I2C_SLST], I2CSLST, RXIF, 1);
        break;
    case STC8G_I2C_SLAVE_TRANSMIT:
        s->regs[STC8G_I2C_SLST] = FIELD_DP8(
            s->regs[STC8G_I2C_SLST], I2CSLST, SLACKI, s->slave_ack);
        s->regs[STC8G_I2C_SLST] = FIELD_DP8(
            s->regs[STC8G_I2C_SLST], I2CSLST, TXIF, 1);
        break;
    case STC8G_I2C_SLAVE_STOP:
        s->regs[STC8G_I2C_SLST] = FIELD_DP8(
            s->regs[STC8G_I2C_SLST], I2CSLST, SLBUSY, 0);
        s->regs[STC8G_I2C_SLST] = FIELD_DP8(
            s->regs[STC8G_I2C_SLST], I2CSLST, STOIF, 1);
        break;
    default:
        return;
    }
    trace_stc8g_i2c_slave_event(level, s->regs[STC8G_I2C_SLST]);
    stc8g_i2c_update_irq(s);
}

static void stc8g_i2c_set_slave_data(void *opaque, int n, int level)
{
    STC8G_I2C(opaque)->slave_data = level;
}

static void stc8g_i2c_set_slave_ack(void *opaque, int n, int level)
{
    STC8G_I2C(opaque)->slave_ack = !!level;
}

static void stc8g_i2c_reset(DeviceState *dev)
{
    Stc8gI2CState *s = STC8G_I2C(dev);
    unsigned index;

    timer_del(s->timer);
    if (s->bus_active) {
        i2c_end_transfer(s->i2c);
    }
    s->pending_command = 0;
    s->command_active = false;
    s->remaining_cycles = 0;
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->bus_active = false;
    s->expect_address = false;
    s->receive_direction = false;
    s->last_ack = false;
    s->sample_ack = false;
    for (index = 0; index < ARRAY_SIZE(stc8g_i2c_regs_info); index++) {
        register_reset(&s->regs_info[index]);
    }
    stc8g_i2c_update_irq(s);
}

static int stc8g_i2c_post_load(void *opaque, int version_id)
{
    stc8g_i2c_update_irq(STC8G_I2C(opaque));
    return 0;
}

static const VMStateDescription stc8g_i2c_vmstate = {
    .name = "stc8g.i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stc8g_i2c_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gI2CState, STC8G_I2C_MMIO_REGS),
        VMSTATE_UINT64(remaining_cycles, Stc8gI2CState),
        VMSTATE_UINT64(clock_remainder, Stc8gI2CState),
        VMSTATE_INT64(last_ns, Stc8gI2CState),
        VMSTATE_UINT8(pending_command, Stc8gI2CState),
        VMSTATE_UINT8(slave_data, Stc8gI2CState),
        VMSTATE_BOOL(command_active, Stc8gI2CState),
        VMSTATE_BOOL(bus_active, Stc8gI2CState),
        VMSTATE_BOOL(expect_address, Stc8gI2CState),
        VMSTATE_BOOL(receive_direction, Stc8gI2CState),
        VMSTATE_BOOL(last_ack, Stc8gI2CState),
        VMSTATE_BOOL(sample_ack, Stc8gI2CState),
        VMSTATE_BOOL(slave_ack, Stc8gI2CState),
        VMSTATE_TIMER_PTR(timer, Stc8gI2CState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property stc8g_i2c_properties[] = {
    DEFINE_PROP_UINT32("clock-frequency", Stc8gI2CState,
                       clock_frequency, 24000000),
};

static void stc8g_i2c_realize(DeviceState *dev, Error **errp)
{
    Stc8gI2CState *s = STC8G_I2C(dev);

    if (!s->clock_frequency) {
        error_setg(errp, "stc8g-i2c clock-frequency must be nonzero");
    }
}

static void stc8g_i2c_init(Object *obj)
{
    Stc8gI2CState *s = STC8G_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_i2c_regs_info) !=
                      STC8G_I2C_MMIO_REGS);
    for (index = 0; index < ARRAY_SIZE(stc8g_i2c_regs_info); index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_i2c_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_i2c_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stc8g_i2c_expire, s);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc8g_i2c_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    s->i2c = i2c_init_bus(DEVICE(obj), "i2c");
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_i2c_slave_event,
                            "slave-event", 1);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_i2c_set_slave_data,
                            "slave-data", 1);
    qdev_init_gpio_in_named(DEVICE(obj), stc8g_i2c_set_slave_ack,
                            "slave-ack", 1);
    sysbus_init_irq(sbd, &s->irq);
}

static void stc8g_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_i2c_realize;
    device_class_set_legacy_reset(dc, stc8g_i2c_reset);
    device_class_set_props(dc, stc8g_i2c_properties);
    dc->vmsd = &stc8g_i2c_vmstate;
}

static const TypeInfo stc8g_i2c_type = {
    .name = TYPE_STC8G_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gI2CState),
    .instance_init = stc8g_i2c_init,
    .class_init = stc8g_i2c_class_init,
};

static void stc8g_i2c_register_types(void)
{
    type_register_static(&stc8g_i2c_type);
}

type_init(stc8g_i2c_register_types)

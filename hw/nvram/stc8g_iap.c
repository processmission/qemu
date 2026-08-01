/*
 * STC8G1K08A IAP and EEPROM controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/mcs51/clock.h"
#include "hw/nvram/stc8g_iap.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "trace.h"

#define STC8G_IAP_NANOSECONDS_PER_MICROSECOND 1000

REG8(IAP_DATA, 0)
REG8(IAP_ADDRH, 0)
REG8(IAP_ADDRL, 0)
REG8(IAP_CMD, 0)
    FIELD(IAP_CMD, CMD, 0, 2)
REG8(IAP_TRIG, 0)
REG8(IAP_CONTR, 0)
    FIELD(IAP_CONTR, IAPEN, 7, 1)
    FIELD(IAP_CONTR, SWBS, 6, 1)
    FIELD(IAP_CONTR, SWRST, 5, 1)
    FIELD(IAP_CONTR, CMD_FAIL, 4, 1)
REG8(IAP_TPS, 0)
    FIELD(IAP_TPS, IAPTPS, 0, 6)

enum Stc8gIapCommand {
    STC8G_IAP_CMD_IDLE,
    STC8G_IAP_CMD_READ,
    STC8G_IAP_CMD_PROGRAM,
    STC8G_IAP_CMD_ERASE,
};

enum Stc8gIapTriggerStage {
    STC8G_IAP_TRIGGER_IDLE,
    STC8G_IAP_TRIGGER_FIRST,
};

struct Stc8gIapState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[STC8G_IAP_MMIO_REGS];
    RegisterInfo regs_info[STC8G_IAP_MMIO_REGS];
    uint8_t regs[STC8G_IAP_MMIO_REGS];
    MemoryRegion eeprom;
    uint8_t *storage;
    Clock *sysclk;
    QEMUTimer *timer;
    uint32_t clock_frequency;
    uint16_t pending_addr;
    uint8_t pending_data;
    uint8_t pending_cmd;
    uint8_t trigger_stage;
    uint64_t remaining_read_cycles;
    uint64_t clock_remainder;
    int64_t last_ns;
    bool busy;
    bool sw_reset_requested;
    bool resetting;
};

static void stc8g_iap_set_failure(Stc8gIapState *s)
{
    s->regs[STC8G_IAP_MMIO_CONTR] = FIELD_DP8(
        s->regs[STC8G_IAP_MMIO_CONTR], IAP_CONTR, CMD_FAIL, 1);
}

static uint16_t stc8g_iap_address(Stc8gIapState *s)
{
    return s->regs[STC8G_IAP_MMIO_ADDRH] << 8 |
           s->regs[STC8G_IAP_MMIO_ADDRL];
}

static uint64_t stc8g_iap_operation_ns(Stc8gIapState *s, unsigned cmd)
{
    switch (cmd) {
    case STC8G_IAP_CMD_READ:
        return mcs51_clock_cycles_to_ns(s->sysclk, 4, 0);
    case STC8G_IAP_CMD_PROGRAM:
        return 31 * STC8G_IAP_NANOSECONDS_PER_MICROSECOND;
    case STC8G_IAP_CMD_ERASE:
        return 4571 * STC8G_IAP_NANOSECONDS_PER_MICROSECOND;
    default:
        g_assert_not_reached();
    }
}

static void stc8g_iap_sync(Stc8gIapState *s);
static void stc8g_iap_schedule(Stc8gIapState *s);

static bool stc8g_iap_tps_valid(Stc8gIapState *s)
{
    unsigned expected;

    if (!s->clock_frequency) {
        return false;
    }
    expected = (s->clock_frequency + 500000) / 1000000;
    return expected <= 0x3f &&
           FIELD_EX8(s->regs[STC8G_IAP_MMIO_TPS], IAP_TPS, IAPTPS) ==
           expected;
}

static void stc8g_iap_start(Stc8gIapState *s)
{
    uint64_t duration;
    unsigned cmd = FIELD_EX8(s->regs[STC8G_IAP_MMIO_CMD], IAP_CMD, CMD);

    if (s->busy ||
        !FIELD_EX8(s->regs[STC8G_IAP_MMIO_CONTR], IAP_CONTR, IAPEN) ||
        cmd == STC8G_IAP_CMD_IDLE ||
        stc8g_iap_address(s) >= STC8G_IAP_EEPROM_SIZE ||
        !stc8g_iap_tps_valid(s)) {
        stc8g_iap_set_failure(s);
        trace_stc8g_iap_failure(cmd, stc8g_iap_address(s));
        return;
    }

    s->pending_addr = stc8g_iap_address(s);
    s->pending_data = s->regs[STC8G_IAP_MMIO_DATA];
    s->pending_cmd = cmd;
    s->busy = true;
    s->remaining_read_cycles = cmd == STC8G_IAP_CMD_READ ? 4 : 0;
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    duration = cmd == STC8G_IAP_CMD_READ ?
        mcs51_clock_cycles_to_ns(s->sysclk, s->remaining_read_cycles, 0) :
        stc8g_iap_operation_ns(s, cmd);
    if (cmd == STC8G_IAP_CMD_READ) {
        stc8g_iap_schedule(s);
    } else {
        timer_mod_ns(s->timer, s->last_ns + duration);
    }
    trace_stc8g_iap_start(cmd, s->pending_addr, duration);
}

static void stc8g_iap_complete(void *opaque)
{
    Stc8gIapState *s = opaque;
    uint16_t sector;

    switch (s->pending_cmd) {
    case STC8G_IAP_CMD_READ:
        s->regs[STC8G_IAP_MMIO_DATA] = s->storage[s->pending_addr];
        break;
    case STC8G_IAP_CMD_PROGRAM:
        s->storage[s->pending_addr] &= s->pending_data;
        memory_region_flush_rom_device(&s->eeprom, s->pending_addr, 1);
        break;
    case STC8G_IAP_CMD_ERASE:
        sector = s->pending_addr & ~(STC8G_IAP_SECTOR_SIZE - 1);
        memset(s->storage + sector, 0xff, STC8G_IAP_SECTOR_SIZE);
        memory_region_flush_rom_device(&s->eeprom, sector,
                                       STC8G_IAP_SECTOR_SIZE);
        break;
    default:
        g_assert_not_reached();
    }

    s->busy = false;
    trace_stc8g_iap_complete(s->pending_cmd, s->pending_addr,
                             s->regs[STC8G_IAP_MMIO_DATA]);
}

static void stc8g_iap_sync(Stc8gIapState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->busy && s->pending_cmd == STC8G_IAP_CMD_READ &&
        clock_is_enabled(s->sysclk)) {
        uint64_t cycles = mcs51_clock_elapsed_cycles(
            s->sysclk, now - s->last_ns, &s->clock_remainder);
        if (cycles >= s->remaining_read_cycles) {
            s->remaining_read_cycles = 0;
            s->last_ns = now;
            timer_del(s->timer);
            stc8g_iap_complete(s);
            return;
        }
        s->remaining_read_cycles -= cycles;
    }
    s->last_ns = now;
}

static void stc8g_iap_schedule(Stc8gIapState *s)
{
    uint64_t delta;

    timer_del(s->timer);
    if (!s->busy || s->pending_cmd != STC8G_IAP_CMD_READ ||
        !clock_is_enabled(s->sysclk)) {
        return;
    }
    delta = mcs51_clock_cycles_to_ns(s->sysclk, s->remaining_read_cycles,
                                     s->clock_remainder);
    timer_mod_ns(s->timer, s->last_ns + MAX(1ull, delta));
}

static void stc8g_iap_expire(void *opaque)
{
    Stc8gIapState *s = opaque;

    if (s->pending_cmd == STC8G_IAP_CMD_READ) {
        stc8g_iap_sync(s);
        stc8g_iap_schedule(s);
    } else {
        stc8g_iap_complete(s);
    }
}

static uint64_t stc8g_iap_eeprom_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    g_assert_not_reached();
}

static void stc8g_iap_eeprom_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "stc8g-iap: direct EEPROM write at 0x%" HWADDR_PRIx
                  " is not supported\n", offset);
}

static const MemoryRegionOps stc8g_iap_eeprom_ops = {
    .read = stc8g_iap_eeprom_read,
    .write = stc8g_iap_eeprom_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t stc8g_iap_trig_pre_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gIapState *s = STC8G_IAP(reg->opaque);

    if (!s->resetting && s->busy) {
        stc8g_iap_set_failure(s);
        trace_stc8g_iap_failure(
            FIELD_EX8(s->regs[STC8G_IAP_MMIO_CMD], IAP_CMD, CMD),
            stc8g_iap_address(s));
    }
    return value;
}

static void stc8g_iap_trig_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gIapState *s = STC8G_IAP(reg->opaque);

    if (s->resetting) {
        return;
    }
    if (value == 0x5a) {
        s->trigger_stage = STC8G_IAP_TRIGGER_FIRST;
    } else if (value == 0xa5 &&
               s->trigger_stage == STC8G_IAP_TRIGGER_FIRST) {
        s->trigger_stage = STC8G_IAP_TRIGGER_IDLE;
        stc8g_iap_start(s);
    } else {
        s->trigger_stage = STC8G_IAP_TRIGGER_IDLE;
    }
}

static uint64_t stc8g_iap_contr_pre_write(RegisterInfo *reg,
                                           uint64_t value)
{
    Stc8gIapState *s = STC8G_IAP(reg->opaque);
    uint8_t old = s->regs[STC8G_IAP_MMIO_CONTR];
    uint8_t next = value;

    s->sw_reset_requested = !s->resetting &&
        FIELD_EX8(next, IAP_CONTR, SWRST);
    next = FIELD_DP8(next, IAP_CONTR, SWRST, 0);
    next = FIELD_DP8(next, IAP_CONTR, CMD_FAIL,
                     FIELD_EX8(old, IAP_CONTR, CMD_FAIL) &&
                     FIELD_EX8(value, IAP_CONTR, CMD_FAIL));
    return next;
}

static void stc8g_iap_contr_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gIapState *s = STC8G_IAP(reg->opaque);

    if (s->sw_reset_requested) {
        trace_stc8g_iap_software_reset(
            FIELD_EX8(s->regs[STC8G_IAP_MMIO_CONTR], IAP_CONTR, SWBS));
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
    s->sw_reset_requested = false;
}

static void stc8g_iap_config_post_write(RegisterInfo *reg, uint64_t value)
{
    Stc8gIapState *s = STC8G_IAP(reg->opaque);

    if (!s->resetting) {
        s->trigger_stage = STC8G_IAP_TRIGGER_IDLE;
    }
}

static const RegisterAccessInfo stc8g_iap_regs_info[] = {
    { .name = "IAP_DATA", .addr = 0, .reset = 0xff,
      .post_write = stc8g_iap_config_post_write },
    { .name = "IAP_ADDRH", .addr = 0,
      .post_write = stc8g_iap_config_post_write },
    { .name = "IAP_ADDRL", .addr = 0,
      .post_write = stc8g_iap_config_post_write },
    { .name = "IAP_CMD", .addr = 0, .rsvd = 0xfc,
      .post_write = stc8g_iap_config_post_write },
    { .name = "IAP_TRIG", .addr = 0,
      .pre_write = stc8g_iap_trig_pre_write,
      .post_write = stc8g_iap_trig_post_write },
    { .name = "IAP_CONTR", .addr = 0, .rsvd = 0x0f,
      .pre_write = stc8g_iap_contr_pre_write,
      .post_write = stc8g_iap_contr_post_write },
    { .name = "IAP_TPS", .addr = 0, .rsvd = 0xc0,
      .post_write = stc8g_iap_config_post_write },
};

static const MemoryRegionOps stc8g_iap_regs_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void stc8g_iap_clock_update(void *opaque, ClockEvent event)
{
    Stc8gIapState *s = opaque;

    if (event == ClockPreUpdate) {
        stc8g_iap_sync(s);
        return;
    }
    s->clock_frequency = clock_get_hz(s->sysclk);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->busy || s->pending_cmd == STC8G_IAP_CMD_READ) {
        stc8g_iap_schedule(s);
    }
}

static void stc8g_iap_reset(DeviceState *dev)
{
    Stc8gIapState *s = STC8G_IAP(dev);
    unsigned index;

    s->resetting = true;
    for (index = 0; index < STC8G_IAP_MMIO_REGS; index++) {
        register_reset(&s->regs_info[index]);
    }
    s->resetting = false;
    s->busy = false;
    s->remaining_read_cycles = 0;
    s->clock_remainder = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->trigger_stage = STC8G_IAP_TRIGGER_IDLE;
    s->sw_reset_requested = false;
    timer_del(s->timer);
}

static const VMStateDescription stc8g_iap_vmstate = {
    .name = "stc8g.iap",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Stc8gIapState, STC8G_IAP_MMIO_REGS),
        VMSTATE_UINT16(pending_addr, Stc8gIapState),
        VMSTATE_UINT8(pending_data, Stc8gIapState),
        VMSTATE_UINT8(pending_cmd, Stc8gIapState),
        VMSTATE_UINT8(trigger_stage, Stc8gIapState),
        VMSTATE_UINT64(remaining_read_cycles, Stc8gIapState),
        VMSTATE_UINT64(clock_remainder, Stc8gIapState),
        VMSTATE_INT64(last_ns, Stc8gIapState),
        VMSTATE_BOOL(busy, Stc8gIapState),
        VMSTATE_TIMER_PTR(timer, Stc8gIapState),
        VMSTATE_END_OF_LIST()
    },
};

static void stc8g_iap_realize(DeviceState *dev, Error **errp)
{
    Stc8gIapState *s = STC8G_IAP(dev);

    if (!memory_region_init_rom_device(&s->eeprom, OBJECT(dev),
                                       &stc8g_iap_eeprom_ops, s,
                                       "stc8g.eeprom",
                                       STC8G_IAP_EEPROM_SIZE, errp)) {
        return;
    }
    s->storage = memory_region_get_ram_ptr(&s->eeprom);
    memset(s->storage, 0xff, STC8G_IAP_EEPROM_SIZE);
    s->clock_frequency = clock_get_hz(s->sysclk);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->eeprom);
}

static void stc8g_iap_init(Object *obj)
{
    Stc8gIapState *s = STC8G_IAP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    unsigned index;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(stc8g_iap_regs_info) !=
                      STC8G_IAP_MMIO_REGS);
    for (index = 0; index < STC8G_IAP_MMIO_REGS; index++) {
        s->reg_array[index] = register_init_block8(
            DEVICE(obj), &stc8g_iap_regs_info[index], 1,
            &s->regs_info[index], &s->regs[index], &stc8g_iap_regs_ops,
            false, 1);
        sysbus_init_mmio(sbd, &s->reg_array[index]->mem);
    }
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk",
                                   stc8g_iap_clock_update, s,
                                   ClockPreUpdate | ClockUpdate);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stc8g_iap_expire, s);
}

static void stc8g_iap_finalize(Object *obj)
{
    Stc8gIapState *s = STC8G_IAP(obj);

    timer_free(s->timer);
}

static void stc8g_iap_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_iap_realize;
    device_class_set_legacy_reset(dc, stc8g_iap_reset);
    dc->vmsd = &stc8g_iap_vmstate;
    dc->desc = "STC8G IAP and EEPROM controller";
}

static const TypeInfo stc8g_iap_type = {
    .name = TYPE_STC8G_IAP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gIapState),
    .instance_init = stc8g_iap_init,
    .instance_finalize = stc8g_iap_finalize,
    .class_init = stc8g_iap_class_init,
};

static void stc8g_iap_register_types(void)
{
    type_register_static(&stc8g_iap_type);
}

type_init(stc8g_iap_register_types)

/*
 * K230 low-speed hardware timers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_timer.h"

#define K230_TIMER_FREQ          27000000ULL
#define K230_TIMER_CH_STRIDE     0x14
#define K230_TIMER_LOAD_COUNT    0x00
#define K230_TIMER_CURRENT_VALUE 0x04
#define K230_TIMER_CONTROL       0x08
#define K230_TIMER_EOI           0x0c
#define K230_TIMER_INTR_STAT     0x10

static uint64_t k230_timer_read_bytes(uint8_t *regs, hwaddr addr,
                                      unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_timer_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                   unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_timer_reg_read32(K230TimerState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_timer_reg_write32(K230TimerState *s, hwaddr addr,
                                   uint32_t val)
{
    stl_le_p(s->regs + addr, val);
}

static bool k230_timer_decode(hwaddr addr, unsigned int size,
                              unsigned int *channel, hwaddr *reg)
{
    if (size != 4) {
        return false;
    }

    *channel = addr / K230_TIMER_CH_STRIDE;
    *reg = addr % K230_TIMER_CH_STRIDE;
    return *channel < K230_TIMER_COUNT;
}

static uint32_t k230_timer_current(K230TimerState *s, unsigned int channel)
{
    hwaddr base = channel * K230_TIMER_CH_STRIDE;
    uint32_t load = k230_timer_reg_read32(s, base + K230_TIMER_LOAD_COUNT);
    uint32_t control = k230_timer_reg_read32(s, base + K230_TIMER_CONTROL);
    int64_t now;
    uint64_t ticks;

    if (!(control & BIT(0))) {
        return k230_timer_reg_read32(s, base + K230_TIMER_CURRENT_VALUE);
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ticks = muldiv64(now - s->start_ns[channel], K230_TIMER_FREQ,
                     NANOSECONDS_PER_SECOND);

    return ticks >= load ? 0 : load - ticks;
}

static void k230_timer_restart(K230TimerState *s, unsigned int channel)
{
    hwaddr base = channel * K230_TIMER_CH_STRIDE;

    s->start_ns[channel] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    k230_timer_reg_write32(s, base + K230_TIMER_CURRENT_VALUE,
                           k230_timer_reg_read32(s,
                               base + K230_TIMER_LOAD_COUNT));
}

static uint64_t k230_timer_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230TimerState *s = K230_TIMER(opaque);
    unsigned int channel;
    hwaddr reg;

    if (k230_timer_decode(addr, size, &channel, &reg)) {
        hwaddr base = channel * K230_TIMER_CH_STRIDE;

        switch (reg) {
        case K230_TIMER_CURRENT_VALUE:
            return k230_timer_current(s, channel);
        case K230_TIMER_INTR_STAT:
            return k230_timer_current(s, channel) == 0 ? 1 : 0;
        case K230_TIMER_EOI:
            k230_timer_reg_write32(s, base + K230_TIMER_INTR_STAT, 0);
            return 1;
        default:
            break;
        }
    }

    return k230_timer_read_bytes(s->regs, addr, size);
}

static void k230_timer_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned int size)
{
    K230TimerState *s = K230_TIMER(opaque);
    unsigned int channel;
    hwaddr reg;

    k230_timer_write_bytes(s->regs, addr, val, size);

    if (k230_timer_decode(addr, size, &channel, &reg) &&
        (reg == K230_TIMER_LOAD_COUNT || reg == K230_TIMER_CONTROL)) {
        k230_timer_restart(s, channel);
    }
}

static const MemoryRegionOps k230_timer_ops = {
    .read = k230_timer_read,
    .write = k230_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_timer_reset(DeviceState *dev)
{
    K230TimerState *s = K230_TIMER(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (int i = 0; i < K230_TIMER_COUNT; i++) {
        hwaddr base = i * K230_TIMER_CH_STRIDE;

        k230_timer_reg_write32(s, base + K230_TIMER_LOAD_COUNT, UINT32_MAX);
        k230_timer_reg_write32(s, base + K230_TIMER_CURRENT_VALUE,
                               UINT32_MAX);
        s->start_ns[i] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
}

static const VMStateDescription vmstate_k230_timer = {
    .name = TYPE_K230_TIMER,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230TimerState, K230_TIMER_SIZE),
        VMSTATE_INT64_ARRAY(start_ns, K230TimerState, K230_TIMER_COUNT),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_timer_realize(DeviceState *dev, Error **errp)
{
    K230TimerState *s = K230_TIMER(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_timer_ops, s,
                          TYPE_K230_TIMER, K230_TIMER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_timer_realize;
    device_class_set_legacy_reset(dc, k230_timer_reset);
    dc->vmsd = &vmstate_k230_timer;
    dc->desc = "K230 low-speed hardware timers";
}

static const TypeInfo k230_timer_type_info = {
    .name = TYPE_K230_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230TimerState),
    .class_init = k230_timer_class_init,
};

static void k230_register_timer_types(void)
{
    type_register_static(&k230_timer_type_info);
}

type_init(k230_register_timer_types)

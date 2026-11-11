/*
 * RK3588 SARADC (v2 controller) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register-level model of the second generation Rockchip SAR ADC, as
 * described by drivers/iio/adc/rockchip_saradc.c and used by the Linux
 * "rockchip,saradc" binding on RK3588.  The block is not register
 * compatible with the rk3566 one: conversions are started through
 * CONV_CON, the interrupt has its own enable and status registers, and
 * each channel keeps its result in a separate data register.
 *
 *   CONV_CON     0x000  RW  write-masked  start/single/channel select
 *   T_PD_SOC     0x004  RW  power-up delay in ADC clocks
 *   T_DAS_SOC    0x00c  RW  sampling delay in ADC clocks
 *   END_INT_EN   0x104  RW  write-masked  end-of-conversion IRQ enable
 *   ST_CON       0x108  RW  write-masked  start condition
 *   STATUS       0x10c  RO  bit0 set while a conversion is in flight
 *   END_INT_ST   0x110  RW1C end-of-conversion flag
 *   DATA_BASE    0x120  RO  one 12-bit result per channel, +4 each
 *
 * The write-masked registers follow the Rockchip convention: the upper
 * half of the value carries the write-enable bits for the lower half, so
 * a bit only changes when it is set in both halves.  Linux programs them
 * as "val | val << 16".
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/adc/rk3588_saradc.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

enum {
    SARADC_CONV_CON   = 0x000,
    SARADC_T_PD_SOC   = 0x004,
    SARADC_T_DAS_SOC  = 0x00c,
    SARADC_END_INT_EN = 0x104,
    SARADC_ST_CON     = 0x108,
    SARADC_STATUS     = 0x10c,
    SARADC_END_INT_ST = 0x110,
    SARADC_DATA_BASE  = 0x120,
};

#define SARADC2_EN_END_INT      BIT(0)
#define SARADC2_START           BIT(4)
#define SARADC2_SINGLE_MODE     BIT(5)
#define SARADC2_CONV_CHANNELS   0xf
#define SARADC2_CONV_CON_MASK   (SARADC2_CONV_CHANNELS | SARADC2_START | \
                                 SARADC2_SINGLE_MODE)
#define SARADC2_STATUS_BUSY     BIT(0)

static void rk3588_saradc_update_irq(RK3588SARADCState *s)
{
    bool pending = (s->regs[SARADC_END_INT_ST >> 2] & SARADC2_EN_END_INT) &&
                   (s->regs[SARADC_END_INT_EN >> 2] & SARADC2_EN_END_INT);

    qemu_set_irq(s->irq, pending);
}

static void rk3588_saradc_complete(void *opaque)
{
    RK3588SARADCState *s = opaque;
    unsigned int ch = s->regs[SARADC_CONV_CON >> 2] & SARADC2_CONV_CHANNELS;
    uint64_t code;

    assert(ch < RK3588_SARADC_NUM_CHANNELS);

    code = (uint64_t)s->inputs_uv[ch] * RK3588_SARADC_DATA_MASK;
    s->regs[(SARADC_DATA_BASE >> 2) + ch] =
        MIN(code / s->reference_uv, RK3588_SARADC_DATA_MASK);

    /* START is self-clearing once the sample has been taken. */
    s->regs[SARADC_CONV_CON >> 2] &= ~SARADC2_START;
    s->regs[SARADC_END_INT_ST >> 2] |= SARADC2_EN_END_INT;
    rk3588_saradc_update_irq(s);
}

static void rk3588_saradc_start(RK3588SARADCState *s)
{
    uint32_t conv_con = s->regs[SARADC_CONV_CON >> 2];
    unsigned int ch = conv_con & SARADC2_CONV_CHANNELS;
    uint64_t clocks, delay_ns;

    if (!(conv_con & SARADC2_START)) {
        return;
    }

    if (ch >= RK3588_SARADC_NUM_CHANNELS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3588-saradc: no channel %u, conversion ignored\n",
                      ch);
        s->regs[SARADC_CONV_CON >> 2] &= ~SARADC2_START;
        return;
    }

    /*
     * The power-up and sampling delays are counted in ADC clocks and the
     * SAR core needs one more clock per bit of resolution, so the sample
     * is ready after T_PD_SOC + T_DAS_SOC + 12 clocks.  With the values
     * Linux programs (32 and 12) and a 1 MHz clock that is 56 us.
     */
    clocks = (uint64_t)s->regs[SARADC_T_PD_SOC >> 2] +
             s->regs[SARADC_T_DAS_SOC >> 2] + RK3588_SARADC_RESOLUTION;
    delay_ns = muldiv64(clocks, NANOSECONDS_PER_SECOND, s->clock_hz);

    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static uint64_t rk3588_saradc_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3588SARADCState *s = opaque;

    if (offset >= SARADC_DATA_BASE &&
        offset < SARADC_DATA_BASE + RK3588_SARADC_NUM_CHANNELS * 4) {
        return s->regs[offset >> 2];
    }

    switch (offset) {
    case SARADC_CONV_CON:
    case SARADC_T_PD_SOC:
    case SARADC_T_DAS_SOC:
    case SARADC_END_INT_EN:
    case SARADC_ST_CON:
        return s->regs[offset >> 2];
    case SARADC_STATUS:
        return timer_pending(s->timer) ? SARADC2_STATUS_BUSY : 0;
    case SARADC_END_INT_ST:
        return s->regs[offset >> 2] & SARADC2_EN_END_INT;
    default:
        return 0;
    }
}

static void rk3588_saradc_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    RK3588SARADCState *s = opaque;
    uint32_t val = value;
    uint32_t mask;

    switch (offset) {
    case SARADC_CONV_CON:
        mask = (val >> 16) & SARADC2_CONV_CON_MASK;
        s->regs[offset >> 2] = (s->regs[offset >> 2] & ~mask) |
                               (val & mask);
        rk3588_saradc_start(s);
        break;
    case SARADC_END_INT_EN:
        mask = (val >> 16) & SARADC2_EN_END_INT;
        s->regs[offset >> 2] = (s->regs[offset >> 2] & ~mask) |
                               (val & mask);
        rk3588_saradc_update_irq(s);
        break;
    case SARADC_ST_CON:
        mask = val >> 16;
        s->regs[offset >> 2] = (s->regs[offset >> 2] & ~mask) |
                               (val & mask);
        break;
    case SARADC_T_PD_SOC:
    case SARADC_T_DAS_SOC:
        s->regs[offset >> 2] = val;
        break;
    case SARADC_END_INT_ST:
        /* Write one to clear the end-of-conversion flag. */
        s->regs[offset >> 2] &= ~(val & SARADC2_EN_END_INT);
        rk3588_saradc_update_irq(s);
        break;
    case SARADC_STATUS:
        /* Read-only. */
        break;
    default:
        if (offset >= SARADC_DATA_BASE &&
            offset < SARADC_DATA_BASE + RK3588_SARADC_NUM_CHANNELS * 4) {
            /* Results are written by the conversion alone. */
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3588-saradc: write to reserved offset 0x%" HWADDR_PRIx
                      "\n", offset);
        break;
    }
}

static const MemoryRegionOps rk3588_saradc_ops = {
    .read = rk3588_saradc_read,
    .write = rk3588_saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rk3588_saradc_reset(DeviceState *dev)
{
    RK3588SARADCState *s = RK3588_SARADC(dev);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void rk3588_saradc_realize(DeviceState *dev, Error **errp)
{
    RK3588SARADCState *s = RK3588_SARADC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!s->reference_uv) {
        error_setg(errp, "rk3588-saradc reference voltage must be non-zero");
        return;
    }
    if (!s->clock_hz) {
        error_setg(errp, "rk3588-saradc clock frequency must be non-zero");
        return;
    }

    sysbus_init_irq(sbd, &s->irq);
    memory_region_init_io(&s->iomem, OBJECT(dev), &rk3588_saradc_ops, s,
                          "rk3588-saradc", RK3588_SARADC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rk3588_saradc_init(Object *obj)
{
    RK3588SARADCState *s = RK3588_SARADC(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rk3588_saradc_complete, s);
}

static void rk3588_saradc_finalize(Object *obj)
{
    RK3588SARADCState *s = RK3588_SARADC(obj);

    timer_free(s->timer);
}

static int rk3588_saradc_post_load(void *opaque, int version_id)
{
    RK3588SARADCState *s = opaque;

    rk3588_saradc_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_rk3588_saradc = {
    .name = TYPE_RK3588_SARADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rk3588_saradc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3588SARADCState,
                             RK3588_SARADC_MMIO_SIZE / 4),
        VMSTATE_TIMER_PTR(timer, RK3588SARADCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rk3588_saradc_properties[] = {
    DEFINE_PROP_UINT32("reference-uv", RK3588SARADCState, reference_uv,
                       1800000),
    DEFINE_PROP_UINT32("clock-hz", RK3588SARADCState, clock_hz, 1000000),
    DEFINE_PROP_UINT32("vin0-uv", RK3588SARADCState, inputs_uv[0], 0),
    DEFINE_PROP_UINT32("vin1-uv", RK3588SARADCState, inputs_uv[1], 0),
    DEFINE_PROP_UINT32("vin2-uv", RK3588SARADCState, inputs_uv[2], 0),
    DEFINE_PROP_UINT32("vin3-uv", RK3588SARADCState, inputs_uv[3], 0),
    DEFINE_PROP_UINT32("vin4-uv", RK3588SARADCState, inputs_uv[4], 0),
    DEFINE_PROP_UINT32("vin5-uv", RK3588SARADCState, inputs_uv[5], 0),
    DEFINE_PROP_UINT32("vin6-uv", RK3588SARADCState, inputs_uv[6], 0),
    DEFINE_PROP_UINT32("vin7-uv", RK3588SARADCState, inputs_uv[7], 0),
};

static void rk3588_saradc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rk3588_saradc_realize;
    dc->vmsd = &vmstate_rk3588_saradc;
    device_class_set_legacy_reset(dc, rk3588_saradc_reset);
    device_class_set_props(dc, rk3588_saradc_properties);
    dc->user_creatable = false;
}

static const TypeInfo rk3588_saradc_info = {
    .name = TYPE_RK3588_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588SARADCState),
    .instance_init = rk3588_saradc_init,
    .instance_finalize = rk3588_saradc_finalize,
    .class_init = rk3588_saradc_class_init,
};

static void rk3588_saradc_register_types(void)
{
    type_register_static(&rk3588_saradc_info);
}

type_init(rk3588_saradc_register_types)

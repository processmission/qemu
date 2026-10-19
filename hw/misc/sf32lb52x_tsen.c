/*
 * SiFli SF32LB52X temperature sensor
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_tsen.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static bool sf32lb52x_tsen_powered(SF32LB52XTsenState *s)
{
    return (s->ctrl & (SF32LB52X_TSEN_CTRL_EN |
                       SF32LB52X_TSEN_CTRL_RSTB |
                       SF32LB52X_TSEN_CTRL_PU)) ==
           (SF32LB52X_TSEN_CTRL_EN |
            SF32LB52X_TSEN_CTRL_RSTB |
            SF32LB52X_TSEN_CTRL_PU);
}

static bool sf32lb52x_tsen_active(SF32LB52XTsenState *s)
{
    return sf32lb52x_tsen_powered(s) &&
           (s->ctrl & SF32LB52X_TSEN_CTRL_RUN) != 0;
}

static void sf32lb52x_tsen_convert(SF32LB52XTsenState *s)
{
    s->rdata = SF32LB52X_TSEN_DEFAULT_CODE;
    s->raw_isr |= SF32LB52X_TSEN_IRQ_IRSR;
}

static void sf32lb52x_tsen_timer_expire(void *opaque)
{
    SF32LB52XTsenState *s = opaque;

    if (!s->running) {
        return;
    }

    sf32lb52x_tsen_convert(s);
}

static void sf32lb52x_tsen_timer_start(SF32LB52XTsenState *s)
{
    ptimer_transaction_begin(s->timer);
    ptimer_set_limit(s->timer, 1, 1);
    ptimer_run(s->timer, 0);
    ptimer_transaction_commit(s->timer);
}

static void sf32lb52x_tsen_timer_stop(SF32LB52XTsenState *s)
{
    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_transaction_commit(s->timer);
}

static void sf32lb52x_tsen_update(SF32LB52XTsenState *s)
{
    bool active;

    if (sf32lb52x_tsen_powered(s)) {
        s->ctrl |= SF32LB52X_TSEN_CTRL_RDY;
    } else {
        s->ctrl &= ~SF32LB52X_TSEN_CTRL_RDY;
    }

    active = sf32lb52x_tsen_active(s);
    if (active && !s->running) {
        s->running = true;
        sf32lb52x_tsen_convert(s);
        sf32lb52x_tsen_timer_start(s);
    } else if (!active && s->running) {
        s->running = false;
        sf32lb52x_tsen_timer_stop(s);
    }
}

static uint64_t sf32lb52x_tsen_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    SF32LB52XTsenState *s = opaque;
    bool raw = (s->raw_isr & SF32LB52X_TSEN_IRQ_IRSR) != 0;
    uint32_t value;

    switch (addr) {
    case SF32LB52X_TSEN_CTRL:
        value = s->ctrl;
        break;
    case SF32LB52X_TSEN_RDATA:
        value = s->rdata;
        break;
    case SF32LB52X_TSEN_IRQ:
        value = raw ? SF32LB52X_TSEN_IRQ_IRSR : 0;
        value |= s->irq_mask & SF32LB52X_TSEN_IRQ_IMR;
        if (raw && !(s->irq_mask & SF32LB52X_TSEN_IRQ_IMR)) {
            value |= SF32LB52X_TSEN_IRQ_ISR;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        value = 0;
        break;
    }

    return value;
}

static void sf32lb52x_tsen_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned int size)
{
    SF32LB52XTsenState *s = opaque;

    switch (addr) {
    case SF32LB52X_TSEN_CTRL:
        s->ctrl = (uint32_t)value;
        sf32lb52x_tsen_update(s);
        break;
    case SF32LB52X_TSEN_RDATA:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: RDATA is read-only\n", __func__);
        break;
    case SF32LB52X_TSEN_IRQ:
        if (value & SF32LB52X_TSEN_IRQ_ICR) {
            s->raw_isr &= ~SF32LB52X_TSEN_IRQ_IRSR;
        }
        s->irq_mask = (uint32_t)value & SF32LB52X_TSEN_IRQ_IMR;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps sf32lb52x_tsen_ops = {
    .read = sf32lb52x_tsen_read,
    .write = sf32lb52x_tsen_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sf32lb52x_tsen_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XTsenState *s = SF32LB52X_TSEN(obj);

    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_transaction_commit(s->timer);

    s->ctrl = SF32LB52X_TSEN_CTRL_RESET;
    s->rdata = 0;
    s->raw_isr = 0;
    s->irq_mask = 0;
    s->running = false;
}

static void sf32lb52x_tsen_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XTsenState *s = SF32LB52X_TSEN(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_tsen_ops, s,
                          TYPE_SF32LB52X_TSEN, SF32LB52X_TSEN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    s->timer = ptimer_init(sf32lb52x_tsen_timer_expire, s,
                           PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                           PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, SF32LB52X_TSEN_CONV_HZ);
    ptimer_transaction_commit(s->timer);
}

static const VMStateDescription vmstate_sf32lb52x_tsen = {
    .name = TYPE_SF32LB52X_TSEN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PTIMER(timer, SF32LB52XTsenState),
        VMSTATE_UINT32(ctrl, SF32LB52XTsenState),
        VMSTATE_UINT32(rdata, SF32LB52XTsenState),
        VMSTATE_UINT32(raw_isr, SF32LB52XTsenState),
        VMSTATE_UINT32(irq_mask, SF32LB52XTsenState),
        VMSTATE_BOOL(running, SF32LB52XTsenState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_tsen_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_tsen_reset_hold;
    dc->realize = sf32lb52x_tsen_realize;
    dc->desc = "SiFli SF32LB52X temperature sensor";
    dc->vmsd = &vmstate_sf32lb52x_tsen;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_tsen_types[] = {
    {
        .name = TYPE_SF32LB52X_TSEN,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XTsenState),
        .class_init = sf32lb52x_tsen_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_tsen_types);

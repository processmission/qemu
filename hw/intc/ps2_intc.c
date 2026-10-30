/*
 * Sony PlayStation 2 Emotion Engine interrupt controller (INTC)
 *
 * The INTC latches events from 16 sources into INTC_STAT and forwards
 * the unmasked ones to the R5900 interrupt line IP2.  INTC_STAT is
 * write-1-to-clear and INTC_MASK is write-1-to-toggle.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/intc/ps2_intc.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void ps2_intc_update(PS2INTCState *s)
{
    qemu_set_irq(s->irq, (s->stat & s->mask) != 0);
}

/* Sources are latched on their rising edge, so devices may pulse them. */
static void ps2_intc_set_irq(void *opaque, int n, int level)
{
    PS2INTCState *s = opaque;
    uint32_t bit = 1u << n;

    if (level && !(s->level & bit)) {
        s->stat |= bit;
    }
    if (level) {
        s->level |= bit;
    } else {
        s->level &= ~bit;
    }
    ps2_intc_update(s);
}

static uint64_t ps2_intc_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2INTCState *s = opaque;

    switch (addr) {
    case PS2_INTC_STAT:
        return s->stat;
    case PS2_INTC_MASK:
        return s->mask;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void ps2_intc_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned int size)
{
    PS2INTCState *s = opaque;

    value &= (1u << PS2_INTC_NUM_IRQS) - 1;

    switch (addr) {
    case PS2_INTC_STAT:
        s->stat &= ~value;
        break;
    case PS2_INTC_MASK:
        s->mask ^= value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }
    ps2_intc_update(s);
}

static const MemoryRegionOps ps2_intc_ops = {
    .read = ps2_intc_read,
    .write = ps2_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void ps2_intc_reset_hold(Object *obj, ResetType type)
{
    PS2INTCState *s = PS2_INTC(obj);

    s->stat = 0;
    s->mask = 0;
    ps2_intc_update(s);
}

static void ps2_intc_init(Object *obj)
{
    PS2INTCState *s = PS2_INTC(obj);

    memory_region_init_io(&s->iomem, obj, &ps2_intc_ops, s, TYPE_PS2_INTC,
                          PS2_INTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(DEVICE(obj), ps2_intc_set_irq, PS2_INTC_NUM_IRQS);
}

static const VMStateDescription vmstate_ps2_intc = {
    .name = TYPE_PS2_INTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(stat, PS2INTCState),
        VMSTATE_UINT32(mask, PS2INTCState),
        VMSTATE_UINT32(level, PS2INTCState),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_intc_reset_hold;
    dc->vmsd = &vmstate_ps2_intc;
}

static const TypeInfo ps2_intc_types[] = {
    {
        .name = TYPE_PS2_INTC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2INTCState),
        .instance_init = ps2_intc_init,
        .class_init = ps2_intc_class_init,
    },
};

DEFINE_TYPES(ps2_intc_types);

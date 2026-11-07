/*
 * SiFli SF32LB52X general purpose SAR ADC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_gpadc.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * Placeholder channel codes for the QEMU model. The real values depend on the
 * analog input; they are deterministic so qtests can assert the RDATA slot
 * mapping. Board wiring can override them in a later revision with a property.
 */
static const uint16_t
sf32lb52x_gpadc_default_code[SF32LB52X_GPADC_MAX_CHANNELS] = {
    0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400, 0x480,
};

static void sf32lb52x_gpadc_update_irq(SF32LB52XGpadcState *s)
{
    bool asserted = (s->raw_isr & SF32LB52X_GPADC_IRQ_IRSR) != 0 &&
                    (s->irq_mask & SF32LB52X_GPADC_IRQ_IMR) == 0;

    qemu_set_irq(s->irq, asserted);
}

static void sf32lb52x_gpadc_convert(SF32LB52XGpadcState *s)
{
    uint32_t done = 0;
    uint32_t cur_slot = 0;
    int i;

    for (i = 0; i < SF32LB52X_GPADC_MAX_SLOTS; i++) {
        uint32_t slot = s->slots[i];
        uint32_t channel = (slot & SF32LB52X_GPADC_SLOT_PCHNL_SEL_MASK) >> 8;
        uint32_t reg = i / 2;
        uint32_t code = sf32lb52x_gpadc_default_code[channel];

        if (!(slot & SF32LB52X_GPADC_SLOT_EN)) {
            continue;
        }

        if (i & 1) {
            s->rdata[reg] = (s->rdata[reg] & ~0x0fff0000U) | (code << 16);
        } else {
            s->rdata[reg] = (s->rdata[reg] & ~0x00000fffU) | code;
        }
        done |= 1U << (i + 1);
        cur_slot = i;
    }

    s->status = SF32LB52X_GPADC_STATUS_ADC_DONE | done |
                (cur_slot << 9);
    s->raw_isr |= SF32LB52X_GPADC_IRQ_IRSR;
    sf32lb52x_gpadc_update_irq(s);
}

static uint64_t sf32lb52x_gpadc_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    SF32LB52XGpadcState *s = opaque;
    uint32_t value;

    if (addr >= SF32LB52X_GPADC_SLOT(0) &&
        addr < SF32LB52X_GPADC_SLOT(SF32LB52X_GPADC_MAX_SLOTS)) {
        return s->slots[(addr - SF32LB52X_GPADC_SLOT(0)) / 4];
    }
    if (addr >= SF32LB52X_GPADC_RDATA(0) &&
        addr < SF32LB52X_GPADC_DMA_RDATA) {
        return s->rdata[(addr - SF32LB52X_GPADC_RDATA(0)) / 4];
    }

    switch (addr) {
    case SF32LB52X_GPADC_CFG1:
        value = s->cfg1;
        break;
    case SF32LB52X_GPADC_DMA_RDATA:
        value = s->dma_rdata;
        break;
    case SF32LB52X_GPADC_CTRL:
        value = s->ctrl;
        break;
    case SF32LB52X_GPADC_CTRL2:
        value = s->ctrl2;
        break;
    case SF32LB52X_GPADC_STATUS:
        value = s->status;
        break;
    case SF32LB52X_GPADC_IRQ:
        value = s->raw_isr & SF32LB52X_GPADC_IRQ_IRSR;
        value |= s->irq_mask & SF32LB52X_GPADC_IRQ_IMR;
        if ((s->raw_isr & SF32LB52X_GPADC_IRQ_IRSR) &&
            !(s->irq_mask & SF32LB52X_GPADC_IRQ_IMR)) {
            value |= SF32LB52X_GPADC_IRQ_ISR;
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

static void sf32lb52x_gpadc_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned int size)
{
    SF32LB52XGpadcState *s = opaque;

    if (addr >= SF32LB52X_GPADC_SLOT(0) &&
        addr < SF32LB52X_GPADC_SLOT(SF32LB52X_GPADC_MAX_SLOTS)) {
        s->slots[(addr - SF32LB52X_GPADC_SLOT(0)) / 4] =
            (uint32_t)value & SF32LB52X_GPADC_SLOT_WMASK;
        return;
    }
    if (addr >= SF32LB52X_GPADC_RDATA(0) &&
        addr < SF32LB52X_GPADC_DMA_RDATA) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: RDATA is read-only\n", __func__);
        return;
    }

    switch (addr) {
    case SF32LB52X_GPADC_CFG1:
        s->cfg1 = (uint32_t)value;
        break;
    case SF32LB52X_GPADC_CTRL:
        if (value & SF32LB52X_GPADC_CTRL_OP_MODE) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: continuous conversion mode not modeled\n",
                          __func__);
        }
        s->ctrl = (uint32_t)value & ~SF32LB52X_GPADC_CTRL_START;
        if (value & SF32LB52X_GPADC_CTRL_START) {
            sf32lb52x_gpadc_convert(s);
        }
        break;
    case SF32LB52X_GPADC_CTRL2:
        s->ctrl2 = (uint32_t)value;
        break;
    case SF32LB52X_GPADC_IRQ:
        if (value & SF32LB52X_GPADC_IRQ_ICR) {
            s->raw_isr &= ~SF32LB52X_GPADC_IRQ_IRSR;
        }
        s->irq_mask = (uint32_t)value & SF32LB52X_GPADC_IRQ_IMR;
        sf32lb52x_gpadc_update_irq(s);
        break;
    case SF32LB52X_GPADC_DMA_RDATA:
    case SF32LB52X_GPADC_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register is read-only\n",
                      __func__);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps sf32lb52x_gpadc_ops = {
    .read = sf32lb52x_gpadc_read,
    .write = sf32lb52x_gpadc_write,
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

static void sf32lb52x_gpadc_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XGpadcState *s = SF32LB52X_GPADC(obj);
    int i;

    s->cfg1 = SF32LB52X_GPADC_CFG1_RESET;
    for (i = 0; i < SF32LB52X_GPADC_MAX_SLOTS; i++) {
        s->slots[i] = SF32LB52X_GPADC_SLOT_RESET;
    }
    memset(s->rdata, 0, sizeof(s->rdata));
    s->dma_rdata = 0;
    s->ctrl = SF32LB52X_GPADC_CTRL_RESET;
    s->ctrl2 = SF32LB52X_GPADC_CTRL_RESET;
    s->status = 0;
    s->raw_isr = 0;
    s->irq_mask = 0;
    sf32lb52x_gpadc_update_irq(s);
}

static void sf32lb52x_gpadc_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XGpadcState *s = SF32LB52X_GPADC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_gpadc_ops, s,
                          TYPE_SF32LB52X_GPADC, SF32LB52X_GPADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_sf32lb52x_gpadc = {
    .name = TYPE_SF32LB52X_GPADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cfg1, SF32LB52XGpadcState),
        VMSTATE_UINT32_ARRAY(slots, SF32LB52XGpadcState,
                             SF32LB52X_GPADC_MAX_SLOTS),
        VMSTATE_UINT32_ARRAY(rdata, SF32LB52XGpadcState,
                             SF32LB52X_GPADC_MAX_SLOTS / 2),
        VMSTATE_UINT32(dma_rdata, SF32LB52XGpadcState),
        VMSTATE_UINT32(ctrl, SF32LB52XGpadcState),
        VMSTATE_UINT32(ctrl2, SF32LB52XGpadcState),
        VMSTATE_UINT32(status, SF32LB52XGpadcState),
        VMSTATE_UINT32(raw_isr, SF32LB52XGpadcState),
        VMSTATE_UINT32(irq_mask, SF32LB52XGpadcState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_gpadc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_gpadc_reset_hold;
    dc->realize = sf32lb52x_gpadc_realize;
    dc->desc = "SiFli SF32LB52X general purpose SAR ADC";
    dc->vmsd = &vmstate_sf32lb52x_gpadc;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_gpadc_types[] = {
    {
        .name = TYPE_SF32LB52X_GPADC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XGpadcState),
        .class_init = sf32lb52x_gpadc_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_gpadc_types);

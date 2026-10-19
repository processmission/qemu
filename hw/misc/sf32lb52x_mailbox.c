/*
 * SiFli SF32LB52X mailbox controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Transport-level model of the HPSYS mailboxes. Each channel block has six
 * registers (HAL mailbox1.h:10-36, mailbox2.h:10-24):
 *   CxIER 0x00 interrupt enable (1 = unmask)
 *   CxITR 0x04 interrupt trigger (write 1 sets the matching CxISR bit)
 *   CxICR 0x08 interrupt clear (write 1 clears the matching CxISR bit)
 *   CxISR 0x0c raw interrupt status
 *   CxMISR 0x10 masked interrupt status (ISR & IER)
 *   CxEXR 0x14 mutex state: ID[3:0] owner core, EX[31] unlocked
 * Blocks are 0x18 bytes apart and each exposes 16 channels (INT0..INT15).
 * MAILBOX1 (HPSYS, 0x50082000, H2L) has 4 blocks; MAILBOX2 (LPSYS,
 * 0x40002000, L2H) has 2.
 *
 * Register layout follows the vendor HAL bf0_hal_mailbox.h:176-258 and
 * bf0_hal_mailbox.c:187-247. The peer core is NOT modeled: the model is a
 * transport-level register bank with level interrupt aggregation; it does
 * not emulate LCPU execution or the shared-memory IPC queues.
 *
 * Manual MemoryRegionOps is used instead of RegisterInfo because every
 * register participates in a write-1 trigger/clear, derived masked status,
 * mutex state, or the OR-of-MISR interrupt line; a per-register framework
 * would duplicate the same state machine.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_mailbox.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void sf32lb52x_mailbox_update_irq(SF32LB52XMailboxState *s)
{
    bool asserted = false;
    uint32_t i;

    for (i = 0; i < s->num_blocks; i++) {
        if (s->isr[i] & s->ier[i] & SF32LB52X_MAILBOX_CHANNEL_MASK) {
            asserted = true;
            break;
        }
    }

    qemu_set_irq(s->irq, asserted);
}

static uint64_t sf32lb52x_mailbox_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    SF32LB52XMailboxState *s = opaque;
    uint32_t block = addr / SF32LB52X_MAILBOX_BLOCK_STRIDE;
    uint32_t reg = addr % SF32LB52X_MAILBOX_BLOCK_STRIDE;

    if (addr >= SF32LB52X_MAILBOX_SIZE || block >= s->num_blocks ||
        (reg & 0x3) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    switch (reg) {
    case SF32LB52X_MAILBOX_IER:
        return s->ier[block];
    case SF32LB52X_MAILBOX_ITR:
    case SF32LB52X_MAILBOX_ICR:
        /* Write-1 trigger/clear registers, reads return zero. */
        return 0;
    case SF32LB52X_MAILBOX_ISR:
        return s->isr[block];
    case SF32LB52X_MAILBOX_MISR:
        return s->isr[block] & s->ier[block] &
               SF32LB52X_MAILBOX_CHANNEL_MASK;
    case SF32LB52X_MAILBOX_EXR:
        return s->exr[block];
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void sf32lb52x_mailbox_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned int size)
{
    SF32LB52XMailboxState *s = opaque;
    uint32_t block = addr / SF32LB52X_MAILBOX_BLOCK_STRIDE;
    uint32_t reg = addr % SF32LB52X_MAILBOX_BLOCK_STRIDE;

    if (addr >= SF32LB52X_MAILBOX_SIZE || block >= s->num_blocks ||
        (reg & 0x3) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    switch (reg) {
    case SF32LB52X_MAILBOX_IER:
        s->ier[block] = value & SF32LB52X_MAILBOX_CHANNEL_MASK;
        sf32lb52x_mailbox_update_irq(s);
        break;
    case SF32LB52X_MAILBOX_ITR:
        s->isr[block] |= value & SF32LB52X_MAILBOX_CHANNEL_MASK;
        sf32lb52x_mailbox_update_irq(s);
        break;
    case SF32LB52X_MAILBOX_ICR:
        s->isr[block] &= ~(value & SF32LB52X_MAILBOX_CHANNEL_MASK);
        sf32lb52x_mailbox_update_irq(s);
        break;
    case SF32LB52X_MAILBOX_ISR:
    case SF32LB52X_MAILBOX_MISR:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: register is read-only\n",
                      __func__);
        break;
    case SF32LB52X_MAILBOX_EXR:
        s->exr[block] = value & SF32LB52X_MAILBOX_EXR_MASK;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps sf32lb52x_mailbox_ops = {
    .read = sf32lb52x_mailbox_read,
    .write = sf32lb52x_mailbox_write,
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

static void sf32lb52x_mailbox_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XMailboxState *s = SF32LB52X_MAILBOX(obj);

    memset(s->ier, 0, sizeof(s->ier));
    memset(s->isr, 0, sizeof(s->isr));
    memset(s->exr, 0, sizeof(s->exr));
    sf32lb52x_mailbox_update_irq(s);
}

static void sf32lb52x_mailbox_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XMailboxState *s = SF32LB52X_MAILBOX(dev);

    if (s->num_blocks == 0 || s->num_blocks > SF32LB52X_MAILBOX_MAX_BLOCKS) {
        error_setg(errp, "%s: num-blocks must be 1..%u", TYPE_SF32LB52X_MAILBOX,
                   SF32LB52X_MAILBOX_MAX_BLOCKS);
        return;
    }

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_mailbox_ops, s,
                          TYPE_SF32LB52X_MAILBOX, SF32LB52X_MAILBOX_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_sf32lb52x_mailbox = {
    .name = TYPE_SF32LB52X_MAILBOX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ier, SF32LB52XMailboxState,
                             SF32LB52X_MAILBOX_MAX_BLOCKS),
        VMSTATE_UINT32_ARRAY(isr, SF32LB52XMailboxState,
                             SF32LB52X_MAILBOX_MAX_BLOCKS),
        VMSTATE_UINT32_ARRAY(exr, SF32LB52XMailboxState,
                             SF32LB52X_MAILBOX_MAX_BLOCKS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sf32lb52x_mailbox_properties[] = {
    /*
     * MAILBOX1 implements 4 channel blocks, MAILBOX2 only 2. The SoC sets
     * this per instance; the reset default keeps standalone use at maximum.
     */
    DEFINE_PROP_UINT32("num-blocks", SF32LB52XMailboxState, num_blocks,
                       SF32LB52X_MAILBOX_MAX_BLOCKS),
};

static void sf32lb52x_mailbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_mailbox_reset_hold;
    dc->realize = sf32lb52x_mailbox_realize;
    dc->desc = "SiFli SF32LB52X mailbox controller";
    dc->vmsd = &vmstate_sf32lb52x_mailbox;
    dc->user_creatable = false;
    device_class_set_props(dc, sf32lb52x_mailbox_properties);
}

static const TypeInfo sf32lb52x_mailbox_types[] = {
    {
        .name = TYPE_SF32LB52X_MAILBOX,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XMailboxState),
        .class_init = sf32lb52x_mailbox_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_mailbox_types);

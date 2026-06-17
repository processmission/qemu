/*
 * Phytium SCP mailbox model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/phytium-scp-mailbox.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define PHYTIUM_SCP_MAILBOX_STATUS_OFFSET 0x04
#define PHYTIUM_SCP_MAILBOX_CONTROL_OFFSET 0x10
#define PHYTIUM_SCP_MAILBOX_RESPONSE_OFFSET 0x1c
#define PHYTIUM_SCP_MAILBOX_DONE 0x1

static void phytium_scp_mailbox_complete(PhytiumScpMailboxState *s)
{
    s->regs[PHYTIUM_SCP_MAILBOX_STATUS_OFFSET / sizeof(uint32_t)] |=
        PHYTIUM_SCP_MAILBOX_DONE;
    s->regs[PHYTIUM_SCP_MAILBOX_RESPONSE_OFFSET / sizeof(uint32_t)] = 0;
}

static uint64_t phytium_scp_mailbox_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    PhytiumScpMailboxState *s = opaque;
    unsigned int index = offset / sizeof(uint32_t);
    unsigned int shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;

    if (index >= ARRAY_SIZE(s->regs)) {
        return 0;
    }

    if ((offset & ~3) == PHYTIUM_SCP_MAILBOX_STATUS_OFFSET) {
        phytium_scp_mailbox_complete(s);
    }

    return (s->regs[index] >> shift) & mask;
}

static void phytium_scp_mailbox_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    PhytiumScpMailboxState *s = opaque;
    unsigned int index = offset / sizeof(uint32_t);
    unsigned int shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : ((1u << (size * 8)) - 1) << shift;
    uint32_t *reg;

    if (index >= ARRAY_SIZE(s->regs)) {
        return;
    }

    reg = &s->regs[index];
    *reg = (*reg & ~mask) | (((uint32_t)value << shift) & mask);
    if ((offset & ~3) == PHYTIUM_SCP_MAILBOX_STATUS_OFFSET ||
        (offset & ~3) == PHYTIUM_SCP_MAILBOX_CONTROL_OFFSET) {
        phytium_scp_mailbox_complete(s);
    }
}

static const MemoryRegionOps phytium_scp_mailbox_ops = {
    .read = phytium_scp_mailbox_read,
    .write = phytium_scp_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void phytium_scp_mailbox_reset(DeviceState *dev)
{
    PhytiumScpMailboxState *s = PHYTIUM_SCP_MAILBOX(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void phytium_scp_mailbox_init(Object *obj)
{
    PhytiumScpMailboxState *s = PHYTIUM_SCP_MAILBOX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &phytium_scp_mailbox_ops, s,
                          TYPE_PHYTIUM_SCP_MAILBOX,
                          PHYTIUM_SCP_MAILBOX_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_phytium_scp_mailbox = {
    .name = TYPE_PHYTIUM_SCP_MAILBOX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, PhytiumScpMailboxState,
                             PHYTIUM_SCP_MAILBOX_MMIO_SIZE /
                             sizeof(uint32_t)),
        VMSTATE_END_OF_LIST()
    },
};

static void phytium_scp_mailbox_class_init(ObjectClass *klass,
                                           const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, phytium_scp_mailbox_reset);
    dc->vmsd = &vmstate_phytium_scp_mailbox;
}

static const TypeInfo phytium_scp_mailbox_info = {
    .name = TYPE_PHYTIUM_SCP_MAILBOX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PhytiumScpMailboxState),
    .instance_init = phytium_scp_mailbox_init,
    .class_init = phytium_scp_mailbox_class_init,
};

static void phytium_scp_mailbox_register_types(void)
{
    type_register_static(&phytium_scp_mailbox_info);
}

type_init(phytium_scp_mailbox_register_types)

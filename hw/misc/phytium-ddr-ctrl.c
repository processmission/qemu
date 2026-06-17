/*
 * Phytium DDR controller shim
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/phytium-ddr-ctrl.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define PHYTIUM_DDR_CTRL_INDEX_OFFSET 0x80
#define PHYTIUM_DDR_CTRL_DATA_OFFSET 0x84
#define PHYTIUM_DDR_CTRL_POLL_INDEX ((0x76 + 0x800) << 2)
#define PHYTIUM_DDR_CTRL_DONE (1u << 27)
#define PHYTIUM_DDR_CTRL_READY_BITS \
    (PHYTIUM_DDR_CTRL_DONE | (1u << 25) | 1u)

static uint64_t phytium_ddr_ctrl_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    PhytiumDdrCtrlState *s = opaque;
    unsigned int index = offset / sizeof(uint32_t);
    unsigned int shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    uint32_t ddr_index;
    uint32_t value;

    if (index >= ARRAY_SIZE(s->regs)) {
        return 0;
    }

    value = s->regs[index];
    ddr_index = s->regs[PHYTIUM_DDR_CTRL_INDEX_OFFSET / sizeof(uint32_t)];

    if ((offset & ~3) == PHYTIUM_DDR_CTRL_DATA_OFFSET) {
        if (ddr_index == PHYTIUM_DDR_CTRL_POLL_INDEX) {
            value = PHYTIUM_DDR_CTRL_DONE;
        } else if (ddr_index < PHYTIUM_DDR_INDEX_SPACE_SIZE) {
            unsigned int ddr_reg = ddr_index / sizeof(uint32_t);

            value = s->index_regs[ddr_reg] ?: PHYTIUM_DDR_CTRL_READY_BITS;
        }
    }

    return (value >> shift) & mask;
}

static void phytium_ddr_ctrl_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    PhytiumDdrCtrlState *s = opaque;
    unsigned int index = offset / sizeof(uint32_t);
    unsigned int shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : ((1u << (size * 8)) - 1) << shift;
    uint32_t ddr_index;
    uint32_t *reg;

    if (index >= ARRAY_SIZE(s->regs)) {
        return;
    }

    reg = &s->regs[index];
    ddr_index = s->regs[PHYTIUM_DDR_CTRL_INDEX_OFFSET / sizeof(uint32_t)];
    *reg = (*reg & ~mask) | (((uint32_t)value << shift) & mask);

    if ((offset & ~3) == PHYTIUM_DDR_CTRL_DATA_OFFSET &&
        ddr_index < PHYTIUM_DDR_INDEX_SPACE_SIZE) {
        unsigned int ddr_reg = ddr_index / sizeof(uint32_t);

        s->index_regs[ddr_reg] = *reg;
    }
}

static const MemoryRegionOps phytium_ddr_ctrl_ops = {
    .read = phytium_ddr_ctrl_read,
    .write = phytium_ddr_ctrl_write,
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

static void phytium_ddr_ctrl_reset(DeviceState *dev)
{
    PhytiumDdrCtrlState *s = PHYTIUM_DDR_CTRL(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->index_regs, 0, sizeof(s->index_regs));
}

static void phytium_ddr_ctrl_init(Object *obj)
{
    PhytiumDdrCtrlState *s = PHYTIUM_DDR_CTRL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &phytium_ddr_ctrl_ops, s,
                          TYPE_PHYTIUM_DDR_CTRL,
                          PHYTIUM_DDR_CTRL_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_phytium_ddr_ctrl = {
    .name = TYPE_PHYTIUM_DDR_CTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, PhytiumDdrCtrlState,
                             PHYTIUM_DDR_CTRL_MMIO_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_ARRAY(index_regs, PhytiumDdrCtrlState,
                             PHYTIUM_DDR_INDEX_SPACE_SIZE / sizeof(uint32_t)),
        VMSTATE_END_OF_LIST()
    },
};

static void phytium_ddr_ctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, phytium_ddr_ctrl_reset);
    dc->vmsd = &vmstate_phytium_ddr_ctrl;
}

static const TypeInfo phytium_ddr_ctrl_info = {
    .name = TYPE_PHYTIUM_DDR_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PhytiumDdrCtrlState),
    .instance_init = phytium_ddr_ctrl_init,
    .class_init = phytium_ddr_ctrl_class_init,
};

static void phytium_ddr_ctrl_register_types(void)
{
    type_register_static(&phytium_ddr_ctrl_info);
}

type_init(phytium_ddr_ctrl_register_types)

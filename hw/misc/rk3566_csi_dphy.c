/*
 * RK3566 CSI DPHY
 *
 * Lane enable and link state for the MIPI CSI D-PHY that feeds the ISP.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

#define TYPE_RK3566_DPHY "rk3566-csi-dphy"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566DPHYState, RK3566_DPHY)

enum {
    DPHY_REG_CTRL,
    DPHY_REG_MAX,
};

struct RK3566DPHYState {
    SysBusDevice parent_obj;
    qemu_irq enabled;
    uint32_t regs[0x1000 / 4];
    RegisterInfo regs_info[DPHY_REG_MAX];
    RegisterInfoArray *reg_array;
};

static void dphy_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566DPHYState *s = RK3566_DPHY(reg->opaque);

    qemu_set_irq(s->enabled, (val & 0x4c) == 0x4c);
}

static const RegisterAccessInfo dphy_regs_info[] = {
    { .name = "LANE_ENABLE", .addr = 0, .reset = 1,
      .post_write = dphy_ctrl_post_write },
};

static uint64_t dphy_read(void *opaque, hwaddr address, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566DPHYState *s = RK3566_DPHY(register_array_get_owner(reg_array));

    if (address == 0) {
        return register_read_memory(reg_array, address, size);
    }
    return s->regs[address / 4];
}

static void dphy_write(void *opaque, hwaddr address, uint64_t value,
                       unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566DPHYState *s = RK3566_DPHY(register_array_get_owner(reg_array));

    if (address == 0) {
        register_write_memory(reg_array, address, value, size);
        return;
    }
    s->regs[address / 4] = value;
}

static const MemoryRegionOps dphy_ops = {
    .read = dphy_read, .write = dphy_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void dphy_reset(DeviceState *device)
{
    RK3566DPHYState *s = RK3566_DPHY(device);

    memset(s->regs, 0, sizeof(s->regs));
    register_reset(&s->regs_info[DPHY_REG_CTRL]);
    qemu_set_irq(s->enabled, 0);
}

static void dphy_init(Object *object)
{
    RK3566DPHYState *s = RK3566_DPHY(object);

    s->reg_array = register_init_block32(DEVICE(object), dphy_regs_info,
                                         ARRAY_SIZE(dphy_regs_info),
                                         s->regs_info, s->regs,
                                         &dphy_ops, false, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->reg_array->mem);
    qdev_init_gpio_out_named(DEVICE(s), &s->enabled, "link-enabled", 1);
}

static const VMStateDescription dphy_vmstate = {
    .name = TYPE_RK3566_DPHY, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3566DPHYState, 0x1000 / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void dphy_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    device_class_set_legacy_reset(device, dphy_reset);
    device->vmsd = &dphy_vmstate;
}

static const TypeInfo dphy_types[] = {
    { .name = TYPE_RK3566_DPHY, .parent = TYPE_SYS_BUS_DEVICE,
      .instance_size = sizeof(RK3566DPHYState), .instance_init = dphy_init,
      .class_init = dphy_class_init },
};

DEFINE_TYPES(dphy_types)

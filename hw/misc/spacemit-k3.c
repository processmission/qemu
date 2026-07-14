/*
 * SpacemiT K3 clock and boot controls
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/spacemit-k3.h"
#include "migration/vmstate.h"

#define K3_APMU_SDH0_CTRL              0x54
#define K3_APMU_SDH0_AXI_RESET_N       BIT(0)
#define K3_APMU_SDH0_RESET_N           BIT(1)
#define K3_APMU_SDH0_AXI_CLK_EN        BIT(3)
#define K3_APMU_SDH0_CLK_EN            BIT(4)
#define K3_APMU_SDH0_CLK_MUX_MASK      (0x7U << 5)
#define K3_APMU_SDH0_CLK_DIV_MASK      (0x7U << 8)
#define K3_APMU_SDH0_CLK_DIV_DEFAULT   (0x1U << 8)
#define K3_APMU_SDH0_CLK_FC            BIT(11)
#define K3_APMU_SDH0_CTRL_MASK         (K3_APMU_SDH0_AXI_RESET_N | \
                                        K3_APMU_SDH0_RESET_N | \
                                        K3_APMU_SDH0_AXI_CLK_EN | \
                                        K3_APMU_SDH0_CLK_EN | \
                                        K3_APMU_SDH0_CLK_MUX_MASK | \
                                        K3_APMU_SDH0_CLK_DIV_MASK | \
                                        K3_APMU_SDH0_CLK_FC)
#define K3_APMU_SDH0_CTRL_RESET        (K3_APMU_SDH0_AXI_RESET_N | \
                                        K3_APMU_SDH0_AXI_CLK_EN | \
                                        K3_APMU_SDH0_CLK_EN | \
                                        K3_APMU_SDH0_CLK_DIV_DEFAULT)

#define K3_CIU_BOOT_FLAG               0x110
#define K3_CIU_BOOT_FROM_SD            0xb10

static uint64_t spacemit_k3_apmu_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    SpacemitK3APMUState *s = SPACEMIT_K3_APMU(opaque);

    switch (addr) {
    case K3_APMU_SDH0_CTRL:
        return s->sdh0_ctrl;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void spacemit_k3_apmu_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned int size)
{
    SpacemitK3APMUState *s = SPACEMIT_K3_APMU(opaque);

    switch (addr) {
    case K3_APMU_SDH0_CTRL:
        s->sdh0_ctrl = value & K3_APMU_SDH0_CTRL_MASK &
                       ~K3_APMU_SDH0_CLK_FC;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps spacemit_k3_apmu_ops = {
    .read = spacemit_k3_apmu_read,
    .write = spacemit_k3_apmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void spacemit_k3_apmu_reset(DeviceState *dev)
{
    SpacemitK3APMUState *s = SPACEMIT_K3_APMU(dev);

    s->sdh0_ctrl = K3_APMU_SDH0_CTRL_RESET;
}

static const VMStateDescription vmstate_spacemit_k3_apmu = {
    .name = TYPE_SPACEMIT_K3_APMU,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sdh0_ctrl, SpacemitK3APMUState),
        VMSTATE_END_OF_LIST(),
    },
};

static void spacemit_k3_apmu_realize(DeviceState *dev, Error **errp)
{
    SpacemitK3APMUState *s = SPACEMIT_K3_APMU(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &spacemit_k3_apmu_ops, s,
                          TYPE_SPACEMIT_K3_APMU,
                          SPACEMIT_K3_APMU_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void spacemit_k3_apmu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = spacemit_k3_apmu_realize;
    device_class_set_legacy_reset(dc, spacemit_k3_apmu_reset);
    dc->vmsd = &vmstate_spacemit_k3_apmu;
    dc->desc = "SpacemiT K3 application power management unit";
}

static const TypeInfo spacemit_k3_apmu_type_info = {
    .name = TYPE_SPACEMIT_K3_APMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SpacemitK3APMUState),
    .class_init = spacemit_k3_apmu_class_init,
};

static uint64_t spacemit_k3_ciu_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    switch (addr) {
    case K3_CIU_BOOT_FLAG:
        return K3_CIU_BOOT_FROM_SD;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void spacemit_k3_ciu_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size)
{
    switch (addr) {
    case K3_CIU_BOOT_FLAG:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only boot flag\n", __func__);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps spacemit_k3_ciu_ops = {
    .read = spacemit_k3_ciu_read,
    .write = spacemit_k3_ciu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void spacemit_k3_ciu_realize(DeviceState *dev, Error **errp)
{
    SpacemitK3CIUState *s = SPACEMIT_K3_CIU(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &spacemit_k3_ciu_ops, s,
                          TYPE_SPACEMIT_K3_CIU,
                          SPACEMIT_K3_CIU_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void spacemit_k3_ciu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = spacemit_k3_ciu_realize;
    dc->desc = "SpacemiT K3 chip interface unit";
}

static const TypeInfo spacemit_k3_ciu_type_info = {
    .name = TYPE_SPACEMIT_K3_CIU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SpacemitK3CIUState),
    .class_init = spacemit_k3_ciu_class_init,
};

static void spacemit_k3_control_register_types(void)
{
    type_register_static(&spacemit_k3_apmu_type_info);
    type_register_static(&spacemit_k3_ciu_type_info);
}

type_init(spacemit_k3_control_register_types)

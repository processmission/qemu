/*
 * Rockchip RK3588 vendor firmware MMIO compatibility region
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Rockchip vendor TPL, SPL and BL31 binaries touch undocumented SoC
 * registers before the individual peripherals are initialized.  Keep a
 * low-priority, RAM-backed compatibility region for those accesses.  Board
 * code maps concrete peripheral devices over this region at higher priority.
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3588_firmware_mmio.h"

#include "qapi/error.h"
#include "qemu/module.h"

struct RK3588FirmwareMMIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
};

static void rk3588_firmware_mmio_reset_hold(Object *obj, ResetType type)
{
    RK3588FirmwareMMIOState *s = RK3588_FIRMWARE_MMIO(obj);

    memset(memory_region_get_ram_ptr(&s->iomem), 0xff,
           RK3588_FIRMWARE_MMIO_SIZE);
}

static void rk3588_firmware_mmio_realize(DeviceState *dev, Error **errp)
{
    RK3588FirmwareMMIOState *s = RK3588_FIRMWARE_MMIO(dev);

    memory_region_init_ram(&s->iomem, OBJECT(s), TYPE_RK3588_FIRMWARE_MMIO,
                           RK3588_FIRMWARE_MMIO_SIZE, errp);
    if (*errp) {
        return;
    }

    memset(memory_region_get_ram_ptr(&s->iomem), 0xff,
           RK3588_FIRMWARE_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void rk3588_firmware_mmio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rk3588_firmware_mmio_realize;
    dc->user_creatable = false;
    rc->phases.hold = rk3588_firmware_mmio_reset_hold;
}

static const TypeInfo rk3588_firmware_mmio_info = {
    .name = TYPE_RK3588_FIRMWARE_MMIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588FirmwareMMIOState),
    .class_init = rk3588_firmware_mmio_class_init,
};

static void rk3588_firmware_mmio_register_types(void)
{
    type_register_static(&rk3588_firmware_mmio_info);
}

type_init(rk3588_firmware_mmio_register_types)

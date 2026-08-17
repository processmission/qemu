/*
 * RK3588 DMAC (pl330) stub
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * RAM-backed window for the RK3588 DMAC0/DMAC1 controllers so the
 * kernel's pl330 amba driver probes and registers the DT DMA provider.
 * Consumer drivers (e.g. spi-rockchip) request channels at probe and
 * defer forever if the provider is missing; transfers never reach the
 * DMAC in the modeled paths (the SPI controller only uses DMA for
 * transfers of 64+ words), so a benign register bank with the correct
 * PrimeCell IDs is sufficient.
 *
 * The amba bus reads the peripheral ID registers at the end of the
 * 0x4000 window (size - 0x20..size - 0x10).  Values below make
 * periphid = 0x00041330 (PART 0x330, DESIGNER 0x41, REV 0) and
 * cid = 0xb105f00d, matching the vendor pl330 amba id table.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/misc/rk3588_pl330.h"
#include "migration/vmstate.h"

#define PL330_PIDR_BASE (RK3588_PL330_MMIO_SIZE - 0x20)
#define PL330_CIDR_BASE (RK3588_PL330_MMIO_SIZE - 0x10)

static uint64_t rk3588_pl330_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3588PL330State *s = opaque;
    uint32_t value = 0;
    int i;

    if (offset + size <= RK3588_PL330_MMIO_SIZE) {
        for (i = 0; i < size; i++) {
            value |= (uint32_t)s->storage[offset + i] << (8 * i);
        }
    }
    return value;
}

static void rk3588_pl330_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RK3588PL330State *s = opaque;
    int i;

    if (offset + size > RK3588_PL330_MMIO_SIZE) {
        return;
    }
    for (i = 0; i < size; i++) {
        s->storage[offset + i] = (value >> (8 * i)) & 0xff;
    }
}

static const MemoryRegionOps rk3588_pl330_ops = {
    .read = rk3588_pl330_read,
    .write = rk3588_pl330_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void rk3588_pl330_realize(DeviceState *dev, Error **errp)
{
    RK3588PL330State *s = RK3588_PL330(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    static const uint8_t pidr[] = { 0x30, 0x13, 0x04, 0x00 };
    static const uint8_t cidr[] = { 0x0d, 0xf0, 0x05, 0xb1 };
    int i;

    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
    for (i = 0; i < 4; i++) {
        s->storage[PL330_PIDR_BASE + i] = pidr[i];
        s->storage[PL330_CIDR_BASE + i] = cidr[i];
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &rk3588_pl330_ops, s,
                          "rk3588-pl330", RK3588_PL330_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rk3588_pl330 = {
    .name = "rk3588-pl330",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(storage, RK3588PL330State, RK3588_PL330_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_pl330_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rk3588_pl330_realize;
    dc->vmsd = &vmstate_rk3588_pl330;
    dc->user_creatable = false;
}

static const TypeInfo rk3588_pl330_info = {
    .name = TYPE_RK3588_PL330,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588PL330State),
    .class_init = rk3588_pl330_class_init,
};

static void rk3588_pl330_register_types(void)
{
    type_register_static(&rk3588_pl330_info);
}

type_init(rk3588_pl330_register_types)

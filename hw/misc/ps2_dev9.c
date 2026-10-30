/*
 * Sony PlayStation 2 DEV9 expansion bay and SPEED interface
 *
 * The DEV9 bridge of the IOP powers and resets the device in the
 * expansion bay.  The device here is an expansion device (not a PC card)
 * built around the SPEED chip with an ATA interface for a hard disk, as on
 * the network adaptor.  SPEED reports its capabilities in its revision
 * registers; the ATA task file registers at SPEED offset 0x40 belong to
 * an IDE channel modelled apart.  Other SPEED registers only hold the
 * values written to them, as the transfer timings need no emulation.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/ps2_dev9.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

/* An expansion device; 0x20 would be a PC card. */
#define PS2_DEV9_REV_EXPANSION  0x30

/* The SPEED revision, and the ATA interface as its only capability. */
#define PS2_SPEED_REVISION      0x11
#define PS2_SPEED_CAPS_ATA      0x02

static uint64_t ps2_dev9_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2DEV9State *s = opaque;
    uint16_t value;

    switch (addr) {
    case PS2_DEV9_REV:
        value = PS2_DEV9_REV_EXPANSION;
        break;
    case PS2_DEV9_1462:
        /* Bit 0 clear: a device is in the bay. */
        value = 0;
        break;
    default:
        value = s->dev9[addr / 2];
        break;
    }
    trace_ps2_dev9_read(addr, value);
    return value;
}

static void ps2_dev9_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned int size)
{
    PS2DEV9State *s = opaque;

    trace_ps2_dev9_write(addr, value);
    s->dev9[addr / 2] = value;
}

static const MemoryRegionOps ps2_dev9_ops = {
    .read = ps2_dev9_read,
    .write = ps2_dev9_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t ps2_speed_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2DEV9State *s = opaque;
    uint16_t value;

    if (addr >= PS2_SPEED_REGS_SIZE) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }

    switch (addr) {
    case PS2_SPEED_REV_1:
        value = PS2_SPEED_REVISION;
        break;
    case PS2_SPEED_REV_3:
        value = PS2_SPEED_CAPS_ATA;
        break;
    case PS2_SPEED_REV_8:
        value = 0;
        break;
    default:
        value = s->speed[addr / 2];
        break;
    }
    trace_ps2_speed_read(addr, value);
    return value;
}

static void ps2_speed_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned int size)
{
    PS2DEV9State *s = opaque;

    if (addr >= PS2_SPEED_REGS_SIZE) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return;
    }
    trace_ps2_speed_write(addr, value);
    s->speed[addr / 2] = value;
}

static const MemoryRegionOps ps2_speed_ops = {
    .read = ps2_speed_read,
    .write = ps2_speed_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ps2_dev9_reset_hold(Object *obj, ResetType type)
{
    PS2DEV9State *s = PS2_DEV9(obj);

    memset(s->dev9, 0, sizeof(s->dev9));
    memset(s->speed, 0, sizeof(s->speed));
}

static void ps2_dev9_init(Object *obj)
{
    PS2DEV9State *s = PS2_DEV9(obj);

    memory_region_init_io(&s->dev9_iomem, obj, &ps2_dev9_ops, s,
                          TYPE_PS2_DEV9, PS2_DEV9_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->dev9_iomem);
    memory_region_init_io(&s->speed_iomem, obj, &ps2_speed_ops, s,
                          "ps2-speed", PS2_SPEED_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->speed_iomem);
}

static const VMStateDescription vmstate_ps2_dev9 = {
    .name = TYPE_PS2_DEV9,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(dev9, PS2DEV9State, PS2_DEV9_SIZE / 2),
        VMSTATE_UINT16_ARRAY(speed, PS2DEV9State, PS2_SPEED_REGS_SIZE / 2),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_dev9_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_dev9_reset_hold;
    dc->vmsd = &vmstate_ps2_dev9;
}

static const TypeInfo ps2_dev9_types[] = {
    {
        .name = TYPE_PS2_DEV9,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2DEV9State),
        .instance_init = ps2_dev9_init,
        .class_init = ps2_dev9_class_init,
    },
};

DEFINE_TYPES(ps2_dev9_types);

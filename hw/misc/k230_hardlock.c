/*
 * K230 hardlock registers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_hardlock.h"

#define K230_HARDLOCK_BASE 0xa0

static uint64_t k230_hardlock_read_bytes(uint8_t *regs, hwaddr addr,
                                         unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_hardlock_write_bytes(uint8_t *regs, hwaddr addr,
                                      uint64_t val, unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static bool k230_hardlock_index(hwaddr addr, unsigned int size,
                                unsigned int *index)
{
    hwaddr offset;

    if (size != 4 || addr < K230_HARDLOCK_BASE) {
        return false;
    }

    offset = addr - K230_HARDLOCK_BASE;
    if (offset >= K230_HARDLOCK_COUNT * 4 || (offset & 3)) {
        return false;
    }

    *index = offset / 4;
    return true;
}

static uint64_t k230_hardlock_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    K230HardlockState *s = K230_HARDLOCK(opaque);
    unsigned int index;

    if (k230_hardlock_index(addr, size, &index)) {
        if (!s->locks[index]) {
            s->locks[index] = true;
            stl_le_p(s->regs + addr, 1);
            return 0;
        }
        return 1;
    }

    return k230_hardlock_read_bytes(s->regs, addr, size);
}

static void k230_hardlock_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned int size)
{
    K230HardlockState *s = K230_HARDLOCK(opaque);
    unsigned int index;

    k230_hardlock_write_bytes(s->regs, addr, val, size);

    if (k230_hardlock_index(addr, size, &index)) {
        s->locks[index] = val != 0;
    }
}

static const MemoryRegionOps k230_hardlock_ops = {
    .read = k230_hardlock_read,
    .write = k230_hardlock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_hardlock_reset(DeviceState *dev)
{
    K230HardlockState *s = K230_HARDLOCK(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->locks, 0, sizeof(s->locks));
}

static const VMStateDescription vmstate_k230_hardlock = {
    .name = TYPE_K230_HARDLOCK,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230HardlockState, K230_HARDLOCK_SIZE),
        VMSTATE_BOOL_ARRAY(locks, K230HardlockState, K230_HARDLOCK_COUNT),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_hardlock_realize(DeviceState *dev, Error **errp)
{
    K230HardlockState *s = K230_HARDLOCK(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_hardlock_ops, s,
                          TYPE_K230_HARDLOCK, K230_HARDLOCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_hardlock_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_hardlock_realize;
    device_class_set_legacy_reset(dc, k230_hardlock_reset);
    dc->vmsd = &vmstate_k230_hardlock;
    dc->desc = "K230 hardlock registers";
}

static const TypeInfo k230_hardlock_type_info = {
    .name = TYPE_K230_HARDLOCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230HardlockState),
    .class_init = k230_hardlock_class_init,
};

static void k230_register_hardlock_types(void)
{
    type_register_static(&k230_hardlock_type_info);
}

type_init(k230_register_hardlock_types)

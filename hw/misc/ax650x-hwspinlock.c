/*
 * AXERA AX650X hardware spinlock
 *
 * The AXERA Linux driver exposes 32 locks.  Each lock has an acquire register
 * at lock_id * 8 and an unlock register at lock_id * 8 + 4.  Reading an
 * unlocked acquire register claims the lock for CPU master ID 0 and returns
 * that ID (zero); a later read reports the lock busy.  The vendor driver
 * converts the zero owner ID into a successful hwspinlock trylock result.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/ax650x-hwspinlock.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define AX650X_HWSPINLOCK_STRIDE 8

static uint64_t ax650x_hwspinlock_read(void *opaque, hwaddr offset,
                                       unsigned int size)
{
    AX650XHWSpinlockState *s = AX650X_HWSPINLOCK(opaque);
    unsigned int lock = offset / AX650X_HWSPINLOCK_STRIDE;

    if (lock >= AX650X_HWSPINLOCK_COUNT ||
        offset % AX650X_HWSPINLOCK_STRIDE != 0) {
        return 0;
    }

    if (s->locked & BIT(lock)) {
        return 1;
    }

    s->locked |= BIT(lock);
    return 0;
}

static void ax650x_hwspinlock_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned int size)
{
    AX650XHWSpinlockState *s = AX650X_HWSPINLOCK(opaque);
    unsigned int lock = offset / AX650X_HWSPINLOCK_STRIDE;

    if (lock < AX650X_HWSPINLOCK_COUNT &&
        offset % AX650X_HWSPINLOCK_STRIDE == sizeof(uint32_t)) {
        s->locked &= ~BIT(lock);
    }
}

static const MemoryRegionOps ax650x_hwspinlock_ops = {
    .read = ax650x_hwspinlock_read,
    .write = ax650x_hwspinlock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void ax650x_hwspinlock_reset(DeviceState *dev)
{
    AX650XHWSpinlockState *s = AX650X_HWSPINLOCK(dev);

    s->locked = 0;
}

static void ax650x_hwspinlock_init(Object *obj)
{
    AX650XHWSpinlockState *s = AX650X_HWSPINLOCK(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ax650x_hwspinlock_ops, s,
                          TYPE_AX650X_HWSPINLOCK,
                          AX650X_HWSPINLOCK_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_ax650x_hwspinlock = {
    .name = TYPE_AX650X_HWSPINLOCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(locked, AX650XHWSpinlockState),
        VMSTATE_END_OF_LIST(),
    },
};

static void ax650x_hwspinlock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "AX650X hardware spinlock";
    device_class_set_legacy_reset(dc, ax650x_hwspinlock_reset);
    dc->vmsd = &vmstate_ax650x_hwspinlock;
}

static const TypeInfo ax650x_hwspinlock_types[] = {
    {
        .name = TYPE_AX650X_HWSPINLOCK,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AX650XHWSpinlockState),
        .instance_init = ax650x_hwspinlock_init,
        .class_init = ax650x_hwspinlock_class_init,
    },
};
DEFINE_TYPES(ax650x_hwspinlock_types)

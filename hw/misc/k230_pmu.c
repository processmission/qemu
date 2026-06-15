/*
 * K230 power management unit
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/k230_pmu.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "system/runstate.h"

#define K230_PMU_STATUS         0x3c
#define K230_PMU_INT_CLEAR      0x54
#define K230_PMU_OUTPUT_REG_CTL 0x78

#define K230_PMU_SOC_NORMAL_PD  2
#define K230_PMU_OUTPUT_ENABLE  1
#define K230_PMU_INT_CLR_ALL    0x3ff

enum {
    K230_PMU_POWEROFF_IDLE,
    K230_PMU_POWEROFF_STATUS,
    K230_PMU_POWEROFF_OUTPUT_ENABLED,
    K230_PMU_POWEROFF_INT_CLEARED,
};

static uint64_t k230_pmu_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230PmuState *s = K230_PMU(opaque);
    uint64_t val = 0;

    if (addr >= K230_PMU_MMIO_SIZE || size > K230_PMU_MMIO_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)s->regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_pmu_restart_poweroff(K230PmuState *s, hwaddr addr,
                                      uint64_t val)
{
    if (addr == K230_PMU_STATUS && val == K230_PMU_SOC_NORMAL_PD) {
        s->poweroff_step = K230_PMU_POWEROFF_STATUS;
    } else {
        s->poweroff_step = K230_PMU_POWEROFF_IDLE;
    }
}

static void k230_pmu_update_poweroff(K230PmuState *s, hwaddr addr,
                                     uint64_t val, unsigned int size)
{
    if (size != 4) {
        return;
    }

    switch (s->poweroff_step) {
    case K230_PMU_POWEROFF_IDLE:
        k230_pmu_restart_poweroff(s, addr, val);
        break;
    case K230_PMU_POWEROFF_STATUS:
        if (addr == K230_PMU_OUTPUT_REG_CTL &&
            val == K230_PMU_OUTPUT_ENABLE) {
            s->poweroff_step = K230_PMU_POWEROFF_OUTPUT_ENABLED;
        } else {
            k230_pmu_restart_poweroff(s, addr, val);
        }
        break;
    case K230_PMU_POWEROFF_OUTPUT_ENABLED:
        if (addr == K230_PMU_INT_CLEAR && val == K230_PMU_INT_CLR_ALL) {
            s->poweroff_step = K230_PMU_POWEROFF_INT_CLEARED;
        } else {
            k230_pmu_restart_poweroff(s, addr, val);
        }
        break;
    case K230_PMU_POWEROFF_INT_CLEARED:
        if (addr == K230_PMU_OUTPUT_REG_CTL && val == 0) {
            s->poweroff_step = K230_PMU_POWEROFF_IDLE;
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        } else {
            k230_pmu_restart_poweroff(s, addr, val);
        }
        break;
    default:
        k230_pmu_restart_poweroff(s, addr, val);
        break;
    }
}

static void k230_pmu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    K230PmuState *s = K230_PMU(opaque);

    if (addr >= K230_PMU_MMIO_SIZE || size > K230_PMU_MMIO_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        s->regs[addr + i] = val >> (i * 8);
    }

    k230_pmu_update_poweroff(s, addr, val, size);
}

static const MemoryRegionOps k230_pmu_ops = {
    .read = k230_pmu_read,
    .write = k230_pmu_write,
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

static void k230_pmu_reset(DeviceState *dev)
{
    K230PmuState *s = K230_PMU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->poweroff_step = K230_PMU_POWEROFF_IDLE;
}

static const VMStateDescription vmstate_k230_pmu = {
    .name = TYPE_K230_PMU,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230PmuState, K230_PMU_MMIO_SIZE),
        VMSTATE_UINT8(poweroff_step, K230PmuState),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_pmu_realize(DeviceState *dev, Error **errp)
{
    K230PmuState *s = K230_PMU(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_pmu_ops, s,
                          TYPE_K230_PMU, K230_PMU_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_pmu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_pmu_realize;
    device_class_set_legacy_reset(dc, k230_pmu_reset);
    dc->vmsd = &vmstate_k230_pmu;
    dc->desc = "K230 power management unit";
}

static const TypeInfo k230_pmu_type_info = {
    .name = TYPE_K230_PMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230PmuState),
    .class_init = k230_pmu_class_init,
};

static void k230_pmu_register_types(void)
{
    type_register_static(&k230_pmu_type_info);
}

type_init(k230_pmu_register_types)

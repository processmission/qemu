/*
 * K230 hardlock registers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_hardlock.h"

#define K230_IPCM_CPU2DSP_INT_EN0       0x0000
#define K230_IPCM_CPU2DSP_INT_SET0      0x0004
#define K230_IPCM_CPU2DSP_INT_CLEAR0    0x0008
#define K230_IPCM_CPU2DSP_INT_STATUS0   0x000c
#define K230_IPCM_CPU2DSP_INT_ERR0      0x0010
#define K230_IPCM_DSP2CPU_INT_EN0       0x0014
#define K230_IPCM_DSP2CPU_INT_SET0      0x0018
#define K230_IPCM_DSP2CPU_INT_CLEAR0    0x001c
#define K230_IPCM_DSP2CPU_INT_STATUS0   0x0020
#define K230_IPCM_DSP2CPU_INT_ERR0      0x0024

#define K230_IPCM_INT_ENABLE            BIT(0)
#define K230_HARDLOCK_BASE 0xa0

static uint32_t k230_hardlock_readl(K230HardlockState *s, hwaddr addr)
{
    return ldl_le_p(s->regs + addr);
}

static void k230_hardlock_writel(K230HardlockState *s, hwaddr addr,
                                 uint32_t val)
{
    stl_le_p(s->regs + addr, val);
}

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

static uint32_t k230_ipcm_channel_mask(uint64_t val)
{
    if (val < 32) {
        return BIT(val);
    }

    return val;
}

static void k230_ipcm_update_dsp2cpu_irq(K230HardlockState *s)
{
    uint32_t enable = k230_hardlock_readl(s, K230_IPCM_DSP2CPU_INT_EN0);
    uint32_t status = k230_hardlock_readl(s, K230_IPCM_DSP2CPU_INT_STATUS0);
    bool irq_enable = enable & K230_IPCM_INT_ENABLE;

    for (int i = 0; i < K230_HARDLOCK_IPCM_IRQ_COUNT; i++) {
        qemu_set_irq(s->irqs[i], irq_enable && (status & BIT(i)));
    }
}

static bool k230_ipcm_write(K230HardlockState *s, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    uint32_t mask;
    uint32_t status;

    if (size != 4) {
        return false;
    }

    switch (addr) {
    case K230_IPCM_CPU2DSP_INT_EN0:
    case K230_IPCM_CPU2DSP_INT_ERR0:
    case K230_IPCM_DSP2CPU_INT_ERR0:
        k230_hardlock_writel(s, addr, val);
        return true;

    case K230_IPCM_DSP2CPU_INT_EN0:
        k230_hardlock_writel(s, addr, val);
        k230_ipcm_update_dsp2cpu_irq(s);
        return true;

    case K230_IPCM_CPU2DSP_INT_SET0:
        mask = k230_ipcm_channel_mask(val);
        status = k230_hardlock_readl(s, K230_IPCM_CPU2DSP_INT_STATUS0);
        k230_hardlock_writel(s, K230_IPCM_CPU2DSP_INT_SET0, val);
        k230_hardlock_writel(s, K230_IPCM_CPU2DSP_INT_STATUS0, status | mask);
        return true;

    case K230_IPCM_CPU2DSP_INT_CLEAR0:
        mask = k230_ipcm_channel_mask(val);
        status = k230_hardlock_readl(s, K230_IPCM_CPU2DSP_INT_STATUS0);
        k230_hardlock_writel(s, K230_IPCM_CPU2DSP_INT_CLEAR0, val);
        k230_hardlock_writel(s, K230_IPCM_CPU2DSP_INT_STATUS0,
                             status & ~mask);
        return true;

    case K230_IPCM_DSP2CPU_INT_SET0:
        mask = k230_ipcm_channel_mask(val);
        status = k230_hardlock_readl(s, K230_IPCM_DSP2CPU_INT_STATUS0);
        k230_hardlock_writel(s, K230_IPCM_DSP2CPU_INT_SET0, val);
        k230_hardlock_writel(s, K230_IPCM_DSP2CPU_INT_STATUS0, status | mask);
        k230_ipcm_update_dsp2cpu_irq(s);
        return true;

    case K230_IPCM_DSP2CPU_INT_CLEAR0:
        mask = k230_ipcm_channel_mask(val);
        status = k230_hardlock_readl(s, K230_IPCM_DSP2CPU_INT_STATUS0);
        k230_hardlock_writel(s, K230_IPCM_DSP2CPU_INT_CLEAR0, val);
        k230_hardlock_writel(s, K230_IPCM_DSP2CPU_INT_STATUS0,
                             status & ~mask);
        k230_ipcm_update_dsp2cpu_irq(s);
        return true;

    default:
        return false;
    }
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

    if (k230_ipcm_write(s, addr, val, size)) {
        return;
    }

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
    for (int i = 0; i < K230_HARDLOCK_IPCM_IRQ_COUNT; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irqs[i]);
    }
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

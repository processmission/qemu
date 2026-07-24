/*
 * Minimal RK3588 CRU (Clock-and-Reset-Unit) stub.
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Backs the 0xfd7c0000/0x5c000 CRU window so the Linux
 * "rockchip,rk3588-cru" provider (CLK_OF_DECLARE -> clk-rk3588 +
 * rst-rk3588) registers and resolves the in-scope drivers'
 * reset_control_get / clk_prepare_enable calls. The six RKNPU controls
 * drive modeled per-core reset outputs.
 *
 * Offset 0x600 (RK3588_GRF_SOC_STATUS0, PLL lock status) MUST read
 * 0xffffffff or the early PLL init in clk-rk3588 hangs before the
 * console comes up. SPL also polls per-PLL status registers at +0x18
 * for bit 15. Other registers are RAM-backed and honor the Rockchip
 * HIWORD-mask update convention.
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3588_cru.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define RK3588_CRU_PLL_STATUS_OFFSET 0x18
#define RK3588_CRU_PLL_STATUS_STRIDE 0x20
#define RK3588_CRU_PLL_LOCKED 0x8000
#define RK3588_CRU_SOFTRST_CON(n) (0xa00 + (n) * 4)

typedef struct RK3588CRURKNNReset {
    hwaddr offset;
    uint32_t mask;
} RK3588CRURKNNReset;

static const RK3588CRURKNNReset rk3588_cru_rknpu_resets[] = {
    { RK3588_CRU_SOFTRST_CON(30), BIT(6) | BIT(8) },
    { RK3588_CRU_SOFTRST_CON(27), BIT(0) | BIT(2) },
    { RK3588_CRU_SOFTRST_CON(28), BIT(0) | BIT(2) },
};

static void rk3588_cru_update_rknpu_reset(RK3588CRUState *s,
                                           hwaddr offset)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(rk3588_cru_rknpu_resets); i++) {
        const RK3588CRURKNNReset *reset = &rk3588_cru_rknpu_resets[i];

        if (offset == reset->offset) {
            qemu_set_irq(s->rknpu_reset[i],
                         !!(s->regs[offset >> 2] & reset->mask));
            return;
        }
    }
}

static uint64_t rk3588_cru_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3588CRUState *s = opaque;
    uint32_t value;

    if (offset + size > RK3588_CRU_SIZE || (offset & 3) || size != 4) {
        return 0;
    }

    if (offset == RK3588_CRU_PLL_STATUS) {
        return 0xffffffff;
    }

    value = s->regs[offset >> 2];
    if ((offset & (RK3588_CRU_PLL_STATUS_STRIDE - 1)) ==
        RK3588_CRU_PLL_STATUS_OFFSET) {
        value |= RK3588_CRU_PLL_LOCKED;
    }

    return value;
}

static void rk3588_cru_write(void *opaque, hwaddr offset, uint64_t val,
                             unsigned size)
{
    RK3588CRUState *s = opaque;
    uint32_t old, mask, value = val;

    if (offset + size > RK3588_CRU_SIZE || (offset & 3) || size != 4) {
        return;
    }

    old = s->regs[offset >> 2];
    mask = value >> 16;
    if (mask) {
        s->regs[offset >> 2] = (old & ~mask) | (value & mask);
    } else {
        s->regs[offset >> 2] = value;
    }
    rk3588_cru_update_rknpu_reset(s, offset);
}

static const MemoryRegionOps rk3588_cru_ops = {
    .read = rk3588_cru_read,
    .write = rk3588_cru_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rk3588_cru_reset(DeviceState *dev)
{
    RK3588CRUState *s = RK3588_CRU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (unsigned int i = 0; i < ARRAY_SIZE(s->rknpu_reset); i++) {
        qemu_set_irq(s->rknpu_reset[i], 0);
    }
}

static void rk3588_cru_init(Object *obj)
{
    RK3588CRUState *s = RK3588_CRU(obj);

    qdev_init_gpio_out_named(DEVICE(obj), s->rknpu_reset,
                             "rknpu-reset",
                             ARRAY_SIZE(s->rknpu_reset));
}

static void rk3588_cru_realize(DeviceState *dev, Error **errp)
{
    RK3588CRUState *s = RK3588_CRU(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &rk3588_cru_ops, s,
                          "rk3588-cru", RK3588_CRU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void rk3588_cru_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rk3588_cru_realize;
    device_class_set_legacy_reset(dc, rk3588_cru_reset);
    /* Not user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo rk3588_cru_info = {
    .name = TYPE_RK3588_CRU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588CRUState),
    .instance_init = rk3588_cru_init,
    .class_init = rk3588_cru_class_init,
};

static void rk3588_cru_register_types(void)
{
    type_register_static(&rk3588_cru_info);
}

type_init(rk3588_cru_register_types)

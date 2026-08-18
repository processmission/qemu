/*
 * RK3588 TSADC (thermal sensor ADC) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register-level model for the rockchip-thermal driver.  The driver
 * configures auto-sampling and alarm registers, then polls
 * TSADCV3_DATA(chn) for the ADC code of each sensor channel.  The model
 * returns a fixed room-temperature code so the Linux thermal framework
 * sees a benign temperature instead of taking the critical-trip
 * shutdown path (unmapped reads previously produced an out-of-range
 * code that the driver rejected with -EAGAIN).
 *
 * Register offsets follow drivers/thermal/rockchip_thermal.h (TSADCV3
 * layout, RK3588 uses the v4 data mask):
 *
 *   USER_CON 0x00  AUTO_CON 0x04  INT_EN 0x08  AUTO_SRC_CON 0x0c
 *   HT_INT_EN 0x14  HSHUT_GPIO_INT_EN 0x18  HSHUT_CRU_INT_EN 0x1c
 *   INT_PD 0x24  HSHUT_PD 0x28  DATA(chn) 0x2c + 4*chn
 *   COMP_INT(chn) 0x6c + 4*chn  COMP_SHUT(chn) 0x10c + 4*chn
 *   AUTO_PERIOD 0x154  AUTO_PERIOD_HT 0x158
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/misc/rk3588_tsadc.h"
#include "migration/vmstate.h"

#define TSADC_USER_CON 0x00
#define TSADC_AUTO_CON 0x04
#define TSADC_INT_EN 0x08
#define TSADC_AUTO_SRC_CON 0x0c
#define TSADC_HT_INT_EN 0x14
#define TSADC_HSHUT_GPIO_INT_EN 0x18
#define TSADC_HSHUT_CRU_INT_EN 0x1c
#define TSADC_INT_PD 0x24
#define TSADC_HSHUT_PD 0x28
#define TSADC_DATA(chn) (0x2c + (chn) * 0x04)
#define TSADC_COMP_INT(chn) (0x6c + (chn) * 0x04)
#define TSADC_COMP_SHUT(chn) (0x10c + (chn) * 0x04)
#define TSADC_AUTO_PERIOD 0x154
#define TSADC_AUTO_PERIOD_HT 0x158
#define TSADC_DATA_MASK 0x1ff

static uint64_t rk3588_tsadc_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3588TSADCState *s = opaque;

    if (offset >= TSADC_DATA(0) &&
        offset < TSADC_DATA(0) + RK3588_TSADC_NUM_CHANNELS * 4) {
        /* Fixed room-temperature code for every channel. */
        return RK3588_TSADC_ROOM_CODE & TSADC_DATA_MASK;
    }

    switch (offset) {
    case TSADC_INT_PD:
    case TSADC_HSHUT_PD:
        /* No alarms pending at room temperature. */
        return 0;
    case TSADC_USER_CON:
    case TSADC_AUTO_CON:
    case TSADC_INT_EN:
    case TSADC_AUTO_SRC_CON:
    case TSADC_HT_INT_EN:
    case TSADC_HSHUT_GPIO_INT_EN:
    case TSADC_HSHUT_CRU_INT_EN:
    case TSADC_AUTO_PERIOD:
    case TSADC_AUTO_PERIOD_HT:
        return s->regs[offset >> 2];
    default:
        if (offset >= TSADC_COMP_INT(0) &&
            offset < TSADC_COMP_INT(0) + RK3588_TSADC_NUM_CHANNELS * 4) {
            return s->regs[offset >> 2];
        }
        if (offset >= TSADC_COMP_SHUT(0) &&
            offset < TSADC_COMP_SHUT(0) + RK3588_TSADC_NUM_CHANNELS * 4) {
            return s->regs[offset >> 2];
        }
        return 0;
    }
}

static void rk3588_tsadc_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RK3588TSADCState *s = opaque;

    if (offset < RK3588_TSADC_MMIO_SIZE - 4) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps rk3588_tsadc_ops = {
    .read = rk3588_tsadc_read,
    .write = rk3588_tsadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rk3588_tsadc_realize(DeviceState *dev, Error **errp)
{
    RK3588TSADCState *s = RK3588_TSADC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_init_irq(sbd, &s->irq);
    memory_region_init_io(&s->iomem, OBJECT(dev), &rk3588_tsadc_ops, s,
                          "rk3588-tsadc", RK3588_TSADC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rk3588_tsadc_reset(DeviceState *dev)
{
    RK3588TSADCState *s = RK3588_TSADC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static const VMStateDescription vmstate_rk3588_tsadc = {
    .name = "rk3588-tsadc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3588TSADCState, RK3588_TSADC_MMIO_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_tsadc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rk3588_tsadc_realize;
    device_class_set_legacy_reset(dc, rk3588_tsadc_reset);
    dc->vmsd = &vmstate_rk3588_tsadc;
    dc->user_creatable = false;
}

static const TypeInfo rk3588_tsadc_info = {
    .name = TYPE_RK3588_TSADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588TSADCState),
    .class_init = rk3588_tsadc_class_init,
};

static void rk3588_tsadc_register_types(void)
{
    type_register_static(&rk3588_tsadc_info);
}

type_init(rk3588_tsadc_register_types)

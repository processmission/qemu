/*
 * Rockchip DWCMSHC vendor register window
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Synopsys DWCMSHC places Rockchip eMMC control, auto-tuning and DLL
 * registers above the 0x100-byte SDHCI core register map. U-Boot and
 * Linux poll DLL_STATUS0 for LOCKED and expect TIMEOUT clear.
 */

#include "qemu/osdep.h"
#include "hw/sd/rockchip_dwcmshc.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define DWCMSHC_EMMC_DLL_STATUS0 0x840
#define DWCMSHC_EMMC_DLL_LOCKED  BIT(8)

static uint64_t rockchip_dwcmshc_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    RockchipDWCMSHCVendorState *s = opaque;
    hwaddr reg = offset + ROCKCHIP_DWCMSHC_VENDOR_BASE;

    if (offset + size > ROCKCHIP_DWCMSHC_VENDOR_SIZE) {
        return 0;
    }

    if (reg == DWCMSHC_EMMC_DLL_STATUS0 && size == 4) {
        return ldl_le_p(&s->regs[offset]) | DWCMSHC_EMMC_DLL_LOCKED;
    }

    switch (size) {
    case 1:
        return s->regs[offset];
    case 2:
        return lduw_le_p(&s->regs[offset]);
    case 4:
        return ldl_le_p(&s->regs[offset]);
    default:
        return 0;
    }
}

static void rockchip_dwcmshc_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    RockchipDWCMSHCVendorState *s = opaque;

    if (offset + size > ROCKCHIP_DWCMSHC_VENDOR_SIZE) {
        return;
    }

    switch (size) {
    case 1:
        s->regs[offset] = value;
        break;
    case 2:
        stw_le_p(&s->regs[offset], value);
        break;
    case 4:
        stl_le_p(&s->regs[offset], value);
        break;
    }
}

static const MemoryRegionOps rockchip_dwcmshc_ops = {
    .read = rockchip_dwcmshc_read,
    .write = rockchip_dwcmshc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void rockchip_dwcmshc_reset(DeviceState *dev)
{
    RockchipDWCMSHCVendorState *s = ROCKCHIP_DWCMSHC_VENDOR(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rockchip_dwcmshc_realize(DeviceState *dev, Error **errp)
{
    RockchipDWCMSHCVendorState *s = ROCKCHIP_DWCMSHC_VENDOR(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &rockchip_dwcmshc_ops, s,
                          "rockchip-dwcmshc-vendor",
                          ROCKCHIP_DWCMSHC_VENDOR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void rockchip_dwcmshc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rockchip_dwcmshc_realize;
    device_class_set_legacy_reset(dc, rockchip_dwcmshc_reset);
    dc->user_creatable = false;
}

static const TypeInfo rockchip_dwcmshc_info = {
    .name = TYPE_ROCKCHIP_DWCMSHC_VENDOR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipDWCMSHCVendorState),
    .class_init = rockchip_dwcmshc_class_init,
};

static void rockchip_dwcmshc_register_types(void)
{
    type_register_static(&rockchip_dwcmshc_info);
}

type_init(rockchip_dwcmshc_register_types)

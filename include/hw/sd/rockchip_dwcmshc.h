/*
 * Rockchip DWCMSHC vendor register window
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_ROCKCHIP_DWCMSHC_H
#define HW_SD_ROCKCHIP_DWCMSHC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_DWCMSHC_VENDOR "rockchip-dwcmshc-vendor"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipDWCMSHCVendorState,
                           ROCKCHIP_DWCMSHC_VENDOR)

#define ROCKCHIP_DWCMSHC_VENDOR_BASE 0x100
#define ROCKCHIP_DWCMSHC_VENDOR_SIZE 0xff00

struct RockchipDWCMSHCVendorState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t regs[ROCKCHIP_DWCMSHC_VENDOR_SIZE];
};

#endif /* HW_SD_ROCKCHIP_DWCMSHC_H */

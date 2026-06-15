/*
 * K230 DWC MSHC SDHCI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_K230_SDHCI_H
#define HW_SD_K230_SDHCI_H

#include "hw/core/sysbus.h"
#include "hw/sd/sdhci.h"
#include "qom/object.h"

#define TYPE_K230_SDHCI "k230-sdhci"
OBJECT_DECLARE_SIMPLE_TYPE(K230SdhciState, K230_SDHCI)

#define K230_SDHCI_COUNT 2
#define K230_SDHCI_VENDOR_SIZE 0xf00

struct K230SdhciState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    SDHCIState sdhci;
    MemoryRegion container;
    MemoryRegion vendor_iomem;
    uint8_t vendor_regs[K230_SDHCI_VENDOR_SIZE];
    BusState *sd_bus;
};

#endif /* HW_SD_K230_SDHCI_H */

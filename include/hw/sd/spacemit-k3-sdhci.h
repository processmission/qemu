/*
 * SpacemiT K3 SDHCI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_SPACEMIT_K3_SDHCI_H
#define HW_SD_SPACEMIT_K3_SDHCI_H

#include "hw/core/sysbus.h"
#include "hw/sd/sdhci.h"
#include "qom/object.h"

#define TYPE_SPACEMIT_K3_SDHCI "spacemit.k3.sdhci"
OBJECT_DECLARE_SIMPLE_TYPE(SpacemitK3SDHCIState, SPACEMIT_K3_SDHCI)

#define SPACEMIT_K3_SDHCI_MMIO_SIZE 0x200
#define SPACEMIT_K3_SDHCI_CAPAREG   UINT64_C(0x112834b4)

struct SpacemitK3SDHCIState {
    SysBusDevice parent_obj;

    SDHCIState sdhci;
    MemoryRegion container;
    MemoryRegion vendor_iomem;
    uint32_t mmc_ctrl;
    uint32_t tx_cfg;
    BusState *sd_bus;
};

#endif /* HW_SD_SPACEMIT_K3_SDHCI_H */

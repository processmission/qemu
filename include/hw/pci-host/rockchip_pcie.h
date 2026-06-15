/*
 * Rockchip DesignWare PCIe host wrapper.
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wraps TYPE_DESIGNWARE_PCIE_HOST and adds the Rockchip APB vendor window the
 * Linux dw-rockchip driver expects. The board remains responsible for mapping
 * the SoC-specific DBI/APB/CFG windows and wiring the IRQs:
 *
 *   - DBI  @ SoC-specific base - DWC core cfg/Port-Logic/iATU/MSI,
 *                                already implemented by designware.c
 *                                (via the inherited 4 KiB sysbus mmio
 *                                plus an optional board cover over the
 *                                rest of the larger DBI window).
 *   - APB  @ SoC-specific base - Rockchip PCIE_CLIENT_* regs. Only
 *                                  PCIE_CLIENT_LTSSM_STATUS (0x300) is
 *                                  load-bearing: pinned to 0x00030011
 *                                  so rockchip_pcie_link_up() returns
 *                                  true on the first read (substitution
 *                                  policy - no analog link in QEMU).
 *                                  All other APB offsets are RAZ/WI.
 *   - CFG  @ SoC-specific base - ECAM window into the designware
 *                                outbound CFG0 viewport alias.
 *
 * IRQs are exposed as five logical outputs: err / legacy / msg / pmc / sys.
 * The board wires them to SoC-specific interrupt lines; only legacy (INTx)
 * and msg (MSI) are functionally load-bearing for enumeration.
 */

#ifndef HW_PCI_HOST_ROCKCHIP_PCIE_H
#define HW_PCI_HOST_ROCKCHIP_PCIE_H

#include "hw/pci-host/designware.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_PCIE_HOST "rockchip-pcie-host"
OBJECT_DECLARE_TYPE(RockchipPCIEHost, RockchipPCIEHostClass,
                    ROCKCHIP_PCIE_HOST)

/*
 * SysBus IRQ indices exported by this device (in declaration order).
 * The legacy+msg IRQs come from the parent designware host
 * (s->pci.irqs[0..3] -> INTA..INTD, s->pci.msi -> MSI). The RK wrapper
 * re-exports them plus three inert RK-only IRQs (err/pmc/sys).
 */
#define ROCKCHIP_PCIE_ERR_IRQ    0
#define ROCKCHIP_PCIE_LEGACY_IRQ 1   /* fans out to INTA..INTD */
#define ROCKCHIP_PCIE_MSG_IRQ    5   /* MSI parent */
#define ROCKCHIP_PCIE_PMC_IRQ    6
#define ROCKCHIP_PCIE_SYS_IRQ    7
#define ROCKCHIP_PCIE_NUM_IRQS   8

/* APB vendor register offsets (PCIE_CLIENT_*). */
#define ROCKCHIP_PCIE_APB_LTSSM_STATUS 0x300

struct RockchipPCIEHost {
    DesignwarePCIEHost parent_obj;

    /* RK APB vendor register window (overlaps PCIE_CLIENT_* regs). */
    MemoryRegion apb;

    /*
     * Five RK-side IRQs wired to the GIC. legacy fans out into the
     * four INTA..INTD lines inherited from designware; msg is the MSI
     * parent. err/pmc/sys are inert in the model but kept for FDT
     * fidelity.
     */
    qemu_irq err_irq;
    qemu_irq pmc_irq;
    qemu_irq sys_irq;
};

struct RockchipPCIEHostClass {
    PCIHostBridgeClass parent_class;

    DeviceRealize parent_realize;
};

#endif /* HW_PCI_HOST_ROCKCHIP_PCIE_H */

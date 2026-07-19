/*
 * Rockchip DesignWare PCIe host wrapper.
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Subclasses TYPE_DESIGNWARE_PCIE_HOST and adds:
 *
 *   - Rockchip APB vendor register window (sysbus mmio[1]).
 *     Only PCIE_CLIENT_LTSSM_STATUS (0x300) is load-bearing: it reports
 *     either L0/link-up or detect/link-down according to the link-up
 *     property. Every other
 *     APB offset is RAZ/WI so the driver's PCIE_CLIENT_* writes
 *     (GENERAL_CON mode set, HOT_RESET_CTRL LTSSM enhance, POWER_CON
 *     clkreq, INTR_MASK_LEGACY) land without aborting (D-15).
 *
 *   - The inherited 4 KiB DBI mmio (sysbus mmio[0]) covers the DWC
 *     core Type-0 header / Port-Logic / iATU viewport / MSI block that
 *     designware.c implements. A device-owned RAZ/WI region covers the
 *     rest of the 4 MiB DBI window, including DBI2 at +0x100000.
 *
 *   - The board's CFG window is served by the designware root's outbound
 *     CFG viewports; the guest programs them in dw_pcie_config_ecam_iatu
 *     and designware.c maps viewport->cfg at the programmed base - no
 *     static alias is needed here.
 *
 * IRQ wiring: the parent designware host already exports 5 sysbus IRQs
 * (0..3 = INTA..INTD, 4 = MSI). This subclass adds three more sysbus
 * IRQs (5 = err, 6 = pmc, 7 = sys) for Rockchip-specific inert lines. The
 * board maps 0..3 -> GIC SPI 260 (legacy), 4 -> 261 (msg/MSI),
 * 5 -> 259 (err), 6 -> 262 (pmc), 7 -> 263 (sys).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci-host/rockchip_pcie.h"

/*
 * PCIE_CLIENT_LTSSM_STATUS (APB 0x300). Driver reads bits 17:16 for
 * link-up (0b11 = up) and bits 5:0 for LTSSM state (L0 = 0x11). Pin
 * both when the link-up property is enabled.
 */
#define ROCKCHIP_PCIE_LTSSM_LINK_UP   0x00030011u

static uint64_t rockchip_pcie_apb_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    RockchipPCIEHost *s = opaque;

    switch (offset) {
    case ROCKCHIP_PCIE_APB_LTSSM_STATUS:
        return s->link_up ? ROCKCHIP_PCIE_LTSSM_LINK_UP : 0;
    default:
        /*
         * All other PCIE_CLIENT_* regs are write-only from the driver's
         * perspective; reads return 0 (reset value). The driver does
         * not read them at probe, so any value is fine - 0 is the
         * documented reset value for the load-bearing intr/mask regs.
         */
        return 0;
    }
}

static const Property rockchip_pcie_host_properties[] = {
    DEFINE_PROP_BOOL("link-up", RockchipPCIEHost, link_up, true),
    DEFINE_PROP_UINT32("domain", RockchipPCIEHost, domain, 0),
};

static void rockchip_pcie_apb_write(void *opaque, hwaddr offset, uint64_t val,
                                    unsigned size)
{
    /*
     * Discard silently. The RK driver uses HIWORD-MASK writes to set
     * RC mode, LTSSM enable, LTSSM enhance, clkreq, and INTx masks;
     * none of these have behavioral effect in the substitution model
     * but the writes MUST land (D-15: AArch64 aborts unassigned
     * writes).
     */
}

static const MemoryRegionOps rockchip_pcie_apb_ops = {
    .read = rockchip_pcie_apb_read,
    .write = rockchip_pcie_apb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static uint64_t rockchip_pcie_dbi_tail_read(void *opaque, hwaddr offset,
                                             unsigned size)
{
    return 0;
}

static void rockchip_pcie_dbi_tail_write(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size)
{
}

static const MemoryRegionOps rockchip_pcie_dbi_tail_ops = {
    .read = rockchip_pcie_dbi_tail_read,
    .write = rockchip_pcie_dbi_tail_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void rockchip_pcie_host_realize(DeviceState *dev, Error **errp)
{
    RockchipPCIEHost *s = ROCKCHIP_PCIE_HOST(dev);
    RockchipPCIEHostClass *rkpc = ROCKCHIP_PCIE_HOST_GET_CLASS(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    snprintf(s->root_bus_path, sizeof(s->root_bus_path), "%04x:%02x",
             s->domain, s->parent_obj.bus_nr);

    /*
     * Realize the parent designware host first. This registers its
     * 4 KiB DBI mmio at sysbus mmio[0] and its 5 sysbus IRQs
     * (0..3 = INTA..INTD, 4 = MSI).
     */
    rkpc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    /*
     * Three Rockchip-specific inert IRQs (err/pmc/sys). legacy (INTA..INTD) and
     * msg (MSI) come from the parent. The board wires all five logical
     * IRQs to the GIC.
     */
    sysbus_init_irq(sbd, &s->err_irq);    /* index 5 */
    sysbus_init_irq(sbd, &s->pmc_irq);    /* index 6 */
    sysbus_init_irq(sbd, &s->sys_irq);    /* index 7 */

    /*
     * RK APB vendor register window (sysbus mmio[1]). LTSSM pinned,
     * everything else RAZ/WI.
     */
    memory_region_init_io(&s->apb, OBJECT(s), &rockchip_pcie_apb_ops, s,
                          "rockchip-pcie-apb", 0x10000);
    sysbus_init_mmio(sbd, &s->apb);

    memory_region_init_io(&s->dbi_tail, OBJECT(s),
                          &rockchip_pcie_dbi_tail_ops, s,
                          "rockchip-pcie-dbi-tail",
                          ROCKCHIP_PCIE_DBI_TAIL_SIZE);
    sysbus_init_mmio(sbd, &s->dbi_tail);
}

static const char *rockchip_pcie_host_root_bus_path(
    PCIHostState *host_bridge, PCIBus *rootbus)
{
    RockchipPCIEHost *s = ROCKCHIP_PCIE_HOST(host_bridge);

    return s->root_bus_path;
}

static void rockchip_pcie_host_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);
    RockchipPCIEHostClass *rkpc = ROCKCHIP_PCIE_HOST_CLASS(klass);

    device_class_set_parent_realize(dc, rockchip_pcie_host_realize,
                                    &rkpc->parent_realize);
    device_class_set_props(dc, rockchip_pcie_host_properties);
    hc->root_bus_path = rockchip_pcie_host_root_bus_path;
    /* Not user-creatable; instantiated by the board. */
    dc->user_creatable = false;
}

static const TypeInfo rockchip_pcie_host_info = {
    .name = TYPE_ROCKCHIP_PCIE_HOST,
    .parent = TYPE_DESIGNWARE_PCIE_HOST,
    .instance_size = sizeof(RockchipPCIEHost),
    .class_init = rockchip_pcie_host_class_init,
};

static void rockchip_pcie_register_types(void)
{
    type_register_static(&rockchip_pcie_host_info);
}

type_init(rockchip_pcie_register_types)

/*
 * SpacemiT K3 SDHCI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/spacemit-k3-sdhci.h"
#include "migration/vmstate.h"

#define K3_SDHCI_STD_SIZE              0x100
#define K3_SDHCI_VENDOR_SIZE           0x100

/* Offsets below are relative to the vendor bank at controller offset 0x100. */
#define K3_SDHCI_MMC_CTRL              0x14
#define K3_SDHCI_TX_CFG                0x1c
#define K3_SDHCI_MMC_CTRL_MASK         (BIT(8) | BIT(9) | BIT(10) | BIT(12))
#define K3_SDHCI_TX_CFG_MASK           (BIT(30) | BIT(31))

static uint64_t spacemit_k3_sdhci_vendor_read(void *opaque, hwaddr addr,
                                               unsigned int size)
{
    SpacemitK3SDHCIState *s = SPACEMIT_K3_SDHCI(opaque);

    switch (addr) {
    case K3_SDHCI_MMC_CTRL:
        return s->mmc_ctrl;
    case K3_SDHCI_TX_CFG:
        return s->tx_cfg;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx
                      "\n", __func__, K3_SDHCI_STD_SIZE + addr);
        return 0;
    }
}

static void spacemit_k3_sdhci_vendor_write(void *opaque, hwaddr addr,
                                            uint64_t value,
                                            unsigned int size)
{
    SpacemitK3SDHCIState *s = SPACEMIT_K3_SDHCI(opaque);

    switch (addr) {
    case K3_SDHCI_MMC_CTRL:
        s->mmc_ctrl = value & K3_SDHCI_MMC_CTRL_MASK;
        break;
    case K3_SDHCI_TX_CFG:
        s->tx_cfg = value & K3_SDHCI_TX_CFG_MASK;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx
                      "\n", __func__, K3_SDHCI_STD_SIZE + addr);
        break;
    }
}

static const MemoryRegionOps spacemit_k3_sdhci_vendor_ops = {
    .read = spacemit_k3_sdhci_vendor_read,
    .write = spacemit_k3_sdhci_vendor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void spacemit_k3_sdhci_instance_init(Object *obj)
{
    SpacemitK3SDHCIState *s = SPACEMIT_K3_SDHCI(obj);

    object_initialize_child(obj, "generic-sdhci", &s->sdhci,
                            TYPE_SYSBUS_SDHCI);
    qdev_prop_set_uint8(DEVICE(&s->sdhci), "sd-spec-version", 3);
    /*
     * The core uses this property to retain v3 Host Control 2 writes.  The
     * capability register still advertises neither 1.8 V nor UHS modes.
     */
    qdev_prop_set_uint8(DEVICE(&s->sdhci), "uhs", UHS_I);
    qdev_prop_set_uint64(DEVICE(&s->sdhci), "capareg",
                         SPACEMIT_K3_SDHCI_CAPAREG);
}

static void spacemit_k3_sdhci_reset(DeviceState *dev)
{
    SpacemitK3SDHCIState *s = SPACEMIT_K3_SDHCI(dev);

    s->mmc_ctrl = 0;
    s->tx_cfg = 0;
    device_cold_reset(DEVICE(&s->sdhci));
}

static void spacemit_k3_sdhci_realize(DeviceState *dev, Error **errp)
{
    SpacemitK3SDHCIState *s = SPACEMIT_K3_SDHCI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SysBusDevice *sdhci_sbd = SYS_BUS_DEVICE(&s->sdhci);

    memory_region_init(&s->container, OBJECT(s),
                       "spacemit.k3.sdhci-container",
                       SPACEMIT_K3_SDHCI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->container);

    memory_region_init_io(&s->vendor_iomem, OBJECT(s),
                          &spacemit_k3_sdhci_vendor_ops, s,
                          "spacemit.k3.sdhci-vendor", K3_SDHCI_VENDOR_SIZE);
    memory_region_add_subregion(&s->container, K3_SDHCI_STD_SIZE,
                                &s->vendor_iomem);

    if (!sysbus_realize(sdhci_sbd, errp)) {
        return;
    }
    memory_region_add_subregion(&s->container, 0,
                                sysbus_mmio_get_region(sdhci_sbd, 0));

    sysbus_pass_irq(sbd, sdhci_sbd);
    s->sd_bus = qdev_get_child_bus(DEVICE(sdhci_sbd), "sd-bus");
}

static const VMStateDescription vmstate_spacemit_k3_sdhci = {
    .name = TYPE_SPACEMIT_K3_SDHCI,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mmc_ctrl, SpacemitK3SDHCIState),
        VMSTATE_UINT32(tx_cfg, SpacemitK3SDHCIState),
        VMSTATE_END_OF_LIST(),
    },
};

static void spacemit_k3_sdhci_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = spacemit_k3_sdhci_realize;
    device_class_set_legacy_reset(dc, spacemit_k3_sdhci_reset);
    dc->vmsd = &vmstate_spacemit_k3_sdhci;
    dc->desc = "SpacemiT K3 SDHCI controller";
}

static const TypeInfo spacemit_k3_sdhci_type_info = {
    .name = TYPE_SPACEMIT_K3_SDHCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SpacemitK3SDHCIState),
    .instance_init = spacemit_k3_sdhci_instance_init,
    .class_init = spacemit_k3_sdhci_class_init,
};

static void spacemit_k3_sdhci_register_types(void)
{
    type_register_static(&spacemit_k3_sdhci_type_info);
}

type_init(spacemit_k3_sdhci_register_types)

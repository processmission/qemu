/*
 * K230 DWC MSHC SDHCI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/sd/k230_sdhci.h"

#define K230_SDHCI_MMIO_SIZE      0x1000
#define K230_SDHCI_STD_SIZE       0x100
#define K230_SDHCI_CAPAREG        0x057c34b4

#define K230_DWC_MSHC_PHY_CNFG    0x300
#define K230_DWC_MSHC_PHY_PWRGOOD BIT(1)

static uint64_t k230_sdhci_read_bytes(uint8_t *regs, hwaddr addr,
                                      unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_sdhci_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                   unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static void k230_sdhci_set_phy_power_good(K230SdhciState *s)
{
    hwaddr phy_cnfg = K230_DWC_MSHC_PHY_CNFG - K230_SDHCI_STD_SIZE;

    s->vendor_regs[phy_cnfg] |= K230_DWC_MSHC_PHY_PWRGOOD;
}

static uint64_t k230_sdhci_vendor_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    return k230_sdhci_read_bytes(K230_SDHCI(opaque)->vendor_regs, addr, size);
}

static void k230_sdhci_vendor_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned int size)
{
    K230SdhciState *s = K230_SDHCI(opaque);

    k230_sdhci_write_bytes(s->vendor_regs, addr, val, size);

    if (addr <= K230_DWC_MSHC_PHY_CNFG - K230_SDHCI_STD_SIZE &&
        addr + size > K230_DWC_MSHC_PHY_CNFG - K230_SDHCI_STD_SIZE) {
        k230_sdhci_set_phy_power_good(s);
    }
}

static const MemoryRegionOps k230_sdhci_vendor_ops = {
    .read = k230_sdhci_vendor_read,
    .write = k230_sdhci_vendor_write,
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

static void k230_sdhci_instance_init(Object *obj)
{
    K230SdhciState *s = K230_SDHCI(obj);

    object_initialize_child(obj, "generic-sdhci", &s->sdhci,
                            TYPE_SYSBUS_SDHCI);
    qdev_prop_set_uint8(DEVICE(&s->sdhci), "sd-spec-version", 3);
    qdev_prop_set_uint64(DEVICE(&s->sdhci), "capareg", K230_SDHCI_CAPAREG);
}

static void k230_sdhci_reset(DeviceState *dev)
{
    K230SdhciState *s = K230_SDHCI(dev);

    memset(s->vendor_regs, 0, sizeof(s->vendor_regs));
    k230_sdhci_set_phy_power_good(s);
    device_cold_reset(DEVICE(&s->sdhci));
}

static void k230_sdhci_realize(DeviceState *dev, Error **errp)
{
    K230SdhciState *s = K230_SDHCI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SysBusDevice *sdhci_sbd = SYS_BUS_DEVICE(&s->sdhci);

    memory_region_init(&s->container, OBJECT(s), "k230.sdhci-container",
                       K230_SDHCI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->container);

    memory_region_init_io(&s->vendor_iomem, OBJECT(s),
                          &k230_sdhci_vendor_ops, s, "k230.sdhci-vendor",
                          K230_SDHCI_VENDOR_SIZE);
    memory_region_add_subregion(&s->container, K230_SDHCI_STD_SIZE,
                                &s->vendor_iomem);

    if (!sysbus_realize(sdhci_sbd, errp)) {
        return;
    }
    memory_region_add_subregion(&s->container, 0,
                                sysbus_mmio_get_region(sdhci_sbd, 0));

    sysbus_pass_irq(sbd, sdhci_sbd);
    s->sd_bus = qdev_get_child_bus(DEVICE(sdhci_sbd), "sd-bus");
}

static const VMStateDescription vmstate_k230_sdhci = {
    .name = TYPE_K230_SDHCI,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(vendor_regs, K230SdhciState,
                            K230_SDHCI_VENDOR_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_sdhci_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_sdhci_realize;
    device_class_set_legacy_reset(dc, k230_sdhci_reset);
    dc->vmsd = &vmstate_k230_sdhci;
    dc->desc = "K230 DWC MSHC SDHCI controller";
}

static const TypeInfo k230_sdhci_type_info = {
    .name = TYPE_K230_SDHCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SdhciState),
    .instance_init = k230_sdhci_instance_init,
    .class_init = k230_sdhci_class_init,
};

static void k230_register_sdhci_types(void)
{
    type_register_static(&k230_sdhci_type_info);
}

type_init(k230_register_sdhci_types)

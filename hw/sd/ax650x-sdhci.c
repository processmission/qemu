/*
 * Axera AX650X SDHCI controller
 *
 * Copyright (c) 2026 Zevorn
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/ax650x-sdhci.h"
#include "migration/vmstate.h"
#include "sdhci-internal.h"

#define AX650X_VENDOR_ADDR(_addr) ((_addr) - AX650X_SDHCI_VENDOR_BASE)

#define VREG(_name, _addr) \
    { .name = (_name), .addr = AX650X_VENDOR_ADDR(_addr) }
#define VREG_RO(_name, _addr, _reset) \
    { .name = (_name), .addr = AX650X_VENDOR_ADDR(_addr), \
      .reset = (_reset), .ro = UINT8_MAX }

static uint64_t ax650x_register_bank_read(void *opaque, hwaddr addr,
                                          unsigned int size)
{
    uint64_t value = 0;

    for (unsigned int i = 0; i < size; i++) {
        value |= register_read_memory(opaque, addr + i, 1) << (i * 8);
    }

    return value;
}

static void ax650x_register_bank_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned int size)
{
    for (unsigned int i = 0; i < size; i++) {
        register_write_memory(opaque, addr + i, value >> (i * 8), 1);
    }
}

static const MemoryRegionOps ax650x_register_bank_ops = {
    .read = ax650x_register_bank_read,
    .write = ax650x_register_bank_write,
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

static void ax650x_dll_ctrl_post_write(RegisterInfo *reg, uint64_t value)
{
    AX650XSDHCIState *s = AX650X_SDHCI(reg->opaque);
    unsigned int status = AX650X_VENDOR_ADDR(AX650X_SDHCI_PHY_DLL_STATUS);

    if (value & AX650X_SDHCI_DLL_EN) {
        s->vendor_regs[status] = AX650X_SDHCI_DLL_LOCKED;
    } else {
        s->vendor_regs[status] = 0;
    }
}

static void ax650x_emmc_ctrl_post_write(RegisterInfo *reg, uint64_t value)
{
    AX650XSDHCIState *s = AX650X_SDHCI(reg->opaque);
    BusChild *child;
    bool reset_asserted;

    reset_asserted = (value & AX650X_SDHCI_EMMC_RST_N_OE) &&
                     !(value & AX650X_SDHCI_EMMC_RST_N);
    if (s->emmc_reset_asserted && !reset_asserted) {
        child = QTAILQ_FIRST(&s->sdhci.sdbus.qbus.children);
        if (child) {
            device_cold_reset(child->child);
        }
    }
    s->emmc_reset_asserted = reset_asserted;
}

static const RegisterAccessInfo ax650x_pointer_regs_info[] = {
    { .name = "P_VENDOR_SPECIFIC_AREA[7:0]", .addr = 0,
      .reset = AX650X_SDHCI_VENDOR_PTR_VALUE & 0xff, .ro = UINT8_MAX },
    { .name = "P_VENDOR_SPECIFIC_AREA[15:8]", .addr = 1,
      .reset = AX650X_SDHCI_VENDOR_PTR_VALUE >> 8, .ro = UINT8_MAX },
};

static const RegisterAccessInfo ax650x_vendor_regs_info[] = {
    { .name = "PHY_CNFG[7:0]",
      .addr = AX650X_VENDOR_ADDR(AX650X_SDHCI_PHY_CNFG),
      .reset = AX650X_SDHCI_PHY_PWRGOOD,
      .ro = AX650X_SDHCI_PHY_PWRGOOD },
    VREG("PHY_CNFG[15:8]", AX650X_SDHCI_PHY_CNFG + 1),
    VREG("PHY_CNFG[23:16]", AX650X_SDHCI_PHY_CNFG + 2),
    VREG("PHY_CNFG[31:24]", AX650X_SDHCI_PHY_CNFG + 3),
    VREG("PHY_CMDPAD_CNFG[7:0]", AX650X_SDHCI_PHY_CMDPAD_CNFG),
    VREG("PHY_CMDPAD_CNFG[15:8]", AX650X_SDHCI_PHY_CMDPAD_CNFG + 1),
    VREG("PHY_DATAPAD_CNFG[7:0]", AX650X_SDHCI_PHY_DATAPAD_CNFG),
    VREG("PHY_DATAPAD_CNFG[15:8]", AX650X_SDHCI_PHY_DATAPAD_CNFG + 1),
    VREG("PHY_CLKPAD_CNFG[7:0]", AX650X_SDHCI_PHY_CLKPAD_CNFG),
    VREG("PHY_CLKPAD_CNFG[15:8]", AX650X_SDHCI_PHY_CLKPAD_CNFG + 1),
    VREG("PHY_STBPAD_CNFG[7:0]", AX650X_SDHCI_PHY_STBPAD_CNFG),
    VREG("PHY_STBPAD_CNFG[15:8]", AX650X_SDHCI_PHY_STBPAD_CNFG + 1),
    VREG("PHY_RSTNPAD_CNFG[7:0]", AX650X_SDHCI_PHY_RSTNPAD_CNFG),
    VREG("PHY_RSTNPAD_CNFG[15:8]", AX650X_SDHCI_PHY_RSTNPAD_CNFG + 1),
    VREG("PHY_COMMDL_CNFG", AX650X_SDHCI_PHY_COMMDL_CNFG),
    VREG("PHY_SDCLKDL_CNFG", AX650X_SDHCI_PHY_SDCLKDL_CNFG),
    VREG("PHY_SDCLKDL_DC", AX650X_SDHCI_PHY_SDCLKDL_DC),
    VREG("PHY_SMPLDL_CNFG", AX650X_SDHCI_PHY_SMPLDL_CNFG),
    VREG("PHY_ATDL_CNFG", AX650X_SDHCI_PHY_ATDL_CNFG),
    { .name = "PHY_DLL_CTRL",
      .addr = AX650X_VENDOR_ADDR(AX650X_SDHCI_PHY_DLL_CTRL),
      .rsvd = 0xf8,
      .post_write = ax650x_dll_ctrl_post_write },
    VREG("PHY_DLL_CNFG1", AX650X_SDHCI_PHY_DLL_CNFG1),
    VREG("PHY_DLL_CNFG2", AX650X_SDHCI_PHY_DLL_CNFG2),
    VREG("PHY_DLLDL_CNFG", AX650X_SDHCI_PHY_DLLDL_CNFG),
    VREG("PHY_DLL_OFFSET", AX650X_SDHCI_PHY_DLL_OFFSET),
    VREG("PHY_DLLLBT_CNFG[7:0]", AX650X_SDHCI_PHY_DLLLBT_CNFG),
    VREG("PHY_DLLLBT_CNFG[15:8]", AX650X_SDHCI_PHY_DLLLBT_CNFG + 1),
    VREG_RO("PHY_DLL_STATUS", AX650X_SDHCI_PHY_DLL_STATUS, 0),
    VREG_RO("PHY_DLLDBG_MLKDC", AX650X_SDHCI_PHY_DLLDBG_MLKDC, 98),
    VREG_RO("PHY_DLLDBG_SLKDC", AX650X_SDHCI_PHY_DLLDBG_SLKDC, 24),
    { .name = "EMMC_CTRL[7:0]",
      .addr = AX650X_VENDOR_ADDR(AX650X_SDHCI_EMMC_CTRL),
      .rsvd = 0xf2,
      .post_write = ax650x_emmc_ctrl_post_write },
    { .name = "EMMC_CTRL[15:8]",
      .addr = AX650X_VENDOR_ADDR(AX650X_SDHCI_EMMC_CTRL + 1),
      .rsvd = 0xfe },
};

static void ax650x_sdhci_reset(DeviceState *dev)
{
    AX650XSDHCIState *s = AX650X_SDHCI(dev);

    device_cold_reset(DEVICE(&s->sdhci));
    for (unsigned int i = 0;
         i < s->pointer_reg_array->num_elements; i++) {
        register_reset(s->pointer_reg_array->r[i]);
    }
    for (unsigned int i = 0;
         i < s->vendor_reg_array->num_elements; i++) {
        register_reset(s->vendor_reg_array->r[i]);
    }
    s->emmc_reset_asserted = false;
}

static void ax650x_sdhci_realize(DeviceState *dev, Error **errp)
{
    AX650XSDHCIState *s = AX650X_SDHCI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SysBusDevice *sdhci_sbd = SYS_BUS_DEVICE(&s->sdhci);

    qdev_prop_set_uint8(DEVICE(&s->sdhci), "sd-spec-version", 3);
    qdev_prop_set_uint8(DEVICE(&s->sdhci), "uhs", UHS_I);
    qdev_prop_set_uint64(DEVICE(&s->sdhci), "capareg",
                         SDHC_CAPAB_REG_DEFAULT |
                         R_SDHC_CAPAB_EMBEDDED_8BIT_MASK |
                         R_SDHC_CAPAB_BUS64BIT_MASK);
    if (!sysbus_realize(sdhci_sbd, errp)) {
        return;
    }

    memory_region_init(&s->container, OBJECT(s), "ax650x.sdhci-container",
                       AX650X_SDHCI_REG_SIZE);
    sysbus_init_mmio(sbd, &s->container);
    memory_region_add_subregion(&s->container, 0,
                                sysbus_mmio_get_region(sdhci_sbd, 0));
    memory_region_add_subregion_overlap(&s->container,
                                        AX650X_SDHCI_VENDOR_PTR,
                                        &s->pointer_reg_array->mem, 1);
    memory_region_add_subregion(&s->container, AX650X_SDHCI_VENDOR_BASE,
                                &s->vendor_reg_array->mem);
    sysbus_pass_irq(sbd, sdhci_sbd);
}

static const VMStateDescription vmstate_ax650x_sdhci = {
    .name = TYPE_AX650X_SDHCI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pointer_regs, AX650XSDHCIState, 2),
        VMSTATE_UINT8_ARRAY(vendor_regs, AX650XSDHCIState,
                            AX650X_SDHCI_VENDOR_SIZE),
        VMSTATE_BOOL(emmc_reset_asserted, AX650XSDHCIState),
        VMSTATE_END_OF_LIST(),
    },
};

static void ax650x_sdhci_init(Object *obj)
{
    AX650XSDHCIState *s = AX650X_SDHCI(obj);

    object_initialize_child(obj, "generic-sdhci", &s->sdhci,
                            TYPE_SYSBUS_SDHCI);
    s->pointer_reg_array =
        register_init_block8(DEVICE(obj), ax650x_pointer_regs_info,
                             ARRAY_SIZE(ax650x_pointer_regs_info),
                             s->pointer_regs_info, s->pointer_regs,
                             &ax650x_register_bank_ops, false,
                             sizeof(s->pointer_regs));
    s->vendor_reg_array =
        register_init_block8(DEVICE(obj), ax650x_vendor_regs_info,
                             ARRAY_SIZE(ax650x_vendor_regs_info),
                             s->vendor_regs_info, s->vendor_regs,
                             &ax650x_register_bank_ops, false,
                             sizeof(s->vendor_regs));
}

static void ax650x_sdhci_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Axera AX650X SD/eMMC Host Controller";
    dc->realize = ax650x_sdhci_realize;
    device_class_set_legacy_reset(dc, ax650x_sdhci_reset);
    dc->vmsd = &vmstate_ax650x_sdhci;
}

SDBus *ax650x_sdhci_get_bus(AX650XSDHCIState *s)
{
    return &s->sdhci.sdbus;
}

static const TypeInfo ax650x_sdhci_info = {
    .name = TYPE_AX650X_SDHCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AX650XSDHCIState),
    .instance_init = ax650x_sdhci_init,
    .class_init = ax650x_sdhci_class_init,
};

static void ax650x_sdhci_register_types(void)
{
    type_register_static(&ax650x_sdhci_info);
}

type_init(ax650x_sdhci_register_types)

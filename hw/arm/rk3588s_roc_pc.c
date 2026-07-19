/*
 * Firefly ROC-RK3588S-PC machine
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "rk3588-internal.h"

#define TYPE_RK3588S_ROC_PC_MACHINE MACHINE_TYPE_NAME("rk3588s-roc-pc")

static const char * const rk3588s_roc_pc_compatible[] = {
    "rockchip,rk3588s-firefly-roc-pc",
    "firefly,rk3588s-roc-pc",
    "rockchip,rk3588s",
    "rockchip,rk3588",
};

static const RK3588BoardConfig rk3588s_roc_pc_board = {
    .machine_name = "rk3588s-roc-pc",
    .desc = "Firefly ROC-RK3588S-PC",
    .ram_id = "rk3588s-roc-pc.ram",
    .fdt_model = "Firefly ROC-RK3588S-PC",
    .fdt_compatible = rk3588s_roc_pc_compatible,
    .fdt_compatible_count = ARRAY_SIZE(rk3588s_roc_pc_compatible),
    .firmware_sd_unit = 2,
    .brom_bootsource = RK3588_BROM_BOOTSOURCE_SD,
    .dram_type = RK3588_DRAM_TYPE_LPDDR4X,
    .gmac_mask = BIT(0) | BIT(1),
    .pcie3x4_num_lanes = 4,
    .swap_gmac_aliases = true,
    .default_zvm_ram = true,
    .rknpu_fdt_topology = RK3588_RKNPU_FDT_PER_CORE,
};

static void rk3588s_roc_pc_machine_instance_init(Object *obj)
{
    rk3588_machine_instance_configure(obj, &rk3588s_roc_pc_board);
}

static void rk3588s_roc_pc_machine_class_init(ObjectClass *oc,
                                              const void *data)
{
    rk3588_machine_class_configure(oc, &rk3588s_roc_pc_board);
}

static const TypeInfo rk3588s_roc_pc_machine_typeinfo = {
    .name = TYPE_RK3588S_ROC_PC_MACHINE,
    .parent = TYPE_RK3588_MACHINE,
    .class_init = rk3588s_roc_pc_machine_class_init,
    .instance_init = rk3588s_roc_pc_machine_instance_init,
};

static void rk3588s_roc_pc_machine_register_types(void)
{
    type_register_static(&rk3588s_roc_pc_machine_typeinfo);
}

type_init(rk3588s_roc_pc_machine_register_types)

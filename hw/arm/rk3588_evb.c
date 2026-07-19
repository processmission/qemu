/*
 * Rockchip RK3588 EVB machine
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "rk3588-internal.h"

#define TYPE_RK3588_EVB_MACHINE MACHINE_TYPE_NAME("rk3588-evb")

static const char * const rk3588_evb_compatible[] = {
    "qemu,rk3588-evb",
    "rockchip,rk3588-evb1-v10",
    "rockchip,rk3588",
};

static const RK3588BoardConfig rk3588_evb_board = {
    .machine_name = "rk3588-evb",
    .desc = "Rockchip RK3588 EVB (minimal)",
    .ram_id = "rk3588-evb.ram",
    .fdt_model = "QEMU Rockchip RK3588 EVB",
    .fdt_compatible = rk3588_evb_compatible,
    .fdt_compatible_count = ARRAY_SIZE(rk3588_evb_compatible),
    .firmware_sd_unit = 0,
    .brom_bootsource = RK3588_BROM_BOOTSOURCE_EMMC,
    .dram_type = RK3588_DRAM_TYPE_LPDDR4X,
    .gmac_mask = BIT(0) | BIT(1),
    .pcie3x4_num_lanes = 4,
    .default_zvm_ram = false,
    .rknpu_fdt_topology = RK3588_RKNPU_FDT_AGGREGATE,
};

static void rk3588_evb_machine_instance_init(Object *obj)
{
    rk3588_machine_instance_configure(obj, &rk3588_evb_board);
}

static void rk3588_evb_machine_class_init(ObjectClass *oc, const void *data)
{
    rk3588_machine_class_configure(oc, &rk3588_evb_board);
}

static const TypeInfo rk3588_evb_machine_typeinfo = {
    .name = TYPE_RK3588_EVB_MACHINE,
    .parent = TYPE_RK3588_MACHINE,
    .class_init = rk3588_evb_machine_class_init,
    .instance_init = rk3588_evb_machine_instance_init,
};

static void rk3588_evb_machine_register_types(void)
{
    type_register_static(&rk3588_evb_machine_typeinfo);
}

type_init(rk3588_evb_machine_register_types)

/*
 * Radxa ROCK 5B+ machine
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "rk3588-internal.h"

#define TYPE_ROCK_5B_PLUS_MACHINE MACHINE_TYPE_NAME("rock-5b-plus")

static const char * const rock_5b_plus_compatible[] = {
    "radxa,rock-5b-plus",
    "rockchip,rk3588",
};

static const RK3588FirmwareProfile rock_5b_plus_firmware = {
    .unfused_secure_otp = true,
    .crypto_v2_sha256 = true,
    .dynamic_fit_handoff = true,
    .atags_core = true,
    .fit_offset = 0x800000,
    .fit_alignment = 512,
};

static const RK3588BoardConfig rock_5b_plus_board = {
    .machine_name = "rock-5b-plus",
    .desc = "Radxa ROCK 5B+",
    .ram_id = "rock-5b-plus.ram",
    .fdt_model = "Radxa ROCK 5B+",
    .fdt_compatible = rock_5b_plus_compatible,
    .fdt_compatible_count = ARRAY_SIZE(rock_5b_plus_compatible),
    .firmware_sd_unit = 0,
    .brom_bootsource = RK3588_BROM_BOOTSOURCE_EMMC,
    .dram_type = RK3588_DRAM_TYPE_LPDDR5,
    .gmac_mask = 0,
    .pcie3x4_num_lanes = 2,
    .pcie3x2_num_lanes = 2,
    .pcie3x4_link_down = true,
    .pcie3x2_link_down = true,
    .swap_gmac_aliases = false,
    .default_zvm_ram = false,
    .rknpu_fdt_topology = RK3588_RKNPU_FDT_AGGREGATE,
    .firmware_profile = &rock_5b_plus_firmware,
};

static void rock_5b_plus_machine_instance_init(Object *obj)
{
    rk3588_machine_instance_configure(obj, &rock_5b_plus_board);
}

static void rock_5b_plus_machine_class_init(ObjectClass *oc,
                                             const void *data)
{
    rk3588_machine_class_configure(oc, &rock_5b_plus_board);
}

static const TypeInfo rock_5b_plus_machine_typeinfo = {
    .name = TYPE_ROCK_5B_PLUS_MACHINE,
    .parent = TYPE_RK3588_MACHINE,
    .class_init = rock_5b_plus_machine_class_init,
    .instance_init = rock_5b_plus_machine_instance_init,
};

static void rock_5b_plus_machine_register_types(void)
{
    type_register_static(&rock_5b_plus_machine_typeinfo);
}

type_init(rock_5b_plus_machine_register_types)

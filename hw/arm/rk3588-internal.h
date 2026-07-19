/*
 * Rockchip RK3588 machine extension interface
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RK3588_INTERNAL_H
#define HW_ARM_RK3588_INTERNAL_H

#include "hw/arm/machines-qom.h"
#include "qom/object.h"

#define TYPE_RK3588_MACHINE MACHINE_TYPE_NAME("rk3588")

#define RK3588_BROM_BOOTSOURCE_EMMC 2
#define RK3588_BROM_BOOTSOURCE_SD 5

#define RK3588_DRAM_TYPE_LPDDR4X 8
#define RK3588_DRAM_TYPE_LPDDR5 9

typedef struct RK3588FirmwareProfile {
    bool unfused_secure_otp;
    bool crypto_v2_sha256;
    bool dynamic_fit_handoff;
    bool atags_core;
    uint64_t fit_offset;
    uint32_t fit_alignment;
} RK3588FirmwareProfile;

typedef enum RK3588RknpuFdtTopology {
    RK3588_RKNPU_FDT_PER_CORE,
    RK3588_RKNPU_FDT_AGGREGATE,
} RK3588RknpuFdtTopology;

typedef struct RK3588BoardConfig {
    const char *machine_name;
    const char *desc;
    const char *ram_id;
    const char *fdt_model;
    const char * const *fdt_compatible;
    size_t fdt_compatible_count;
    unsigned int firmware_sd_unit;
    uint32_t brom_bootsource;
    uint32_t dram_type;
    uint32_t gmac_mask;
    unsigned int pcie3x4_num_lanes;
    unsigned int pcie3x2_num_lanes;
    bool pcie3x4_link_down;
    bool pcie3x2_link_down;
    bool swap_gmac_aliases;
    bool default_zvm_ram;
    RK3588RknpuFdtTopology rknpu_fdt_topology;
    const RK3588FirmwareProfile *firmware_profile;
} RK3588BoardConfig;

void rk3588_machine_instance_configure(Object *obj,
                                       const RK3588BoardConfig *board);
void rk3588_machine_class_configure(ObjectClass *oc,
                                    const RK3588BoardConfig *board);

#endif

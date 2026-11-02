/*
 * Rockchip RK3566 RKNPU
 *
 * Single-core RKNPU configuration on top of the shared Rockchip RKNPU core.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3566_rknpu.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define RK3566_RKNPU_PC_VERSION 0x46495245
#define RK3566_RKNPU_PC_VERSION_NUM 0x00000000

static const RockchipRKNNSocConfig rk3566_rknpu_soc_config = {
    .core_count = 1,
    .command_count_scale = 1,
    .dma_address_bits = 32,
    .pc_version = RK3566_RKNPU_PC_VERSION,
    .pc_version_num = RK3566_RKNPU_PC_VERSION_NUM,
    .has_amount_window = true,
    .amount_clr_all_offset = 0x8010,
    .amount_dt_wr_offset = 0x8034,
    .amount_dt_rd_offset = 0x8038,
    .amount_wt_rd_offset = 0x803c,
};

static DeviceRealize parent_realize;

static void rk3566_rknpu_realize(DeviceState *device, Error **errp)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(device);

    if (s->core_index != 0) {
        error_setg(errp, "RK3566 has one RKNPU core, core-index must be zero");
        return;
    }
    s->soc_config = &rk3566_rknpu_soc_config;
    parent_realize(device, errp);
}

static void rk3566_rknpu_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);

    device->desc = "RK3566 single-core RKNPU command processor";
    device_class_set_parent_realize(device, rk3566_rknpu_realize,
                                   &parent_realize);
}

static const TypeInfo rk3566_rknpu_type = {
    .name = TYPE_RK3566_RKNPU,
    .parent = TYPE_ROCKCHIP_RKNN_CORE,
    .class_init = rk3566_rknpu_class_init,
};

static void register_rk3566_rknpu_type(void)
{
    type_register_static(&rk3566_rknpu_type);
}

type_init(register_rk3566_rknpu_type)

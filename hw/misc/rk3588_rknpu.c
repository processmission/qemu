/*
 * Rockchip RK3588 RKNPU
 *
 * Three-core RKNPU configuration on top of the shared Rockchip RKNPU core.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3588_rknpu.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define RK3588_RKNPU_PC_VERSION 0x46495245
#define RK3588_RKNPU_PC_VERSION_NUM 0x00000000

static const RockchipRKNNSocConfig rk3588_rknpu_soc_config = {
    .core_count = 3,
    .command_count_scale = 2,
    .dma_address_bits = 40,
    .pc_version = RK3588_RKNPU_PC_VERSION,
    .pc_version_num = RK3588_RKNPU_PC_VERSION_NUM,
    .has_amount_window = false,
};

static DeviceRealize parent_realize;

static void rk3588_rknpu_realize(DeviceState *device, Error **errp)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(device);

    s->soc_config = &rk3588_rknpu_soc_config;
    parent_realize(device, errp);
}

static void rk3588_rknpu_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);

    device->desc = "RK3588 three-core RKNPU command processor";
    device_class_set_parent_realize(device, rk3588_rknpu_realize,
                                   &parent_realize);
}

static const TypeInfo rk3588_rknpu_type = {
    .name = TYPE_RK3588_RKNPU,
    .parent = TYPE_ROCKCHIP_RKNN_CORE,
    .class_init = rk3588_rknpu_class_init,
};

static void register_rk3588_rknpu_type(void)
{
    type_register_static(&rk3588_rknpu_type);
}

type_init(register_rk3588_rknpu_type)

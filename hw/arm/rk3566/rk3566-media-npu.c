/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RK3566 single-core RKNPU command processor configuration. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/misc/rk3588_rknpu.h"

#define TYPE_RK3566_NPU "rk3566-rknpu"

static DeviceRealize parent_realize;

static void rk3566_npu_realize(DeviceState *device, Error **errp)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(device);
    if (s->core_index != 0) {
        error_setg(errp, "RK3566 has one RKNPU core, core-index must be zero");
        return;
    }
    s->command_count_scale = 1;
    s->dma_address_bits = 32;
    parent_realize(device, errp);
}

static void rk3566_npu_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    device->desc = "RK3566 single-core RKNPU command processor";
    device_class_set_parent_realize(device, rk3566_npu_realize,
                                   &parent_realize);
}

static const TypeInfo rk3566_npu_type = {
    .name = TYPE_RK3566_NPU,
    .parent = TYPE_ROCKCHIP_RKNN_CORE,
    .class_init = rk3566_npu_class_init,
};

static void register_rk3566_npu_type(void)
{
    type_register_static(&rk3566_npu_type);
}

type_init(register_rk3566_npu_type)

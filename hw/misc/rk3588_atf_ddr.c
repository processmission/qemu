/*
 * Rockchip RK3588 ATF DDR runtime descriptor
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3588_atf_ddr.h"

#include "migration/vmstate.h"
#include "qemu/bswap.h"
#include "qemu/module.h"

#define RK3588_ATF_DDR_GLOBAL_PTR_ADDR 0x0008d0a8ULL
#define RK3588_ATF_TIMER_PTR_ADDR 0x0008d0b0ULL
#define RK3588_ATF_TIMER_TABLE_ADDR 0x0008d0b8ULL
#define RK3588_ATF_TIMER_COUNTER_ADDR 0x00062054ULL
#define RK3588_ATF_DDR_DESCRIPTOR_ADDR 0x0008fd20ULL
#define RK3588_ATF_DDR_CHANNEL_TABLE_ADDR 0x0008fe00ULL

#define RK3588_ATF_DDR_GLOBAL_PTR_OFFSET \
    (RK3588_ATF_DDR_GLOBAL_PTR_ADDR - RK3588_ATF_DDR_RUNTIME_BASE)
#define RK3588_ATF_TIMER_PTR_OFFSET \
    (RK3588_ATF_TIMER_PTR_ADDR - RK3588_ATF_DDR_RUNTIME_BASE)
#define RK3588_ATF_TIMER_TABLE_OFFSET \
    (RK3588_ATF_TIMER_TABLE_ADDR - RK3588_ATF_DDR_RUNTIME_BASE)
#define RK3588_ATF_DDR_DESCRIPTOR_OFFSET \
    (RK3588_ATF_DDR_DESCRIPTOR_ADDR - RK3588_ATF_DDR_RUNTIME_BASE)
#define RK3588_ATF_DDR_CHANNEL_TABLE_OFFSET \
    (RK3588_ATF_DDR_CHANNEL_TABLE_ADDR - RK3588_ATF_DDR_RUNTIME_BASE)

#define RK3588_ATF_DDR_GLOBAL_BASE 0xfd000000ULL
#define RK3588_ATF_DDR_CHANNEL_BASE 0xfd100000ULL
#define RK3588_ATF_DDR_CHANNEL_STRIDE 0x00020000ULL
#define RK3588_ATF_DDR_CHANNELS 8
#define RK3588_ATF_TIMER_US_HZ 1000000
#define RK3588_ATF_TIMER_HZ 24000000

struct RK3588ATFDDRState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint8_t runtime[RK3588_ATF_DDR_RUNTIME_SIZE];
};

static void rk3588_atf_ddr_seed(RK3588ATFDDRState *s)
{
    memset(s->runtime, 0, sizeof(s->runtime));

    /*
     * Rockchip's closed BL31 keeps a DDR controller runtime descriptor in its
     * SRAM BSS. SPL also uses this SRAM area, so provide the minimum stable
     * descriptor the BL31 DDR save/restore code expects before it programs
     * per-channel registers.
     */
    stq_le_p(&s->runtime[RK3588_ATF_DDR_GLOBAL_PTR_OFFSET],
             RK3588_ATF_DDR_DESCRIPTOR_ADDR);
    stq_le_p(&s->runtime[RK3588_ATF_TIMER_PTR_OFFSET],
             RK3588_ATF_TIMER_TABLE_ADDR);
    stq_le_p(&s->runtime[RK3588_ATF_TIMER_TABLE_OFFSET],
             RK3588_ATF_TIMER_COUNTER_ADDR);
    stl_le_p(&s->runtime[RK3588_ATF_TIMER_TABLE_OFFSET + 0x8],
             RK3588_ATF_TIMER_US_HZ);
    stl_le_p(&s->runtime[RK3588_ATF_TIMER_TABLE_OFFSET + 0xc],
             RK3588_ATF_TIMER_HZ);
    stq_le_p(&s->runtime[RK3588_ATF_DDR_DESCRIPTOR_OFFSET],
             RK3588_ATF_DDR_GLOBAL_BASE);
    stq_le_p(&s->runtime[RK3588_ATF_DDR_DESCRIPTOR_OFFSET + 0x20],
             RK3588_ATF_DDR_CHANNEL_TABLE_ADDR);

    for (unsigned int i = 0; i < RK3588_ATF_DDR_CHANNELS; i++) {
        stq_le_p(&s->runtime[
                 RK3588_ATF_DDR_CHANNEL_TABLE_OFFSET + i * sizeof(uint64_t)],
                 RK3588_ATF_DDR_CHANNEL_BASE +
                 i * RK3588_ATF_DDR_CHANNEL_STRIDE);
    }
}

static uint64_t rk3588_atf_ddr_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    RK3588ATFDDRState *s = opaque;

    if (size > sizeof(uint64_t) ||
        offset > RK3588_ATF_DDR_RUNTIME_SIZE - size) {
        return 0;
    }

    switch (size) {
    case 1:
        return s->runtime[offset];
    case 2:
        return lduw_le_p(&s->runtime[offset]);
    case 4:
        return ldl_le_p(&s->runtime[offset]);
    case 8:
        return ldq_le_p(&s->runtime[offset]);
    default:
        return 0;
    }
}

static void rk3588_atf_ddr_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    RK3588ATFDDRState *s = opaque;

    if (size > sizeof(uint64_t) ||
        offset > RK3588_ATF_DDR_RUNTIME_SIZE - size) {
        return;
    }

    switch (size) {
    case 1:
        s->runtime[offset] = value;
        break;
    case 2:
        stw_le_p(&s->runtime[offset], value);
        break;
    case 4:
        stl_le_p(&s->runtime[offset], value);
        break;
    case 8:
        stq_le_p(&s->runtime[offset], value);
        break;
    default:
        return;
    }

    rk3588_atf_ddr_seed(s);
}

static const MemoryRegionOps rk3588_atf_ddr_ops = {
    .read = rk3588_atf_ddr_read,
    .write = rk3588_atf_ddr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void rk3588_atf_ddr_reset_hold(Object *obj, ResetType type)
{
    RK3588ATFDDRState *s = RK3588_ATF_DDR(obj);

    rk3588_atf_ddr_seed(s);
}

static const VMStateDescription vmstate_rk3588_atf_ddr = {
    .name = TYPE_RK3588_ATF_DDR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(runtime, RK3588ATFDDRState,
                            RK3588_ATF_DDR_RUNTIME_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_atf_ddr_init(Object *obj)
{
    RK3588ATFDDRState *s = RK3588_ATF_DDR(obj);

    rk3588_atf_ddr_seed(s);
    memory_region_init_io(&s->mmio, obj, &rk3588_atf_ddr_ops, s,
                          TYPE_RK3588_ATF_DDR,
                          RK3588_ATF_DDR_RUNTIME_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void rk3588_atf_ddr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_rk3588_atf_ddr;
    rc->phases.hold = rk3588_atf_ddr_reset_hold;
}

static const TypeInfo rk3588_atf_ddr_info = {
    .name = TYPE_RK3588_ATF_DDR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588ATFDDRState),
    .instance_init = rk3588_atf_ddr_init,
    .class_init = rk3588_atf_ddr_class_init,
};

static void rk3588_atf_ddr_register_types(void)
{
    type_register_static(&rk3588_atf_ddr_info);
}

type_init(rk3588_atf_ddr_register_types)

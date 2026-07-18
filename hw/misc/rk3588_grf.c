/*
 * Rockchip RK3588 PMU General Register Files
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3588_grf.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/units.h"

#define RK3588_GRF_REG_WORDS \
    (RK3588_GRF_MMIO_SIZE / sizeof(uint32_t))

#define RK3588_DDR_SYS_REG_VERSION 3
#define RK3588_PMU0_WARM_BOOT_MAGIC 0x13579bdf
#define RK3588_SYS_CORE_STATUS 0xf0

REG32(PMU0_WARM_BOOT_MAGIC, 0x84)
REG32(PMU1_OS_REG2, 0x208)
REG32(PMU1_OS_REG3, 0x20c)
REG32(PMU1_OS_REG4, 0x210)
REG32(PMU1_OS_REG5, 0x214)
REG32(SYS_CORE_STATUS, 0x38c)

struct RK3588GRFState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array[RK3588_GRF_MMIO_COUNT];
    RegisterInfo regs_info[RK3588_GRF_MMIO_COUNT][RK3588_GRF_REG_WORDS];
    RegisterAccessInfo access_info[RK3588_GRF_MMIO_COUNT]
                                  [RK3588_GRF_REG_WORDS];
    uint32_t regs[RK3588_GRF_MMIO_COUNT][RK3588_GRF_REG_WORDS];
    uint64_t ram_size;
    uint32_t dram_type;
};

static uint64_t rk3588_grf_pre_write(RegisterInfo *reg, uint64_t value)
{
    uint32_t old = *(uint32_t *)reg->data;
    uint32_t mask = value >> 16;

    if (!mask) {
        return value;
    }

    return (old & ~mask) | (value & mask);
}

static const MemoryRegionOps rk3588_grf_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static uint32_t rk3588_grf_ddr_sys_reg2(uint64_t group_bytes,
                                        uint32_t dram_type)
{
    uint64_t group_mb = MAX(group_bytes / MiB, 256);
    unsigned int row = 13;
    uint32_t row_delta, reg;

    while ((256ULL << (row - 13)) < group_mb && row < 17) {
        row++;
    }

    row_delta = row - 13;
    reg = (dram_type & 7) << 13;
    reg |= 1 << 28;
    reg |= 1 << 9;
    reg |= (row_delta & 3) << 6;

    return reg;
}

static uint32_t rk3588_grf_ddr_sys_reg3(uint64_t group_bytes,
                                        uint32_t dram_type)
{
    uint64_t group_mb = MAX(group_bytes / MiB, 256);
    unsigned int row = 13;
    uint32_t row_delta, reg;

    while ((256ULL << (row - 13)) < group_mb && row < 17) {
        row++;
    }

    row_delta = row - 13;
    reg = RK3588_DDR_SYS_REG_VERSION << 28;
    reg |= (dram_type >> 3) << 12;
    reg |= ((row_delta >> 2) & 1) << 5;

    return reg;
}

static void rk3588_grf_reset_hold(Object *obj, ResetType type)
{
    RK3588GRFState *s = RK3588_GRF(obj);
    uint64_t group = MAX(s->ram_size / 2, 256 * MiB);
    uint32_t sys_reg2 = rk3588_grf_ddr_sys_reg2(group, s->dram_type);
    uint32_t sys_reg3 = rk3588_grf_ddr_sys_reg3(group, s->dram_type);

    s->access_info[RK3588_GRF_MMIO_PMU0]
                  [R_PMU0_WARM_BOOT_MAGIC].reset =
        RK3588_PMU0_WARM_BOOT_MAGIC;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG2].reset = sys_reg2;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG3].reset = sys_reg3;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG4].reset = sys_reg2;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG5].reset = sys_reg3;
    s->access_info[RK3588_GRF_MMIO_SYS][R_SYS_CORE_STATUS].reset =
        RK3588_SYS_CORE_STATUS;
    s->access_info[RK3588_GRF_MMIO_SYS][R_SYS_CORE_STATUS].ro = UINT32_MAX;

    for (unsigned int bank = 0; bank < RK3588_GRF_MMIO_COUNT; bank++) {
        for (unsigned int reg = 0; reg < RK3588_GRF_REG_WORDS; reg++) {
            register_reset(&s->regs_info[bank][reg]);
        }
    }
}

static const VMStateDescription vmstate_rk3588_grf = {
    .name = TYPE_RK3588_GRF,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(regs, RK3588GRFState,
                              RK3588_GRF_MMIO_COUNT,
                              RK3588_GRF_REG_WORDS),
        VMSTATE_END_OF_LIST(),
    },
};

static void rk3588_grf_init(Object *obj)
{
    RK3588GRFState *s = RK3588_GRF(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    static const char * const bank_names[] = {
        [RK3588_GRF_MMIO_PMU0] = "PMU0_GRF",
        [RK3588_GRF_MMIO_PMU1] = "PMU1_GRF",
        [RK3588_GRF_MMIO_SYS] = "SYS_GRF",
    };

    for (unsigned int bank = 0; bank < RK3588_GRF_MMIO_COUNT; bank++) {
        for (unsigned int reg = 0; reg < RK3588_GRF_REG_WORDS; reg++) {
            RegisterAccessInfo *access = &s->access_info[bank][reg];

            access->name = bank_names[bank];
            access->addr = reg * sizeof(uint32_t);
            access->pre_write = rk3588_grf_pre_write;
        }

        s->reg_array[bank] = register_init_block32(
            DEVICE(obj), s->access_info[bank], RK3588_GRF_REG_WORDS,
            s->regs_info[bank], s->regs[bank], &rk3588_grf_ops, false,
            RK3588_GRF_MMIO_SIZE);
        sysbus_init_mmio(sbd, &s->reg_array[bank]->mem);
    }
}

static const Property rk3588_grf_properties[] = {
    DEFINE_PROP_UINT64("ram-size", RK3588GRFState, ram_size, 0),
    DEFINE_PROP_UINT32("dram-type", RK3588GRFState, dram_type, 0),
};

static void rk3588_grf_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_rk3588_grf;
    device_class_set_props(dc, rk3588_grf_properties);
    rc->phases.hold = rk3588_grf_reset_hold;
}

static const TypeInfo rk3588_grf_info = {
    .name = TYPE_RK3588_GRF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588GRFState),
    .instance_init = rk3588_grf_init,
    .class_init = rk3588_grf_class_init,
};

static void rk3588_grf_register_types(void)
{
    type_register_static(&rk3588_grf_info);
}

type_init(rk3588_grf_register_types)

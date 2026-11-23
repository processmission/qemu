/*
 * Rockchip RK3588 PMU General Register Files
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/rk3588_grf.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/host-utils.h"
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

FIELD(PMU1_OS_REG2, CS0_ROW, 6, 2)
FIELD(PMU1_OS_REG2, CS0_COL, 9, 2)
FIELD(PMU1_OS_REG2, DDRTYPE, 13, 3)
FIELD(PMU1_OS_REG2, CHINFO, 28, 2)
FIELD(PMU1_OS_REG2, ROW_3_4, 30, 1)
FIELD(PMU1_OS_REG3, EXTEND_CS0_ROW, 5, 1)
FIELD(PMU1_OS_REG3, EXTEND_DDRTYPE, 12, 2)
FIELD(PMU1_OS_REG3, VERSION, 28, 4)

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

/* Writable low bits of HIWORD registers, from TRM sections 6.3--6.5. */
static const uint16_t rk3588_grf_hiword_masks[RK3588_GRF_MMIO_COUNT]
                                               [RK3588_GRF_REG_WORDS] = {
    [RK3588_GRF_MMIO_PMU0] = {
        [0x000 / 4] = 0xc761, /* SOC_CON0 */
        [0x004 / 4] = 0xffff, /* SOC_CON1 */
        [0x008 / 4] = 0x3fff, /* SOC_CON2 */
        [0x00c / 4] = 0x07ff, /* SOC_CON3 */
        [0x020 / 4] = 0x00ff, /* IO_RET_CON0 */
        [0x024 / 4] = 0x00ff, /* IO_RET_CON1 */
    },
    [RK3588_GRF_MMIO_PMU1] = {
        [0x000 / 4] = 0x0b7f, /* SOC_CON0 */
        [0x004 / 4] = 0xffff, /* SOC_CON1 */
        [0x008 / 4] = 0xffff, /* SOC_CON2 */
        [0x00c / 4] = 0x0011, /* SOC_CON3 */
        [0x010 / 4] = 0xf00f, /* SOC_CON4 */
        [0x014 / 4] = 0xf00f, /* SOC_CON5 */
        [0x018 / 4] = 0xffff, /* SOC_CON6 */
        [0x01c / 4] = 0xffff, /* SOC_CON7 */
        [0x020 / 4] = 0xffff, /* SOC_CON8 */
        [0x024 / 4] = 0x00ff, /* SOC_CON9 */
        [0x028 / 4] = 0xfff0, /* SOC_CON10 */
        [0x050 / 4] = 0x000c, /* BIU_CON */
        [0x080 / 4] = 0xffff, /* MEM_CON0 */
        [0x084 / 4] = 0xffff, /* MEM_CON1 */
        [0x088 / 4] = 0xffff, /* MEM_CON2 */
        [0x08c / 4] = 0x01ff, /* MEM_CON3 */
        [0x380 / 4] = 0x0003, /* SD_DETECT_CON */
    },
    [RK3588_GRF_MMIO_SYS] = {
        [0x000 / 4] = 0x0001, /* WDT_CON0 */
        [0x010 / 4] = 0x01ff, /* UART_CON0 */
        [0x014 / 4] = 0x01ff, /* UART_CON1 */
        [0x0c0 / 4] = 0xff02, /* GIC_CON0 */
        [0x200 / 4] = 0xffff, /* MEMCFG_CON0 */
        [0x204 / 4] = 0xffff, /* MEMCFG_CON1 */
        [0x208 / 4] = 0x0007, /* MEMCFG_CON2 */
        [0x20c / 4] = 0xffff, /* MEMCFG_CON3 */
        [0x210 / 4] = 0x00ff, /* MEMCFG_CON4 */
        [0x214 / 4] = 0xffff, /* MEMCFG_CON5 */
        [0x218 / 4] = 0xffff, /* MEMCFG_CON6 */
        [0x21c / 4] = 0x0007, /* MEMCFG_CON7 */
        [0x220 / 4] = 0xffff, /* MEMCFG_CON8 */
        [0x224 / 4] = 0x00ff, /* MEMCFG_CON9 */
        [0x228 / 4] = 0xffff, /* MEMCFG_CON10 */
        [0x22c / 4] = 0xffff, /* MEMCFG_CON11 */
        [0x230 / 4] = 0x0007, /* MEMCFG_CON12 */
        [0x234 / 4] = 0xffff, /* MEMCFG_CON13 */
        [0x238 / 4] = 0x00ff, /* MEMCFG_CON14 */
        [0x23c / 4] = 0x00ff, /* MEMCFG_CON15 */
        [0x240 / 4] = 0xffff, /* MEMCFG_CON16 */
        [0x244 / 4] = 0x07ff, /* MEMCFG_CON17 */
        [0x248 / 4] = 0xffff, /* MEMCFG_CON18 */
        [0x24c / 4] = 0xffff, /* MEMCFG_CON19 */
        [0x250 / 4] = 0x00ff, /* MEMCFG_CON20 */
        [0x254 / 4] = 0xffff, /* MEMCFG_CON21 */
        [0x258 / 4] = 0xffff, /* MEMCFG_CON22 */
        [0x25c / 4] = 0x00ff, /* MEMCFG_CON23 */
        [0x260 / 4] = 0xffff, /* MEMCFG_CON24 */
        [0x268 / 4] = 0xffff, /* MEMCFG_CON26 */
        [0x26c / 4] = 0x01ff, /* MEMCFG_CON27 */
        [0x270 / 4] = 0xffff, /* MEMCFG_CON28 */
        [0x274 / 4] = 0xffff, /* MEMCFG_CON29 */
        [0x278 / 4] = 0x00ff, /* MEMCFG_CON30 */
        [0x27c / 4] = 0xffff, /* MEMCFG_CON31 */
        [0x304 / 4] = 0xfc03, /* SOC_CON1 */
        [0x308 / 4] = 0xfff0, /* SOC_CON2 */
        [0x30c / 4] = 0xffff, /* SOC_CON3 */
        [0x318 / 4] = 0xffc7, /* SOC_CON6 */
        [0x31c / 4] = 0xffff, /* SOC_CON7 */
        [0x320 / 4] = 0xffff, /* SOC_CON8 */
        [0x324 / 4] = 0xffff, /* SOC_CON9 */
        [0x328 / 4] = 0xffff, /* SOC_CON10 */
        [0x330 / 4] = 0xffff, /* SOC_CON12 */
        [0x334 / 4] = 0xffff, /* SOC_CON13 */
    },
};

static uint32_t rk3588_grf_rw_mask(unsigned int bank, unsigned int offset)
{
    switch (bank) {
    case RK3588_GRF_MMIO_PMU0:
        switch (offset) {
        case 0x080 ... 0x08c: /* OS_REG8..11 */
            return UINT32_MAX;
        }
        break;
    case RK3588_GRF_MMIO_PMU1:
        switch (offset) {
        case 0x02c: /* SOC_CON11 */
        case 0x200 ... 0x21c: /* OS_REG0..7 */
            return UINT32_MAX;
        case 0x3b0: /* SD_DETECT_CNT */
            return 0x003fffff;
        }
        break;
    case RK3588_GRF_MMIO_SYS:
        if (offset == 0x32c) { /* SOC_CON11 */
            return UINT32_MAX;
        }
        break;
    }

    return 0;
}

static uint64_t rk3588_grf_hiword_pre_write(RegisterInfo *reg,
                                             uint64_t value)
{
    uint32_t old = *(uint32_t *)reg->data;
    uint32_t mask = value >> 16;

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

typedef struct RK3588DRAMGroup {
    uint64_t size;
    uint32_t sys_reg2;
    uint32_t sys_reg3;
} RK3588DRAMGroup;

static bool rk3588_grf_encode_group(uint64_t size, uint32_t dram_type,
                                    RK3588DRAMGroup *group)
{
    bool row_3_4 = size % 3 == 0 && is_power_of_2(size / 3);
    uint64_t geometry = row_3_4 ? size / 3 * 4 : size;
    unsigned int col, row, row_code;

    if (!is_power_of_2(geometry)) {
        return false;
    }

    /* Each group describes one channel, one rank, 8 banks and a 32-bit bus. */
    col = geometry < 256 * MiB ? 9 : 10;
    row = ctz64(geometry) - col - 3 - 2;
    if (row < 12 || row > 19) {
        return false;
    }

    /* U-Boot's DRAM geometry format uses row code 7 for 12 row bits. */
    row_code = row == 12 ? 7 : row - 13;
    group->size = size;
    group->sys_reg2 = FIELD_DP32(0, PMU1_OS_REG2, DDRTYPE, dram_type & 7);
    group->sys_reg2 = FIELD_DP32(group->sys_reg2, PMU1_OS_REG2,
                                 CHINFO, 1);
    group->sys_reg2 = FIELD_DP32(group->sys_reg2, PMU1_OS_REG2,
                                 CS0_COL, col - 9);
    group->sys_reg2 = FIELD_DP32(group->sys_reg2, PMU1_OS_REG2,
                                 CS0_ROW, row_code & 3);
    if (row_3_4) {
        group->sys_reg2 = FIELD_DP32(group->sys_reg2, PMU1_OS_REG2,
                                     ROW_3_4, 1);
    }
    group->sys_reg3 = FIELD_DP32(0, PMU1_OS_REG3, VERSION,
                                 RK3588_DDR_SYS_REG_VERSION);
    group->sys_reg3 = FIELD_DP32(group->sys_reg3, PMU1_OS_REG3,
                                 EXTEND_DDRTYPE, dram_type >> 3);
    group->sys_reg3 = FIELD_DP32(group->sys_reg3, PMU1_OS_REG3,
                                 EXTEND_CS0_ROW, (row_code >> 2) & 1);

    return true;
}

static bool rk3588_grf_encode_ram(uint64_t ram_size, uint32_t dram_type,
                                  RK3588DRAMGroup groups[2])
{
    RK3588DRAMGroup candidates[18];
    size_t count = 0;
    size_t low = 0, high;
    bool found = false;

    /* Add each 3/4 geometry before the larger power-of-two capacity. */
    for (uint64_t geometry = 64 * MiB; geometry <= 16 * GiB;
         geometry *= 2) {
        if (rk3588_grf_encode_group(geometry / 4 * 3, dram_type,
                                    &candidates[count])) {
            count++;
        }
        if (rk3588_grf_encode_group(geometry, dram_type,
                                    &candidates[count])) {
            count++;
        }
    }

    /* Two pointers find exact sums while moving toward an even split. */
    high = count;
    while (low < high) {
        size_t right = high - 1;
        uint64_t sum = candidates[low].size + candidates[right].size;

        if (sum < ram_size) {
            low++;
        } else if (sum > ram_size) {
            high = right;
        } else {
            groups[0] = candidates[low];
            groups[1] = candidates[right];
            found = true;
            low++;
            high = right;
        }
    }

    return found;
}

static void rk3588_grf_realize(DeviceState *dev, Error **errp)
{
    RK3588GRFState *s = RK3588_GRF(dev);
    RK3588DRAMGroup groups[2];

    if (s->ram_size < 128 * MiB || s->ram_size > 16 * GiB ||
        !rk3588_grf_encode_ram(s->ram_size, s->dram_type, groups)) {
        g_autofree char *size = size_to_str(s->ram_size);

        error_setg(errp, "rk3588-grf: unsupported RAM size %s", size);
        error_append_hint(errp, "RAM size must be between 128 MiB and 16 GiB "
                          "and representable by two DRAM geometry groups.\n");
        return;
    }

    /* Boot state supplied in place of DDR firmware initialization. */
    s->access_info[RK3588_GRF_MMIO_PMU0]
                  [R_PMU0_WARM_BOOT_MAGIC].reset =
        RK3588_PMU0_WARM_BOOT_MAGIC;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG2].reset =
        groups[0].sys_reg2;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG3].reset =
        groups[0].sys_reg3;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG4].reset =
        groups[1].sys_reg2;
    s->access_info[RK3588_GRF_MMIO_PMU1][R_PMU1_OS_REG5].reset =
        groups[1].sys_reg3;
    s->access_info[RK3588_GRF_MMIO_SYS][R_SYS_CORE_STATUS].reset =
        RK3588_SYS_CORE_STATUS;
}

static void rk3588_grf_reset_hold(Object *obj, ResetType type)
{
    RK3588GRFState *s = RK3588_GRF(obj);

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
            if (rk3588_grf_hiword_masks[bank][reg]) {
                /* .ro applies first; preserve high write-enable bits. */
                access->ro = (uint16_t)~rk3588_grf_hiword_masks[bank][reg];
                access->pre_write = rk3588_grf_hiword_pre_write;
            } else {
                access->ro = ~rk3588_grf_rw_mask(bank, access->addr);
            }
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

    dc->realize = rk3588_grf_realize;
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

/*
 * Rockchip IOMMU
 *
 * This is a minimal RK3588-oriented model for Linux rockchip-iommu driver
 * bring-up. It accepts the control path used for domain attach/map/zap, but
 * does not translate DMA or generate IOMMU faults/IRQs. Current RKNN
 * fake-completion does not perform DMA.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/rockchip_iommu.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

REG32(DTE_ADDR, 0x00)
REG32(STATUS, 0x04)
    FIELD(STATUS, PAGING_ENABLED, 0, 1)
    FIELD(STATUS, PAGE_FAULT_ACTIVE, 1, 1)
    FIELD(STATUS, STALL_ACTIVE, 2, 1)
    FIELD(STATUS, IDLE, 3, 1)
    FIELD(STATUS, REPLAY_BUFFER_EMPTY, 4, 1)
    FIELD(STATUS, PAGE_FAULT_IS_WRITE, 5, 1)
    FIELD(STATUS, STALL_NOT_ACTIVE, 31, 1)
REG32(COMMAND, 0x08)
REG32(PAGE_FAULT_ADDR, 0x0c)
REG32(ZAP_ONE_LINE, 0x10)
REG32(INT_RAWSTAT, 0x14)
REG32(INT_CLEAR, 0x18)
REG32(INT_MASK, 0x1c)
REG32(INT_STATUS, 0x20)
REG32(AUTO_GATING, 0x24)

enum {
    RK_MMU_CMD_ENABLE_PAGING = 0,
    RK_MMU_CMD_DISABLE_PAGING = 1,
    RK_MMU_CMD_ENABLE_STALL = 2,
    RK_MMU_CMD_DISABLE_STALL = 3,
    RK_MMU_CMD_ZAP_CACHE = 4,
    RK_MMU_CMD_PAGE_FAULT_DONE = 5,
    RK_MMU_CMD_FORCE_RESET = 6,
};

#define ROCKCHIP_IOMMU_STATUS_RESET \
    (R_STATUS_IDLE_MASK | R_STATUS_REPLAY_BUFFER_EMPTY_MASK | \
     R_STATUS_STALL_NOT_ACTIVE_MASK)

static unsigned int rockchip_iommu_index(RegisterInfo *reg)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);

    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        if (reg >= &s->regs_info[i][0] &&
            reg < &s->regs_info[i][ROCKCHIP_IOMMU_R_MAX]) {
            return i;
        }
    }

    g_assert_not_reached();
}

static void rockchip_iommu_update_irq(RockchipIOMMUState *s, unsigned int i)
{
    s->regs[i][R_INT_STATUS] = s->regs[i][R_INT_RAWSTAT] &
                               s->regs[i][R_INT_MASK];
}

static void rockchip_iommu_command_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);
    unsigned int i = rockchip_iommu_index(reg);
    uint32_t status = s->regs[i][R_STATUS];

    switch (val) {
    case RK_MMU_CMD_ENABLE_PAGING:
        status |= R_STATUS_PAGING_ENABLED_MASK;
        break;
    case RK_MMU_CMD_DISABLE_PAGING:
        status &= ~R_STATUS_PAGING_ENABLED_MASK;
        break;
    case RK_MMU_CMD_ENABLE_STALL:
        if (status & R_STATUS_PAGING_ENABLED_MASK) {
            status |= R_STATUS_STALL_ACTIVE_MASK;
            status &= ~R_STATUS_STALL_NOT_ACTIVE_MASK;
        }
        break;
    case RK_MMU_CMD_DISABLE_STALL:
        status &= ~R_STATUS_STALL_ACTIVE_MASK;
        status |= R_STATUS_STALL_NOT_ACTIVE_MASK;
        break;
    case RK_MMU_CMD_ZAP_CACHE:
        break;
    case RK_MMU_CMD_PAGE_FAULT_DONE:
        status &= ~(R_STATUS_PAGE_FAULT_ACTIVE_MASK |
                    R_STATUS_PAGE_FAULT_IS_WRITE_MASK);
        s->regs[i][R_INT_RAWSTAT] = 0;
        rockchip_iommu_update_irq(s, i);
        break;
    case RK_MMU_CMD_FORCE_RESET:
        s->regs[i][R_DTE_ADDR] = 0;
        s->regs[i][R_INT_RAWSTAT] = 0;
        s->regs[i][R_INT_STATUS] = 0;
        s->regs[i][R_PAGE_FAULT_ADDR] = 0;
        status = ROCKCHIP_IOMMU_STATUS_RESET;
        break;
    default:
        break;
    }

    status |= R_STATUS_IDLE_MASK | R_STATUS_REPLAY_BUFFER_EMPTY_MASK;
    s->regs[i][R_STATUS] = status;
}

static uint64_t rockchip_iommu_int_clear_prew(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);
    unsigned int i = rockchip_iommu_index(reg);

    s->regs[i][R_INT_RAWSTAT] &= ~((uint32_t)val);
    rockchip_iommu_update_irq(s, i);
    return 0;
}

static void rockchip_iommu_int_mask_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);

    rockchip_iommu_update_irq(s, rockchip_iommu_index(reg));
}

static const RegisterAccessInfo rockchip_iommu_regs_info[] = {
    {   .name = "DTE_ADDR", .addr = A_DTE_ADDR,
    }, { .name = "STATUS", .addr = A_STATUS,
        .reset = ROCKCHIP_IOMMU_STATUS_RESET,
        .ro = UINT32_MAX,
    }, { .name = "COMMAND", .addr = A_COMMAND,
        .post_write = rockchip_iommu_command_postw,
    }, { .name = "PAGE_FAULT_ADDR", .addr = A_PAGE_FAULT_ADDR,
        .ro = UINT32_MAX,
    }, { .name = "ZAP_ONE_LINE", .addr = A_ZAP_ONE_LINE,
    }, { .name = "INT_RAWSTAT", .addr = A_INT_RAWSTAT,
        .ro = UINT32_MAX,
    }, { .name = "INT_CLEAR", .addr = A_INT_CLEAR,
        .pre_write = rockchip_iommu_int_clear_prew,
    }, { .name = "INT_MASK", .addr = A_INT_MASK,
        .post_write = rockchip_iommu_int_mask_postw,
    }, { .name = "INT_STATUS", .addr = A_INT_STATUS,
        .ro = UINT32_MAX,
    }, { .name = "AUTO_GATING", .addr = A_AUTO_GATING,
    },
};

static const MemoryRegionOps rockchip_iommu_reg_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rockchip_iommu_reset(DeviceState *dev)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(dev);

    for (unsigned int i = 0; i < ROCKCHIP_IOMMU_MAX_MMU; i++) {
        for (unsigned int r = 0; r < ARRAY_SIZE(s->regs_info[i]); r++) {
            s->regs[i][r] = s->regs_info[i][r].access->reset;
        }
        rockchip_iommu_update_irq(s, i);
    }
}

static void rockchip_iommu_init(Object *obj)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(obj);
    DeviceState *dev = DEVICE(obj);

    for (unsigned int i = 0; i < ROCKCHIP_IOMMU_MAX_MMU; i++) {
        s->reg_array[i] =
            register_init_block32(dev, rockchip_iommu_regs_info,
                                  ARRAY_SIZE(rockchip_iommu_regs_info),
                                  s->regs_info[i], s->regs[i],
                                  &rockchip_iommu_reg_ops, false,
                                  ROCKCHIP_IOMMU_WINDOW_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array[i]->mem);
    }
}

static const VMStateDescription vmstate_rockchip_iommu = {
    .name = TYPE_ROCKCHIP_IOMMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(regs, RockchipIOMMUState,
                               ROCKCHIP_IOMMU_MAX_MMU,
                               ROCKCHIP_IOMMU_R_MAX),
        VMSTATE_UINT32(num_mmu, RockchipIOMMUState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rockchip_iommu_properties[] = {
    DEFINE_PROP_UINT32("num-mmu", RockchipIOMMUState, num_mmu, 1),
};

static void rockchip_iommu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rockchip_iommu_reset);
    dc->vmsd = &vmstate_rockchip_iommu;
    device_class_set_props(dc, rockchip_iommu_properties);
    dc->user_creatable = false;
}

static const TypeInfo rockchip_iommu_info = {
    .name = TYPE_ROCKCHIP_IOMMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipIOMMUState),
    .instance_init = rockchip_iommu_init,
    .class_init = rockchip_iommu_class_init,
};

static void rockchip_iommu_register_types(void)
{
    type_register_static(&rockchip_iommu_info);
}

type_init(rockchip_iommu_register_types)

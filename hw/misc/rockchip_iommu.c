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
#include "system/dma.h"
#include "trace.h"

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

#define ROCKCHIP_IOMMU_DTE_VALID BIT(0)
#define ROCKCHIP_IOMMU_PTE_VALID BIT(0)
#define ROCKCHIP_IOMMU_V2_DESC_ADDRESS_MASK 0xfffffff0U
#define ROCKCHIP_IOMMU_V2_DESC_HI_MASK1 0x00000f00U
#define ROCKCHIP_IOMMU_V2_DESC_HI_MASK2 0x000000f0U
#define ROCKCHIP_IOMMU_V2_DESC_HI_SHIFT1 24
#define ROCKCHIP_IOMMU_V2_DESC_HI_SHIFT2 32

static unsigned int rockchip_iommu_bank(RegisterInfo *reg)
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

static bool rockchip_iommu_read_u32(hwaddr addr, uint32_t *val)
{
    uint8_t buf[sizeof(uint32_t)];

    if (dma_memory_read(&address_space_memory, addr, buf, sizeof(buf),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }

    *val = ldl_le_p(buf);
    return true;
}

static hwaddr rockchip_iommu_v2_desc_address(uint32_t desc)
{
    uint64_t raw = desc;

    return ((raw & ROCKCHIP_IOMMU_V2_DESC_HI_MASK2) <<
            ROCKCHIP_IOMMU_V2_DESC_HI_SHIFT2) |
           ((raw & ROCKCHIP_IOMMU_V2_DESC_HI_MASK1) <<
            ROCKCHIP_IOMMU_V2_DESC_HI_SHIFT1) |
           (raw & ROCKCHIP_IOMMU_V2_DESC_ADDRESS_MASK);
}

static bool rockchip_iommu_bank_iova_to_phys(RockchipIOMMUState *s,
                                             unsigned int i, uint32_t iova,
                                             hwaddr *phys,
                                             const char **reason)
{
    hwaddr dt_addr = rockchip_iommu_v2_desc_address(s->regs[i][R_DTE_ADDR]);
    uint32_t dte_index = extract32(iova, 22, 10);
    uint32_t pte_index = extract32(iova, 12, 10);
    uint32_t page_offset = extract32(iova, 0, 12);
    uint32_t dte;
    uint32_t pte;

    if (!dt_addr) {
        *reason = "no-dte-addr";
        return false;
    }

    if (!rockchip_iommu_read_u32(dt_addr + dte_index * sizeof(uint32_t),
                                 &dte)) {
        *reason = "dte-read-failed";
        return false;
    }

    if (!(dte & ROCKCHIP_IOMMU_DTE_VALID)) {
        *reason = "dte-invalid";
        return false;
    }

    if (!rockchip_iommu_read_u32(rockchip_iommu_v2_desc_address(dte) +
                                 pte_index * sizeof(uint32_t), &pte)) {
        *reason = "pte-read-failed";
        return false;
    }

    if (!(pte & ROCKCHIP_IOMMU_PTE_VALID)) {
        *reason = "pte-invalid";
        return false;
    }

    *phys = rockchip_iommu_v2_desc_address(pte) + page_offset;
    *reason = "ok";
    return true;
}

bool rockchip_iommu_iova_to_phys(RockchipIOMMUState *s, uint32_t iova,
                                 hwaddr *phys, unsigned int *bank,
                                 const char **reason)
{
    unsigned int num_mmu = MIN(s->num_mmu, ROCKCHIP_IOMMU_MAX_MMU);

    if (!num_mmu) {
        *reason = "no-mmu-bank";
        return false;
    }

    for (unsigned int i = 0; i < num_mmu; i++) {
        if (rockchip_iommu_bank_iova_to_phys(s, i, iova, phys, reason)) {
            *bank = i;
            return true;
        }
    }

    *bank = 0;
    return false;
}

static void rockchip_iommu_dte_addr_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);

    trace_rockchip_iommu_dte_addr(s->core_index, rockchip_iommu_bank(reg),
                                  val);
}

static void rockchip_iommu_command_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);
    unsigned int i = rockchip_iommu_bank(reg);
    uint32_t status = s->regs[i][R_STATUS];

    trace_rockchip_iommu_command(s->core_index, i, val);

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
    unsigned int i = rockchip_iommu_bank(reg);

    s->regs[i][R_INT_RAWSTAT] &= ~((uint32_t)val);
    rockchip_iommu_update_irq(s, i);
    return 0;
}

static void rockchip_iommu_int_mask_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);
    unsigned int i = rockchip_iommu_bank(reg);

    rockchip_iommu_update_irq(s, i);
    trace_rockchip_iommu_int_mask(s->core_index, i, val,
                                  s->regs[i][R_INT_STATUS]);
}

static void rockchip_iommu_zap_one_line_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);

    trace_rockchip_iommu_zap_one_line(s->core_index, rockchip_iommu_bank(reg),
                                      val);
}

static const RegisterAccessInfo rockchip_iommu_regs_info[] = {
    {   .name = "DTE_ADDR", .addr = A_DTE_ADDR,
        .post_write = rockchip_iommu_dte_addr_postw,
    }, { .name = "STATUS", .addr = A_STATUS,
        .reset = ROCKCHIP_IOMMU_STATUS_RESET,
        .ro = UINT32_MAX,
    }, { .name = "COMMAND", .addr = A_COMMAND,
        .post_write = rockchip_iommu_command_postw,
    }, { .name = "PAGE_FAULT_ADDR", .addr = A_PAGE_FAULT_ADDR,
        .ro = UINT32_MAX,
    }, { .name = "ZAP_ONE_LINE", .addr = A_ZAP_ONE_LINE,
        .post_write = rockchip_iommu_zap_one_line_postw,
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
    DEFINE_PROP_UINT32("core-index", RockchipIOMMUState, core_index, 0),
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

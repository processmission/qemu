/*
 * Rockchip IOMMU
 *
 * This is a minimal RK3588-oriented model for Linux rockchip-iommu driver
 * bring-up. It accepts the control path used for domain attach/map/zap and
 * exposes bounded v2 page-table translation through QEMU's standard IOMMU
 * memory-region interface and reports translation faults to the guest.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/rockchip_iommu.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
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
#define ROCKCHIP_IOMMU_PTE_READABLE BIT(1)
#define ROCKCHIP_IOMMU_PTE_WRITABLE BIT(2)
#define ROCKCHIP_IOMMU_V2_DESC_LOW_ADDRESS_MASK 0xfffff000U
#define ROCKCHIP_IOMMU_V2_DESC_HI_MASK1 0x00000f00U
#define ROCKCHIP_IOMMU_V2_DESC_HI_MASK2 0x000000f0U
#define ROCKCHIP_IOMMU_V2_DESC_HI_SHIFT1 24
#define ROCKCHIP_IOMMU_V2_DESC_HI_SHIFT2 32
#define ROCKCHIP_IOMMU_PAGE_SIZE 0x1000
#define ROCKCHIP_IOMMU_IRQ_PAGE_FAULT BIT(0)
#define ROCKCHIP_IOMMU_IRQ_BUS_ERROR BIT(1)

typedef enum RockchipIOMMUTranslateResult {
    ROCKCHIP_IOMMU_TRANSLATE_OK,
    ROCKCHIP_IOMMU_TRANSLATE_DISABLED,
    ROCKCHIP_IOMMU_TRANSLATE_PAGE_FAULT,
    ROCKCHIP_IOMMU_TRANSLATE_BUS_ERROR,
} RockchipIOMMUTranslateResult;

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

static void rockchip_iommu_update_irq(RockchipIOMMUState *s)
{
    bool level = false;
    unsigned int num_mmu = MIN(s->num_mmu, ROCKCHIP_IOMMU_MAX_MMU);

    for (unsigned int i = 0; i < num_mmu; i++) {
        s->regs[i][R_INT_STATUS] = s->regs[i][R_INT_RAWSTAT] &
                                   s->regs[i][R_INT_MASK];
        level |= s->regs[i][R_INT_STATUS] != 0;
    }
    qemu_set_irq(s->irq, level);
}

static void rockchip_iommu_reset_bank(RockchipIOMMUState *s, unsigned int i)
{
    for (unsigned int r = 0; r < ARRAY_SIZE(s->regs_info[i]); r++) {
        s->regs[i][r] = s->regs_info[i][r].access->reset;
    }
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
           (raw & ROCKCHIP_IOMMU_V2_DESC_LOW_ADDRESS_MASK);
}

static RockchipIOMMUTranslateResult rockchip_iommu_bank_translate(
    RockchipIOMMUState *s, unsigned int i, uint32_t iova, hwaddr *phys,
    IOMMUAccessFlags *perm)
{
    hwaddr dt_addr = rockchip_iommu_v2_desc_address(s->regs[i][R_DTE_ADDR]);
    uint32_t dte_index = extract32(iova, 22, 10);
    uint32_t pte_index = extract32(iova, 12, 10);
    uint32_t page_offset = extract32(iova, 0, 12);
    uint32_t dte;
    uint32_t pte;

    if (!(s->regs[i][R_STATUS] & R_STATUS_PAGING_ENABLED_MASK)) {
        return ROCKCHIP_IOMMU_TRANSLATE_DISABLED;
    }

    if (!dt_addr) {
        return ROCKCHIP_IOMMU_TRANSLATE_PAGE_FAULT;
    }

    if (!rockchip_iommu_read_u32(dt_addr + dte_index * sizeof(uint32_t),
                                 &dte)) {
        return ROCKCHIP_IOMMU_TRANSLATE_BUS_ERROR;
    }

    if (!(dte & ROCKCHIP_IOMMU_DTE_VALID)) {
        return ROCKCHIP_IOMMU_TRANSLATE_PAGE_FAULT;
    }

    if (!rockchip_iommu_read_u32(rockchip_iommu_v2_desc_address(dte) +
                                 pte_index * sizeof(uint32_t), &pte)) {
        return ROCKCHIP_IOMMU_TRANSLATE_BUS_ERROR;
    }

    if (!(pte & ROCKCHIP_IOMMU_PTE_VALID)) {
        return ROCKCHIP_IOMMU_TRANSLATE_PAGE_FAULT;
    }

    *phys = rockchip_iommu_v2_desc_address(pte) + page_offset;
    *perm = IOMMU_ACCESS_FLAG(pte & ROCKCHIP_IOMMU_PTE_READABLE,
                              pte & ROCKCHIP_IOMMU_PTE_WRITABLE);
    return ROCKCHIP_IOMMU_TRANSLATE_OK;
}

static void rockchip_iommu_latch_fault(RockchipIOMMUState *s,
                                       unsigned int i, uint32_t iova,
                                       IOMMUAccessFlags flag,
                                       RockchipIOMMUTranslateResult result)
{
    uint32_t status = s->regs[i][R_STATUS];
    uint32_t rawstat = s->regs[i][R_INT_RAWSTAT];
    bool first_fault = !(rawstat & (ROCKCHIP_IOMMU_IRQ_PAGE_FAULT |
                                    ROCKCHIP_IOMMU_IRQ_BUS_ERROR));

    if (result == ROCKCHIP_IOMMU_TRANSLATE_PAGE_FAULT) {
        if (status & R_STATUS_PAGE_FAULT_ACTIVE_MASK) {
            return;
        }
        status |= R_STATUS_PAGE_FAULT_ACTIVE_MASK;
        if (flag & IOMMU_WO) {
            status |= R_STATUS_PAGE_FAULT_IS_WRITE_MASK;
        } else {
            status &= ~R_STATUS_PAGE_FAULT_IS_WRITE_MASK;
        }
        rawstat |= ROCKCHIP_IOMMU_IRQ_PAGE_FAULT;
    } else if (result == ROCKCHIP_IOMMU_TRANSLATE_BUS_ERROR) {
        rawstat |= ROCKCHIP_IOMMU_IRQ_BUS_ERROR;
    } else {
        return;
    }

    if (first_fault) {
        s->regs[i][R_PAGE_FAULT_ADDR] = iova;
    }
    s->regs[i][R_INT_RAWSTAT] = rawstat;
    s->regs[i][R_STATUS] = status;
    rockchip_iommu_update_irq(s);
}

static bool rockchip_iommu_paging_enabled(RockchipIOMMUState *s,
                                          unsigned int num_mmu)
{
    for (unsigned int i = 0; i < num_mmu; i++) {
        if (s->regs[i][R_STATUS] & R_STATUS_PAGING_ENABLED_MASK) {
            return true;
        }
    }

    return false;
}

static IOMMUTLBEntry rockchip_iommu_translate_internal(
    RockchipIOMMUState *s, hwaddr addr, IOMMUAccessFlags flag,
    unsigned int *translated_bank, bool report_fault)
{
    unsigned int num_mmu = MIN(s->num_mmu, ROCKCHIP_IOMMU_MAX_MMU);
    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr & ~(ROCKCHIP_IOMMU_PAGE_SIZE - 1),
        .translated_addr = 0,
        .addr_mask = ROCKCHIP_IOMMU_PAGE_SIZE - 1,
        .perm = IOMMU_NONE,
    };
    IOMMUAccessFlags perm;
    hwaddr phys;
    RockchipIOMMUTranslateResult fault = ROCKCHIP_IOMMU_TRANSLATE_DISABLED;
    unsigned int fault_bank = 0;
    bool permission_fault = false;

    if (!num_mmu || addr > UINT32_MAX) {
        return entry;
    }

    if (!rockchip_iommu_paging_enabled(s, num_mmu)) {
        entry.translated_addr = entry.iova;
        entry.perm = IOMMU_RW;
        if (translated_bank) {
            *translated_bank = 0;
        }
        return entry;
    }

    for (unsigned int i = 0; i < num_mmu; i++) {
        RockchipIOMMUTranslateResult result =
            rockchip_iommu_bank_translate(s, i, addr, &phys, &perm);

        if (result == ROCKCHIP_IOMMU_TRANSLATE_OK &&
            (flag == IOMMU_NONE || (perm & flag) == flag)) {
            trace_rockchip_iommu_translate(i, addr, phys, perm);
            entry.translated_addr = phys & ~entry.addr_mask;
            entry.perm = perm;
            if (translated_bank) {
                *translated_bank = i;
            }
            return entry;
        }
        if (result == ROCKCHIP_IOMMU_TRANSLATE_OK) {
            if (!permission_fault) {
                permission_fault = true;
                fault_bank = i;
            }
            continue;
        }
        if (!permission_fault &&
            fault == ROCKCHIP_IOMMU_TRANSLATE_DISABLED &&
            result != ROCKCHIP_IOMMU_TRANSLATE_DISABLED) {
            fault = result;
            fault_bank = i;
        }
    }

    if (permission_fault) {
        fault = ROCKCHIP_IOMMU_TRANSLATE_PAGE_FAULT;
    }

    if (report_fault && flag != IOMMU_NONE) {
        rockchip_iommu_latch_fault(s, fault_bank, addr, flag, fault);
    }

    return entry;
}

static IOMMUTLBEntry rockchip_iommu_memory_region_translate(
    IOMMUMemoryRegion *iommu, hwaddr addr, IOMMUAccessFlags flag,
    int iommu_idx)
{
    RockchipIOMMUState *s = container_of(iommu, RockchipIOMMUState,
                                         iommu_mr);

    return rockchip_iommu_translate_internal(s, addr, flag, NULL, true);
}

static int rockchip_iommu_notify_flag_changed(
    IOMMUMemoryRegion *iommu, IOMMUNotifierFlag old_flags,
    IOMMUNotifierFlag new_flags, Error **errp)
{
    if (new_flags & ~IOMMU_NOTIFIER_UNMAP) {
        error_setg(errp, TYPE_ROCKCHIP_IOMMU_MEMORY_REGION
                   " only supports IOMMU UNMAP notifiers");
        return -EINVAL;
    }

    return 0;
}

MemoryRegion *rockchip_iommu_get_memory_region(RockchipIOMMUState *s)
{
    return MEMORY_REGION(&s->iommu_mr);
}

bool rockchip_iommu_find_translation_bank(IOMMUMemoryRegion *iommu,
                                           hwaddr addr,
                                           IOMMUAccessFlags flag,
                                           unsigned int *bank)
{
    RockchipIOMMUState *s;
    IOMMUTLBEntry entry;

    if (!bank || !object_dynamic_cast(OBJECT(iommu),
                                      TYPE_ROCKCHIP_IOMMU_MEMORY_REGION)) {
        return false;
    }

    s = container_of(iommu, RockchipIOMMUState, iommu_mr);
    entry = rockchip_iommu_translate_internal(s, addr, flag, bank, false);
    return entry.target_as && entry.perm != IOMMU_NONE &&
           (flag == IOMMU_NONE || (entry.perm & flag) == flag);
}

static void rockchip_iommu_notify_unmap(RockchipIOMMUState *s,
                                        hwaddr iova, hwaddr addr_mask)
{
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &address_space_memory,
            .iova = iova & ~addr_mask,
            .translated_addr = 0,
            .addr_mask = addr_mask,
            .perm = IOMMU_NONE,
        },
    };

    memory_region_notify_iommu(&s->iommu_mr, 0, event);
}

static void rockchip_iommu_notify_unmap_all(RockchipIOMMUState *s)
{
    rockchip_iommu_notify_unmap(s, 0, UINT32_MAX);
}

static void rockchip_iommu_dte_addr_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);

    trace_rockchip_iommu_dte_addr(s->core_index, rockchip_iommu_bank(reg),
                                  val);
    rockchip_iommu_notify_unmap_all(s);
}

static void rockchip_iommu_command_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);
    unsigned int i = rockchip_iommu_bank(reg);
    uint32_t status = s->regs[i][R_STATUS];
    bool invalidate = false;

    trace_rockchip_iommu_command(s->core_index, i, val);

    switch (val) {
    case RK_MMU_CMD_ENABLE_PAGING:
        status |= R_STATUS_PAGING_ENABLED_MASK;
        invalidate = true;
        break;
    case RK_MMU_CMD_DISABLE_PAGING:
        status &= ~R_STATUS_PAGING_ENABLED_MASK;
        invalidate = true;
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
        invalidate = true;
        break;
    case RK_MMU_CMD_PAGE_FAULT_DONE:
        status &= ~(R_STATUS_PAGE_FAULT_ACTIVE_MASK |
                    R_STATUS_PAGE_FAULT_IS_WRITE_MASK);
        break;
    case RK_MMU_CMD_FORCE_RESET:
        rockchip_iommu_reset_bank(s, i);
        status = s->regs[i][R_STATUS];
        invalidate = true;
        break;
    default:
        break;
    }

    status |= R_STATUS_IDLE_MASK | R_STATUS_REPLAY_BUFFER_EMPTY_MASK;
    s->regs[i][R_STATUS] = status;
    rockchip_iommu_update_irq(s);
    if (invalidate) {
        rockchip_iommu_notify_unmap_all(s);
    }
}

static uint64_t rockchip_iommu_int_clear_prew(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);
    unsigned int i = rockchip_iommu_bank(reg);

    s->regs[i][R_INT_RAWSTAT] &= ~((uint32_t)val);
    rockchip_iommu_update_irq(s);
    return 0;
}

static void rockchip_iommu_int_mask_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);
    unsigned int i = rockchip_iommu_bank(reg);

    rockchip_iommu_update_irq(s);
    trace_rockchip_iommu_int_mask(s->core_index, i, val,
                                  s->regs[i][R_INT_STATUS]);
}

static void rockchip_iommu_zap_one_line_postw(RegisterInfo *reg, uint64_t val)
{
    RockchipIOMMUState *s = ROCKCHIP_IOMMU(reg->opaque);

    trace_rockchip_iommu_zap_one_line(s->core_index, rockchip_iommu_bank(reg),
                                      val);
    rockchip_iommu_notify_unmap(s, val, ROCKCHIP_IOMMU_PAGE_SIZE - 1);
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
        .reset = BIT(0),
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
        rockchip_iommu_reset_bank(s, i);
    }
    rockchip_iommu_update_irq(s);
    rockchip_iommu_notify_unmap_all(s);
}

static int rockchip_iommu_post_load(void *opaque, int version_id)
{
    RockchipIOMMUState *s = opaque;

    rockchip_iommu_update_irq(s);
    return 0;
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
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_iommu(&s->iommu_mr, sizeof(s->iommu_mr),
                             TYPE_ROCKCHIP_IOMMU_MEMORY_REGION, obj,
                             "rockchip-iommu-dma", UINT64_C(1) << 32);
}

static const VMStateDescription vmstate_rockchip_iommu = {
    .name = TYPE_ROCKCHIP_IOMMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rockchip_iommu_post_load,
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

static void rockchip_iommu_memory_region_class_init(ObjectClass *klass,
                                                    const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = rockchip_iommu_memory_region_translate;
    imrc->notify_flag_changed = rockchip_iommu_notify_flag_changed;
}

static const TypeInfo rockchip_iommu_memory_region_info = {
    .name = TYPE_ROCKCHIP_IOMMU_MEMORY_REGION,
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .class_init = rockchip_iommu_memory_region_class_init,
};

static void rockchip_iommu_register_types(void)
{
    type_register_static(&rockchip_iommu_memory_region_info);
    type_register_static(&rockchip_iommu_info);
}

type_init(rockchip_iommu_register_types)

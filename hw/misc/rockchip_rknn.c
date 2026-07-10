/*
 * Rockchip RK3588 RKNN/RKNPU core
 *
 * This is a register-level fake-completion model for the Linux mainline
 * rocket driver path. It accepts task submission writes and completes them
 * with the DPU interrupt bits; it does not execute RKNN command streams.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "hw/misc/rockchip_rknn.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "system/dma.h"
#include "trace/control.h"
#include "trace.h"

REG32(PC_VERSION, 0x0000)
REG32(PC_VERSION_NUM, 0x0004)
REG32(PC_OPERATION_ENABLE, 0x0008)
    FIELD(PC_OPERATION_ENABLE, OP_EN, 0, 1)
REG32(PC_BASE_ADDRESS, 0x0010)
REG32(PC_REGISTER_AMOUNTS, 0x0014)
REG32(PC_INTERRUPT_MASK, 0x0020)
REG32(PC_INTERRUPT_CLEAR, 0x0024)
REG32(PC_INTERRUPT_STATUS, 0x0028)
REG32(PC_INTERRUPT_RAW_STATUS, 0x002c)
REG32(PC_TASK_CON, 0x0030)
REG32(PC_TASK_DMA_BASE_ADDR, 0x0034)
REG32(PC_TASK_STATUS, 0x003c)

REG32(CNA_S_POINTER, 0x0004)
REG32(CORE_S_POINTER, 0x0004)

#define ROCKCHIP_RKNN_PC_VERSION 0x00000100
#define ROCKCHIP_RKNN_PC_VERSION_NUM 0x00003588
#define ROCKCHIP_RKNN_DPU_INTERRUPT_BITS 0x00000300
#define ROCKCHIP_RKNN_COMPLETE_DELAY_NS (100 * 1000)
#define ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES 16
#define ROCKCHIP_RKNN_REGCMD_SAMPLE_COMMANDS_MAX 16
#define ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX \
    (ROCKCHIP_RKNN_REGCMD_SAMPLE_COMMANDS_MAX * sizeof(uint64_t))
#define ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK 0xfffffff0U
#define ROCKCHIP_RKNN_PC_REGISTER_AMOUNTS_MASK 0x0000ffffU
#define ROCKCHIP_RKNN_REGCMD_TARGET_PC 0x0101
#define ROCKCHIP_RKNN_REGCMD_TARGET_CNA 0x0201
#define ROCKCHIP_RKNN_REGCMD_TARGET_CORE 0x0801
#define ROCKCHIP_RKNN_REGCMD_TARGET_DPU 0x1001
#define ROCKCHIP_RKNN_REGCMD_TARGET_DPU_RDMA 0x2001
#define ROCKCHIP_RKNN_REGCMD_CNA_BASE 0x1000
#define ROCKCHIP_RKNN_REGCMD_CORE_BASE 0x3000
#define ROCKCHIP_RKNN_REGCMD_DPU_BASE 0x4000
#define ROCKCHIP_RKNN_REGCMD_DPU_RDMA_BASE 0x5000

typedef struct RockchipRKNNRegcmdStats {
    uint32_t pc;
    uint32_t cna;
    uint32_t core;
    uint32_t dpu;
    uint32_t dpu_rdma;
    uint32_t raw;
    uint32_t unknown;
} RockchipRKNNRegcmdStats;

static void rockchip_rknn_update_irq(RockchipRKNNCoreState *s)
{
    bool old_level = s->irq_level;

    s->pc_regs[R_PC_INTERRUPT_STATUS] =
        s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] &
        s->pc_regs[R_PC_INTERRUPT_MASK];
    s->irq_level = s->pc_regs[R_PC_INTERRUPT_STATUS] != 0;
    if (s->irq_level != old_level) {
        trace_rockchip_rknn_irq(s->core_index, s->irq_level,
                                s->pc_regs[R_PC_INTERRUPT_RAW_STATUS],
                                s->pc_regs[R_PC_INTERRUPT_MASK],
                                s->pc_regs[R_PC_INTERRUPT_STATUS]);
    }
    qemu_set_irq(s->irq, s->irq_level);
}

static void rockchip_rknn_complete(void *opaque)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(opaque);

    s->busy = false;
    s->pc_regs[R_PC_OPERATION_ENABLE] &= ~R_PC_OPERATION_ENABLE_OP_EN_MASK;
    s->pc_regs[R_PC_TASK_STATUS] = 1;
    s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] |= ROCKCHIP_RKNN_DPU_INTERRUPT_BITS;
    rockchip_rknn_update_irq(s);
    trace_rockchip_rknn_complete(s->core_index,
                                 s->pc_regs[R_PC_INTERRUPT_RAW_STATUS],
                                 s->pc_regs[R_PC_INTERRUPT_STATUS]);
}

static void rockchip_rknn_clear_regcmd_shadow(RockchipRKNNCoreState *s)
{
    memset(s->regcmd_shadow_pc, 0, sizeof(s->regcmd_shadow_pc));
    memset(s->regcmd_shadow_cna, 0, sizeof(s->regcmd_shadow_cna));
    memset(s->regcmd_shadow_core, 0, sizeof(s->regcmd_shadow_core));
    memset(s->regcmd_shadow_dpu, 0, sizeof(s->regcmd_shadow_dpu));
    memset(s->regcmd_shadow_dpu_rdma, 0, sizeof(s->regcmd_shadow_dpu_rdma));
}

static bool rockchip_rknn_shadow_write_domain(uint32_t reg, uint32_t value,
                                              uint32_t base,
                                              uint32_t shadow[],
                                              const char **domain,
                                              const char *domain_name,
                                              uint32_t *rel)
{
    if (reg < base || reg >= base + ROCKCHIP_RKNN_WINDOW_SIZE) {
        return false;
    }

    *rel = reg - base;
    shadow[*rel / sizeof(uint32_t)] = value;
    *domain = domain_name;
    return true;
}

static bool rockchip_rknn_regcmd_shadow_write(RockchipRKNNCoreState *s,
                                              uint32_t target, uint32_t reg,
                                              uint32_t value,
                                              const char **domain,
                                              uint32_t *rel)
{
    *domain = NULL;
    *rel = 0;

    switch (target) {
    case ROCKCHIP_RKNN_REGCMD_TARGET_PC:
        if (reg >= sizeof(s->regcmd_shadow_pc)) {
            return false;
        }
        s->regcmd_shadow_pc[reg / sizeof(uint32_t)] = value;
        *domain = "PC";
        *rel = reg;
        return true;
    case ROCKCHIP_RKNN_REGCMD_TARGET_CNA:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_CNA_BASE,
            s->regcmd_shadow_cna, domain, "CNA", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_CORE:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_CORE_BASE,
            s->regcmd_shadow_core, domain, "CORE", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_DPU:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_DPU_BASE,
            s->regcmd_shadow_dpu, domain, "DPU", rel);
    case ROCKCHIP_RKNN_REGCMD_TARGET_DPU_RDMA:
        return rockchip_rknn_shadow_write_domain(
            reg, value, ROCKCHIP_RKNN_REGCMD_DPU_RDMA_BASE,
            s->regcmd_shadow_dpu_rdma, domain, "DPU_RDMA", rel);
    default:
        return false;
    }
}

static void rockchip_rknn_ingest_regcmd(RockchipRKNNCoreState *s,
                                        unsigned int bank,
                                        const uint8_t *sample,
                                        uint32_t sample_commands,
                                        uint32_t command_count)
{
    RockchipRKNNRegcmdStats stats = { 0 };
    bool trace_shadow_write =
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SHADOW_WRITE);

    rockchip_rknn_clear_regcmd_shadow(s);

    for (uint32_t i = 0; i < sample_commands; i++) {
        uint64_t raw = ldq_le_p(sample + i * sizeof(uint64_t));
        uint32_t reg = raw & 0xffff;
        uint32_t value = (raw >> 16) & 0xffffffff;
        uint32_t target = (raw >> 48) & 0xffff;
        const char *domain = NULL;
        uint32_t rel = 0;

        if (!rockchip_rknn_regcmd_shadow_write(s, target, reg, value,
                                               &domain, &rel)) {
            if (raw == 0 || target == 0 ||
                target == 0x0041 || target == 0x0081) {
                stats.raw++;
            } else {
                stats.unknown++;
            }
            continue;
        }

        if (trace_shadow_write) {
            trace_rockchip_rknn_regcmd_shadow_write(s->core_index, bank, i,
                                                    domain, rel, value);
        }

        if (g_str_equal(domain, "PC")) {
            stats.pc++;
        } else if (g_str_equal(domain, "CNA")) {
            stats.cna++;
        } else if (g_str_equal(domain, "CORE")) {
            stats.core++;
        } else if (g_str_equal(domain, "DPU")) {
            stats.dpu++;
        } else if (g_str_equal(domain, "DPU_RDMA")) {
            stats.dpu_rdma++;
        }
    }

    stats.raw += command_count - sample_commands;
    trace_rockchip_rknn_regcmd_ingest(s->core_index, bank, command_count,
                                      sample_commands, stats.pc, stats.cna,
                                      stats.core, stats.dpu + stats.dpu_rdma,
                                      stats.raw, stats.unknown);
}

static void rockchip_rknn_trace_regcmd_sample(RockchipRKNNCoreState *s)
{
    uint32_t iova = s->pc_regs[R_PC_BASE_ADDRESS] &
                    ROCKCHIP_RKNN_PC_BASE_ADDRESS_MASK;
    uint32_t amounts = s->pc_regs[R_PC_REGISTER_AMOUNTS] &
                       ROCKCHIP_RKNN_PC_REGISTER_AMOUNTS_MASK;
    uint32_t command_count = amounts + 1;
    uint32_t command_bytes = command_count * sizeof(uint64_t);
    uint32_t sample_bytes;
    uint32_t sample_commands;
    bool trace_sample =
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SAMPLE);
    bool trace_words = trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_WORD);
    bool trace_ingest =
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_INGEST) ||
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SHADOW_WRITE);
    unsigned int bank = 0;
    const char *reason = NULL;
    hwaddr phys = 0;
    uint8_t sample[ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX] = { 0 };

    if (!s->iommu) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "no-iommu-link");
        return;
    }

    if (!rockchip_iommu_iova_to_phys(s->iommu, iova, &phys, &bank, &reason)) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova, reason);
        return;
    }

    if (!trace_sample && !trace_words && !trace_ingest) {
        return;
    }

    sample_bytes = MIN(command_bytes, trace_words || trace_ingest ?
                       ROCKCHIP_RKNN_REGCMD_SAMPLE_BYTES_MAX :
                       ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES);
    sample_bytes = MIN(sample_bytes, 0x1000 - (uint32_t)(phys & 0xfff));
    sample_commands = trace_words || trace_ingest ?
                      sample_bytes / sizeof(uint64_t) : 0;

    if (dma_memory_read(&address_space_memory, phys, sample, sample_bytes,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        trace_rockchip_rknn_regcmd_sample_error(s->core_index, iova,
                                                "sample-read-failed");
        return;
    }

    if (trace_sample) {
        uint32_t summary_bytes = MIN(sample_bytes,
                                     ROCKCHIP_RKNN_REGCMD_SUMMARY_BYTES);

        trace_rockchip_rknn_regcmd_sample(s->core_index, bank, iova, phys,
                                          command_bytes, summary_bytes,
                                          ldl_le_p(sample),
                                          ldl_le_p(sample + 4),
                                          ldl_le_p(sample + 8),
                                          ldl_le_p(sample + 12));
    }

    for (uint32_t i = 0; i < sample_commands; i++) {
        uint64_t raw = ldq_le_p(sample + i * sizeof(uint64_t));
        uint32_t reg = raw & 0xffff;
        uint32_t value = (raw >> 16) & 0xffffffff;
        uint32_t target = (raw >> 48) & 0xffff;

        trace_rockchip_rknn_regcmd_word(s->core_index, bank, i, target, reg,
                                        value, raw);
    }

    if (trace_ingest) {
        rockchip_rknn_ingest_regcmd(s, bank, sample, sample_commands,
                                    command_count);
    }
}

static void rockchip_rknn_start(RockchipRKNNCoreState *s)
{
    s->busy = true;
    s->pc_regs[R_PC_TASK_STATUS] = 0;
    trace_rockchip_rknn_start(s->core_index,
                              s->pc_regs[R_PC_BASE_ADDRESS],
                              s->pc_regs[R_PC_REGISTER_AMOUNTS],
                              s->pc_regs[R_PC_TASK_CON],
                              s->pc_regs[R_PC_TASK_DMA_BASE_ADDR],
                              s->cna_regs[R_CNA_S_POINTER],
                              s->core_regs[R_CORE_S_POINTER]);
    if (trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SAMPLE) ||
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_WORD) ||
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_INGEST) ||
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SHADOW_WRITE) ||
        trace_event_get_state(TRACE_ROCKCHIP_RKNN_REGCMD_SAMPLE_ERROR)) {
        rockchip_rknn_trace_regcmd_sample(s);
    }
    timer_mod(&s->complete_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ROCKCHIP_RKNN_COMPLETE_DELAY_NS);
}

static void rockchip_rknn_operation_enable_postw(RegisterInfo *reg,
                                                 uint64_t val)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(reg->opaque);

    if (val & R_PC_OPERATION_ENABLE_OP_EN_MASK) {
        rockchip_rknn_start(s);
    } else if (s->busy) {
        timer_del(&s->complete_timer);
        s->busy = false;
    }
}

static void rockchip_rknn_interrupt_mask_postw(RegisterInfo *reg,
                                               uint64_t val)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(reg->opaque);

    rockchip_rknn_update_irq(s);
}

static uint64_t rockchip_rknn_interrupt_clear_prew(RegisterInfo *reg,
                                                   uint64_t val)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(reg->opaque);

    s->pc_regs[R_PC_INTERRUPT_RAW_STATUS] &= ~((uint32_t)val);
    rockchip_rknn_update_irq(s);
    return 0;
}

static const RegisterAccessInfo rockchip_rknn_pc_regs_info[] = {
    {   .name = "PC_VERSION", .addr = A_PC_VERSION,
        .reset = ROCKCHIP_RKNN_PC_VERSION,
        .ro = UINT32_MAX,
    }, { .name = "PC_VERSION_NUM", .addr = A_PC_VERSION_NUM,
        .reset = ROCKCHIP_RKNN_PC_VERSION_NUM,
        .ro = UINT32_MAX,
    }, { .name = "PC_OPERATION_ENABLE", .addr = A_PC_OPERATION_ENABLE,
        .post_write = rockchip_rknn_operation_enable_postw,
    }, { .name = "PC_BASE_ADDRESS", .addr = A_PC_BASE_ADDRESS,
    }, { .name = "PC_REGISTER_AMOUNTS", .addr = A_PC_REGISTER_AMOUNTS,
    }, { .name = "PC_INTERRUPT_MASK", .addr = A_PC_INTERRUPT_MASK,
        .post_write = rockchip_rknn_interrupt_mask_postw,
    }, { .name = "PC_INTERRUPT_CLEAR", .addr = A_PC_INTERRUPT_CLEAR,
        .pre_write = rockchip_rknn_interrupt_clear_prew,
    }, { .name = "PC_INTERRUPT_STATUS", .addr = A_PC_INTERRUPT_STATUS,
        .ro = UINT32_MAX,
    }, { .name = "PC_INTERRUPT_RAW_STATUS", .addr = A_PC_INTERRUPT_RAW_STATUS,
        .ro = UINT32_MAX,
    }, { .name = "PC_TASK_CON", .addr = A_PC_TASK_CON,
    }, { .name = "PC_TASK_DMA_BASE_ADDR", .addr = A_PC_TASK_DMA_BASE_ADDR,
    }, { .name = "PC_TASK_STATUS", .addr = A_PC_TASK_STATUS,
        .ro = UINT32_MAX,
    },
};

static const RegisterAccessInfo rockchip_rknn_cna_regs_info[] = {
    {   .name = "CNA_S_POINTER", .addr = A_CNA_S_POINTER,
    },
};

static const RegisterAccessInfo rockchip_rknn_core_regs_info[] = {
    {   .name = "CORE_S_POINTER", .addr = A_CORE_S_POINTER,
    },
};

static const MemoryRegionOps rockchip_rknn_reg_ops = {
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

static void rockchip_rknn_reset(DeviceState *dev)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(dev);

    timer_del(&s->complete_timer);
    s->busy = false;
    rockchip_rknn_clear_regcmd_shadow(s);

    for (unsigned int i = 0; i < ARRAY_SIZE(s->pc_regs_info); i++) {
        register_reset(&s->pc_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->cna_regs_info); i++) {
        register_reset(&s->cna_regs_info[i]);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(s->core_regs_info); i++) {
        register_reset(&s->core_regs_info[i]);
    }

    rockchip_rknn_update_irq(s);
}

static int rockchip_rknn_post_load(void *opaque, int version_id)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(opaque);

    rockchip_rknn_update_irq(s);
    return 0;
}

static void rockchip_rknn_init(Object *obj)
{
    RockchipRKNNCoreState *s = ROCKCHIP_RKNN_CORE(obj);
    DeviceState *dev = DEVICE(obj);

    s->pc_reg_array =
        register_init_block32(dev, rockchip_rknn_pc_regs_info,
                              ARRAY_SIZE(rockchip_rknn_pc_regs_info),
                              s->pc_regs_info, s->pc_regs,
                              &rockchip_rknn_reg_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);
    s->cna_reg_array =
        register_init_block32(dev, rockchip_rknn_cna_regs_info,
                              ARRAY_SIZE(rockchip_rknn_cna_regs_info),
                              s->cna_regs_info, s->cna_regs,
                              &rockchip_rknn_reg_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);
    s->core_reg_array =
        register_init_block32(dev, rockchip_rknn_core_regs_info,
                              ARRAY_SIZE(rockchip_rknn_core_regs_info),
                              s->core_regs_info, s->core_regs,
                              &rockchip_rknn_reg_ops, false,
                              ROCKCHIP_RKNN_WINDOW_SIZE);

    timer_init_ns(&s->complete_timer, QEMU_CLOCK_VIRTUAL,
                  rockchip_rknn_complete, s);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pc_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->cna_reg_array->mem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->core_reg_array->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_rockchip_rknn = {
    .name = TYPE_ROCKCHIP_RKNN_CORE,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rockchip_rknn_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pc_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_PC_R_MAX),
        VMSTATE_UINT32_ARRAY(cna_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_CNA_R_MAX),
        VMSTATE_UINT32_ARRAY(core_regs, RockchipRKNNCoreState,
                             ROCKCHIP_RKNN_CORE_R_MAX),
        VMSTATE_TIMER(complete_timer, RockchipRKNNCoreState),
        VMSTATE_UINT32(core_index, RockchipRKNNCoreState),
        VMSTATE_BOOL(busy, RockchipRKNNCoreState),
        VMSTATE_BOOL(irq_level, RockchipRKNNCoreState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rockchip_rknn_properties[] = {
    DEFINE_PROP_LINK("iommu", RockchipRKNNCoreState, iommu,
                     TYPE_ROCKCHIP_IOMMU, RockchipIOMMUState *),
    DEFINE_PROP_UINT32("core-index", RockchipRKNNCoreState, core_index, 0),
};

static void rockchip_rknn_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rockchip_rknn_reset);
    dc->vmsd = &vmstate_rockchip_rknn;
    device_class_set_props(dc, rockchip_rknn_properties);
    dc->user_creatable = false;
}

static const TypeInfo rockchip_rknn_info = {
    .name = TYPE_ROCKCHIP_RKNN_CORE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipRKNNCoreState),
    .instance_init = rockchip_rknn_init,
    .class_init = rockchip_rknn_class_init,
};

static void rockchip_rknn_register_types(void)
{
    type_register_static(&rockchip_rknn_info);
}

type_init(rockchip_rknn_register_types)

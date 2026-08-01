/*
 * MCS-51 family CPU
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/log.h"
#include "qemu/qemu-print.h"
#include "internals.h"
#include "disas/dis-asm.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "system/address-spaces.h"
#include "accel/tcg/cpu-ops.h"
#include "tcg/debug-assert.h"
#include "trace.h"

static void mcs251_cpu_set_pc(CPUState *cs, vaddr value)
{
    MCS251CPU *cpu = MCS251_CPU(cs);

    cpu->env.pc = value & MCS_TARGET_ADDR_MASK;
}

static vaddr mcs251_cpu_get_pc(CPUState *cs)
{
    MCS251CPU *cpu = MCS251_CPU(cs);

    return cpu->env.pc;
}

static bool mcs251_cpu_has_work(CPUState *cs)
{
    return (cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) &&
            mcs251_cpu_has_interrupt(cs)) ||
           cpu_test_interrupt(cs, CPU_INTERRUPT_RESET);
}

static void mcs251_cpu_update_interrupt_request(MCS251CPU *cpu)
{
    CPUState *cs = CPU(cpu);

    if (cpu->env.irq_pending) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void mcs251_cpu_update_classic_irq_configuration(MCS251CPU *cpu)
{
    CPUMCS251State *env = &cpu->env;
    unsigned irq;

    for (irq = MCS251_IRQ_INT0; irq <= MCS251_IRQ_UART1; irq++) {
        cpu->irq_enabled[irq] = extract8(env->ie, irq, 1);
        cpu->irq_priority[irq] = extract8(env->iph, irq, 1) * 2 +
                                 extract8(env->ip, irq, 1);
    }
    cpu->irq_auto_clear[MCS251_IRQ_INT0] =
        FIELD_EX8(env->tcon, TCON, IT0);
    cpu->irq_auto_clear[MCS251_IRQ_INT1] =
        FIELD_EX8(env->tcon, TCON, IT1);
    mcs251_cpu_update_interrupt_request(cpu);
}

void mcs251_cpu_notify_sfr_write(MCS251CPU *cpu, uint8_t addr,
                                 uint8_t value)
{
    unsigned index;

    for (index = 0; index < cpu->sfr_write_notifier_count; index++) {
        cpu->sfr_write_notifier[index](
            cpu->sfr_write_notifier_opaque[index], addr, value);
    }
}

static int mcs251_cpu_mmu_index(CPUState *cs, bool ifetch)
{
#ifndef TARGET_MCS251
    return ifetch ? MCS51_MMU_CODE_IDX : MCS51_MMU_DATA_IDX;
#else
    return 0;
#endif
}

static TCGTBCPUState mcs251_get_tb_cpu_state(CPUState *cs)
{
    CPUMCS251State *env = cpu_env(cs);

    return (TCGTBCPUState) {
        .pc = env->pc,
#ifndef TARGET_MCS251
        .flags = 0,
#else
        .flags = FIELD_EX8(env->auxr2, AUXR2, CPUMODE) ?
                 MCS251_TB_FLAG_BINARY : 0,
#endif
    };
}

static void mcs251_cpu_synchronize_from_tb(CPUState *cs,
                                           const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu_env(cs)->pc = tb->pc & MCS_TARGET_ADDR_MASK;
}

static void mcs251_restore_state_to_opc(CPUState *cs,
                                        const TranslationBlock *tb,
                                        const uint64_t *data)
{
    cpu_env(cs)->pc = data[0] & MCS_TARGET_ADDR_MASK;
}

static uint8_t mcs251_cpu_parity(CPUMCS251State *env)
{
    return ctpop8(mcs251_cpu_get_reg8(env, MCS251_REG_ACC)) & 1;
}

uint8_t mcs251_cpu_get_psw(CPUMCS251State *env)
{
    uint8_t value = 0;

    value = FIELD_DP8(value, PSW, C, env->flag_c);
    value = FIELD_DP8(value, PSW, AC, env->flag_ac);
    value = FIELD_DP8(value, PSW, F0, env->flag_f0);
    value = FIELD_DP8(value, PSW, RS, env->reg_bank);
    value = FIELD_DP8(value, PSW, OV, env->flag_ov);
    value = FIELD_DP8(value, PSW, F1, env->flag_f1);
    return FIELD_DP8(value, PSW, P, mcs251_cpu_parity(env));
}

void mcs251_cpu_set_psw(CPUMCS251State *env, uint8_t value)
{
    env->flag_c = FIELD_EX8(value, PSW, C);
    env->flag_ac = FIELD_EX8(value, PSW, AC);
    env->flag_f0 = FIELD_EX8(value, PSW, F0);
    env->reg_bank = FIELD_EX8(value, PSW, RS);
    env->flag_ov = FIELD_EX8(value, PSW, OV);
    env->flag_f1 = FIELD_EX8(value, PSW, F1);
}

uint8_t mcs251_cpu_get_psw1(CPUMCS251State *env)
{
    uint8_t value = 0;

    value = FIELD_DP8(value, PSW1, C, env->flag_c);
    value = FIELD_DP8(value, PSW1, AC, env->flag_ac);
    value = FIELD_DP8(value, PSW1, N, env->flag_n);
    value = FIELD_DP8(value, PSW1, RS, env->reg_bank);
    value = FIELD_DP8(value, PSW1, OV, env->flag_ov);
    return FIELD_DP8(value, PSW1, Z, env->flag_z);
}

void mcs251_cpu_set_psw1(CPUMCS251State *env, uint8_t value)
{
    env->flag_c = FIELD_EX8(value, PSW1, C);
    env->flag_ac = FIELD_EX8(value, PSW1, AC);
    env->flag_n = FIELD_EX8(value, PSW1, N);
    env->reg_bank = FIELD_EX8(value, PSW1, RS);
    env->flag_ov = FIELD_EX8(value, PSW1, OV);
    env->flag_z = FIELD_EX8(value, PSW1, Z);
}

uint8_t mcs251_cpu_get_reg8(CPUMCS251State *env, unsigned reg)
{
    if (reg < MCS251_REG_BANK_COUNT) {
        CPUState *cs = env_cpu(env);
        hwaddr addr = mcs251_cpu_idata_phys_addr(
            env->reg_bank * MCS251_REG_BANK_COUNT + reg);

        return address_space_ldub(cs->as, addr, MEMTXATTRS_UNSPECIFIED, NULL);
    }
    if (reg >= MCS251_REG_DPTR_FIRST &&
        reg < MCS251_REG_DPTR_FIRST + MCS251_REG_DPTR_COUNT) {
        unsigned shift = (MCS251_REG_DPL - reg) * 8;
        unsigned selected = FIELD_EX8(env->dps, DPS, SEL);

        return extract32(env->dptr[selected], shift, 8);
    }
    if (reg < MCS251_REG_GENERAL_COUNT || reg >= MCS251_REG_SPX) {
        return env->regs[reg];
    }
    return 0;
}

void mcs251_cpu_set_reg8(CPUMCS251State *env, unsigned reg, uint8_t value)
{
    if (reg < MCS251_REG_BANK_COUNT) {
        CPUState *cs = env_cpu(env);
        hwaddr addr = mcs251_cpu_idata_phys_addr(
            env->reg_bank * MCS251_REG_BANK_COUNT + reg);

        address_space_stb(cs->as, addr, value, MEMTXATTRS_UNSPECIFIED, NULL);
    } else if (reg >= MCS251_REG_DPTR_FIRST &&
               reg < MCS251_REG_DPTR_FIRST + MCS251_REG_DPTR_COUNT) {
        unsigned shift = (MCS251_REG_DPL - reg) * 8;
        unsigned selected = FIELD_EX8(env->dps, DPS, SEL);

        env->dptr[selected] =
            deposit32(env->dptr[selected], shift, 8, value);
    } else if (reg < MCS251_REG_GENERAL_COUNT || reg >= MCS251_REG_SPX) {
        env->regs[reg] = value;
    }
}

uint32_t mcs251_cpu_get_reg(CPUMCS251State *env, unsigned reg,
                            unsigned bytes)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < bytes; i++) {
        unsigned shift = (bytes - i - 1) * 8;

        value = deposit32(value, shift, 8,
                          mcs251_cpu_get_reg8(env, reg + i));
    }
    return value;
}

void mcs251_cpu_set_reg(CPUMCS251State *env, unsigned reg, unsigned bytes,
                        uint32_t value)
{
    unsigned i;

    for (i = 0; i < bytes; i++) {
        unsigned shift = (bytes - i - 1) * 8;

        mcs251_cpu_set_reg8(env, reg + i, extract32(value, shift, 8));
    }
}

uint8_t mcs251_cpu_direct_read(CPUMCS251State *env, uint8_t addr)
{
    CPUState *cs = env_cpu(env);

    if (addr < MCS251_SFR_BASE) {
        return address_space_ldub(cs->as,
                                  mcs251_cpu_idata_phys_addr(addr),
                                  MEMTXATTRS_UNSPECIFIED, NULL);
    }
    return address_space_ldub(cs->as,
                              MCS251_SFR_PHYS_BASE + addr - MCS251_SFR_BASE,
                              MEMTXATTRS_UNSPECIFIED, NULL);
}

uint8_t mcs251_cpu_direct_rmw_read(CPUMCS251State *env, uint8_t addr)
{
    uint8_t value;

    env->direct_rmw = true;
    value = mcs251_cpu_direct_read(env, addr);
    env->direct_rmw = false;
    return value;
}

void mcs251_cpu_direct_write(CPUMCS251State *env, uint8_t addr,
                             uint8_t value)
{
    CPUState *cs = env_cpu(env);

    if (addr < MCS251_SFR_BASE) {
        address_space_stb(cs->as, mcs251_cpu_idata_phys_addr(addr), value,
                          MEMTXATTRS_UNSPECIFIED, NULL);
    } else {
        address_space_stb(cs->as,
                          MCS251_SFR_PHYS_BASE + addr - MCS251_SFR_BASE,
                          value, MEMTXATTRS_UNSPECIFIED, NULL);
    }
}

void mcs251_cpu_direct_write_immediate(CPUMCS251State *env, uint8_t addr,
                                       uint8_t value)
{
    MCS251CPU *cpu = env_archcpu(env);

    mcs251_cpu_direct_write(env, addr, value);
    if (cpu->sfr_immediate_write) {
        cpu->sfr_immediate_write(cpu->sfr_immediate_opaque, addr, value);
    }
}

void mcs251_cpu_set_sfr_immediate_write(MCS251CPU *cpu,
                                        MCS251SFRImmediateWrite callback,
                                        void *opaque)
{
    cpu->sfr_immediate_write = callback;
    cpu->sfr_immediate_opaque = opaque;
}

void mcs251_cpu_add_sfr_write_notifier(MCS251CPU *cpu,
                                       MCS251SFRWriteNotifier callback,
                                       void *opaque)
{
    unsigned index = cpu->sfr_write_notifier_count;

    g_assert(index < MCS251_MAX_SFR_WRITE_NOTIFIERS);
    cpu->sfr_write_notifier[index] = callback;
    cpu->sfr_write_notifier_opaque[index] = opaque;
    cpu->sfr_write_notifier_count++;
}

void mcs251_cpu_configure_irq(MCS251CPU *cpu, unsigned irq,
                              uint32_t vector, unsigned priority,
                              bool enabled, bool auto_clear)
{
    g_assert(irq < MCS251_NUM_IRQS);

    cpu->irq_vector[irq] = vector & MCS_TARGET_ADDR_MASK;
    cpu->irq_priority[irq] = priority;
    cpu->irq_enabled[irq] = enabled;
    cpu->irq_auto_clear[irq] = auto_clear;
    mcs251_cpu_update_interrupt_request(cpu);
}

void mcs251_cpu_sync_irq_configuration(MCS251CPU *cpu)
{
    CPUMCS251State *env = &cpu->env;

    mcs251_cpu_update_classic_irq_configuration(cpu);
    mcs251_cpu_notify_sfr_write(cpu, MCS251_SFR_IE, env->ie);
    mcs251_cpu_notify_sfr_write(cpu, MCS251_SFR_IPH, env->iph);
    mcs251_cpu_notify_sfr_write(cpu, MCS251_SFR_IP, env->ip);
    mcs251_cpu_notify_sfr_write(cpu, MCS251_SFR_INTCLKO, env->intclko);
}

static uint64_t mcs251_cpu_sfr_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    CPUMCS251State *env = opaque;
    uint8_t addr = offset + MCS251_SFR_BASE;

    switch (addr) {
    case MCS251_SFR_SP:
        return mcs251_cpu_get_reg8(env, MCS251_REG_SP);
    case MCS251_SFR_DPL:
        return mcs251_cpu_get_reg8(env, MCS251_REG_DPL);
    case MCS251_SFR_DPH:
        return mcs251_cpu_get_reg8(env, MCS251_REG_DPH);
#ifdef TARGET_MCS251
    case MCS251_SFR_DPXL:
        return mcs251_cpu_get_reg8(env, MCS251_REG_DPXL);
    case MCS251_SFR_SPH:
        return mcs251_cpu_get_reg8(env, MCS251_REG_SPH);
#endif
    case MCS251_SFR_PCON:
        return env->pcon;
    case MCS251_SFR_TCON:
        return env->tcon;
    case MCS251_SFR_AUXR:
        return env->auxr;
    case MCS251_SFR_INTCLKO:
        return env->intclko;
#ifdef TARGET_MCS251
    case MCS251_SFR_AUXR2:
        return env->auxr2;
#endif
    case MCS251_SFR_P2:
        return env->p2;
    case MCS251_SFR_IE:
        return env->ie;
    case MCS251_SFR_IPH:
        return env->iph;
    case MCS251_SFR_IP:
        return env->ip;
    case MCS251_SFR_P_SW2:
        return env->p_sw2;
    case MCS251_SFR_PSW:
        return mcs251_cpu_get_psw(env);
#ifdef TARGET_MCS251
    case MCS251_SFR_PSW1:
        return mcs251_cpu_get_psw1(env);
#endif
    case MCS251_SFR_ACC:
        return mcs251_cpu_get_reg8(env, MCS251_REG_ACC);
    case MCS251_SFR_DPS:
        return env->dps;
#ifndef TARGET_MCS251
    case MCS251_SFR_DPL1:
        return extract32(env->dptr[1], 0, 8);
    case MCS251_SFR_DPH1:
        return extract32(env->dptr[1], 8, 8);
#else
    case MCS251_SFR_CKCON:
        return env->ckcon;
    case MCS251_SFR_MXAX:
        return env->mxax;
#endif
    case MCS251_SFR_B:
        return mcs251_cpu_get_reg8(env, MCS251_REG_B);
    default:
        return 0;
    }
}

static void mcs251_cpu_sfr_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    CPUMCS251State *env = opaque;
    CPUState *cs = env_cpu(env);
    uint8_t addr = offset + MCS251_SFR_BASE;
    uint8_t byte = value;
    bool flush = false;

    switch (addr) {
    case MCS251_SFR_SP:
        mcs251_cpu_set_reg8(env, MCS251_REG_SP, byte);
        break;
    case MCS251_SFR_DPL:
        mcs251_cpu_set_reg8(env, MCS251_REG_DPL, byte);
        break;
    case MCS251_SFR_DPH:
        mcs251_cpu_set_reg8(env, MCS251_REG_DPH, byte);
        break;
#ifdef TARGET_MCS251
    case MCS251_SFR_DPXL:
        mcs251_cpu_set_reg8(env, MCS251_REG_DPXL, byte);
        break;
    case MCS251_SFR_SPH:
        mcs251_cpu_set_reg8(env, MCS251_REG_SPH, byte);
        break;
#endif
    case MCS251_SFR_PCON:
        env->pcon = byte;
#ifndef TARGET_MCS251
        if (FIELD_EX8(byte, PCON, IDL) || FIELD_EX8(byte, PCON, PD)) {
            cs->halted = 1;
        }
#endif
        break;
    case MCS251_SFR_TCON:
        env->tcon = byte;
        break;
    case MCS251_SFR_AUXR:
        flush = FIELD_EX8(env->auxr, AUXR, RAMEXE) !=
                FIELD_EX8(byte, AUXR, RAMEXE);
        env->auxr = byte;
        break;
    case MCS251_SFR_INTCLKO:
        env->intclko = byte;
        break;
#ifdef TARGET_MCS251
    case MCS251_SFR_AUXR2:
        /*
         * The STC manual defines Source mode as the reset mode but does not
         * explicitly publish CPUMODE polarity. The model's documented
         * inference is 0=Source and 1=Binary.
         */
        env->auxr2 = byte & R_AUXR2_CPUMODE_MASK;
        break;
#endif
    case MCS251_SFR_P2:
        env->p2 = byte;
        break;
    case MCS251_SFR_IE:
        env->ie = byte & MCS251_IE_WRITABLE_MASK;
        if (env->timer0_mode3 && FIELD_EX8(env->ie, IE, ET0)) {
            env->timer0_mode3_armed = true;
        }
        break;
    case MCS251_SFR_TA:
        env->ta_touched = true;
        if (byte == MCS251_TA_FIRST_KEY &&
            env->ta_stage == MCS251_TA_STAGE_LOCKED) {
            env->ta_stage = MCS251_TA_STAGE_FIRST_KEY;
        } else if (byte == MCS251_TA_SECOND_KEY &&
                   env->ta_stage == MCS251_TA_STAGE_FIRST_KEY) {
            env->ta_stage = MCS251_TA_STAGE_UNLOCKED;
        } else {
            env->ta_stage = MCS251_TA_STAGE_LOCKED;
        }
        break;
    case MCS251_SFR_IPH:
        env->iph = byte & MCS251_IP_WRITABLE_MASK;
        break;
    case MCS251_SFR_IP:
        env->ip = byte & MCS251_IP_WRITABLE_MASK;
        break;
    case MCS251_SFR_P_SW2:
        flush = FIELD_EX8(env->p_sw2, P_SW2, EAXFR) !=
                FIELD_EX8(byte, P_SW2, EAXFR);
        env->p_sw2 = byte & R_P_SW2_EAXFR_MASK;
        break;
    case MCS251_SFR_PSW:
        mcs251_cpu_set_psw(env, byte);
        break;
#ifdef TARGET_MCS251
    case MCS251_SFR_PSW1:
        mcs251_cpu_set_psw1(env, byte);
        break;
#endif
    case MCS251_SFR_ACC:
        mcs251_cpu_set_reg8(env, MCS251_REG_ACC, byte);
        break;
    case MCS251_SFR_DPS:
        byte &= MCS251_DPS_WRITABLE_MASK;
        if (env->ta_stage != MCS251_TA_STAGE_UNLOCKED) {
            if (FIELD_EX8(byte, DPS, AU0) &&
                !FIELD_EX8(byte, DPS, AU1)) {
                byte = FIELD_DP8(byte, DPS, AU0, 0);
            } else if (!FIELD_EX8(byte, DPS, AU0) &&
                       FIELD_EX8(byte, DPS, AU1)) {
                byte = FIELD_DP8(byte, DPS, AU0, 1);
            }
        }
        env->dps = byte;
        env->ta_stage = MCS251_TA_STAGE_LOCKED;
        env->ta_touched = true;
        break;
#ifndef TARGET_MCS251
    case MCS251_SFR_DPL1:
        env->dptr[1] = deposit32(env->dptr[1], 0, 8, byte);
        break;
    case MCS251_SFR_DPH1:
        env->dptr[1] = deposit32(env->dptr[1], 8, 8, byte);
        break;
#else
    case MCS251_SFR_CKCON:
        flush = FIELD_EX8(env->ckcon, CKCON, EAXRAM) !=
                FIELD_EX8(byte, CKCON, EAXRAM);
        env->ckcon = byte;
        break;
    case MCS251_SFR_MXAX:
        env->mxax = byte;
        break;
#endif
    case MCS251_SFR_B:
        mcs251_cpu_set_reg8(env, MCS251_REG_B, byte);
        break;
    default:
        break;
    }

    if (flush) {
        tlb_flush(cs);
    }
    if (addr == MCS251_SFR_IE || addr == MCS251_SFR_IPH ||
        addr == MCS251_SFR_IP) {
        mcs251_cpu_update_classic_irq_configuration(MCS251_CPU(cs));
    }
    mcs251_cpu_notify_sfr_write(MCS251_CPU(cs), addr,
                                 mcs251_cpu_sfr_read(env, offset, size));
}

static const MemoryRegionOps mcs251_cpu_sfr_ops = {
    .read = mcs251_cpu_sfr_read,
    .write = mcs251_cpu_sfr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static uint64_t mcs251_cpu_disabled_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    return 0;
}

static void mcs251_cpu_disabled_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
}

static const MemoryRegionOps mcs251_cpu_disabled_ops = {
    .read = mcs251_cpu_disabled_read,
    .write = mcs251_cpu_disabled_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void mcs251_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    MCS251CPU *cpu = MCS251_CPU(cs);
    MCS251CPUClass *mcc = MCS251_CPU_GET_CLASS(obj);
    CPUMCS251State *env = &cpu->env;

    if (mcc->parent_phases.hold) {
        mcc->parent_phases.hold(obj, type);
    }

    memset(env, 0, sizeof(*env));
    env->pc = MCS_TARGET_RESET_PC;
#ifndef TARGET_MCS251
    env->dptr[0] = 0;
    env->dptr[1] = 0;
#else
    env->dptr[0] = 0x00010000;
    env->dptr[1] = 0x00010000;
#endif
    env->regs[MCS251_REG_SP] = 0x07;
    env->pcon = 0x30;
    env->auxr = 0x01;
    env->p2 = 0xff;
#ifdef TARGET_MCS251
    env->ckcon = 0x07;
    env->mxax = 0x01;
#endif
    env->irq_ack = UINT32_MAX;
    env->irq_level = UINT32_MAX;

    mcs251_cpu_sync_irq_configuration(cpu);
    mcs251_cpu_notify_sfr_write(cpu, MCS251_SFR_PCON, env->pcon);

    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_RESET);
    trace_mcs51_cpu_reset(cs->cpu_index, env->pc,
                          mcs251_cpu_get_reg8(env, MCS251_REG_SP));
    qemu_log_mask(CPU_LOG_RESET,
                  "%s: CPU %d reset PC=0x%06" PRIx32 " SP=0x%02x\n",
                  object_get_typename(obj), cs->cpu_index, env->pc,
                  mcs251_cpu_get_reg8(env, MCS251_REG_SP));
}

static ObjectClass *mcs251_cpu_class_by_name(const char *cpu_model)
{
    g_autofree char *typename = NULL;
    ObjectClass *oc;

    oc = object_class_by_name(cpu_model);
    if (oc && object_class_dynamic_cast(oc, TYPE_MCS51_CPU)) {
        return oc;
    }

    typename = g_strdup_printf("%s-cpu", cpu_model);
    return object_class_by_name(typename);
}

static void mcs251_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    MCS251CPU *cpu = MCS251_CPU(dev);
    MCS251CPUClass *mcc = MCS251_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    memory_region_init_io(&cpu->sfr, OBJECT(cpu), &mcs251_cpu_sfr_ops,
                          &cpu->env,
#ifndef TARGET_MCS251
                          "mcs51-cpu-sfr",
#else
                          "mcs251-cpu-sfr",
#endif
                          0x80);
    memory_region_init_io(&cpu->disabled, OBJECT(cpu),
                          &mcs251_cpu_disabled_ops, &cpu->env,
#ifndef TARGET_MCS251
                          "mcs51-disabled-access", TARGET_PAGE_SIZE);
#else
                          "mcs251-disabled-access", TARGET_PAGE_SIZE);
#endif

    qemu_init_vcpu(cs);
    cpu_reset(cs);
    mcc->parent_realize(dev, errp);
}

static void mcs251_cpu_set_irq(void *opaque, int irq, int level)
{
    MCS251CPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    uint64_t mask = BIT_ULL(irq);
    bool changed = !!(cpu->env.irq_pending & mask) != !!level;
    int active;

    if (level) {
        cpu->env.irq_pending |= mask;
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu->env.irq_pending &= ~mask;
        mcs251_cpu_update_interrupt_request(cpu);
    }
    if (!changed) {
        return;
    }

    active = cpu->env.irq_level == UINT32_MAX ? -1 : cpu->env.irq_level;
    trace_mcs51_irq_set(cs->cpu_index, irq, level, cpu->env.irq_pending,
                        cpu->env.ie, cpu->env.ip, cpu->env.iph, active);
    qemu_log_mask(CPU_LOG_INT,
                  "%s: CPU %d IRQ %d input %s pending=0x%016" PRIx64
                  " IE=0x%02x IP=0x%02x IPH=0x%02x active=%d\n",
                  object_get_typename(OBJECT(cpu)), cs->cpu_index, irq,
                  level ? "asserted" : "cleared", cpu->env.irq_pending,
                  cpu->env.ie, cpu->env.ip, cpu->env.iph, active);
}

static void mcs251_cpu_init(Object *obj)
{
    MCS251CPU *cpu = MCS251_CPU(obj);
    uint32_t vector_base;
    unsigned irq;

#ifndef TARGET_MCS251
    vector_base = 0;
#else
    vector_base = 0xff0000;
#endif
    for (irq = MCS251_IRQ_INT0; irq <= MCS251_IRQ_UART1; irq++) {
        cpu->irq_vector[irq] = vector_base + 0x0003 + irq * 8;
        cpu->irq_auto_clear[irq] = irq != MCS251_IRQ_UART1;
    }

    qdev_init_gpio_in(DEVICE(cpu), mcs251_cpu_set_irq, MCS251_NUM_IRQS);
}

static void mcs251_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUMCS251State *env = cpu_env(cs);

#ifndef TARGET_MCS251
    qemu_fprintf(f, "PC=%04x SP=%02x DPTR=%04x PSW=%02x\n",
                 env->pc, mcs251_cpu_get_reg8(env, MCS251_REG_SP),
                 mcs251_cpu_get_reg(env, MCS251_REG_DPH, 2),
                 mcs251_cpu_get_psw(env));
    qemu_fprintf(f,
                 "R0=%02x R1=%02x R2=%02x R3=%02x "
                 "R4=%02x R5=%02x R6=%02x R7=%02x\n",
                 mcs251_cpu_get_reg8(env, 0),
                 mcs251_cpu_get_reg8(env, 1),
                 mcs251_cpu_get_reg8(env, 2),
                 mcs251_cpu_get_reg8(env, 3),
                 mcs251_cpu_get_reg8(env, 4),
                 mcs251_cpu_get_reg8(env, 5),
                 mcs251_cpu_get_reg8(env, 6),
                 mcs251_cpu_get_reg8(env, 7));
#else
    int i;

    qemu_fprintf(f, "PC=%06x SPX=%08x DPX=%08x PSW=%02x PSW1=%02x "
                 "mode=%s\n",
                 env->pc, mcs251_cpu_get_reg(env, MCS251_REG_SPX, 4),
                 mcs251_cpu_get_reg(env, MCS251_REG_DPTR_FIRST, 4),
                 mcs251_cpu_get_psw(env),
                 mcs251_cpu_get_psw1(env),
                 FIELD_EX8(env->auxr2, AUXR2, CPUMODE) ?
                 "binary" : "source");
    for (i = MCS251_REG_GENERAL_FIRST;
         i < MCS251_REG_GENERAL_COUNT;
         i += MCS251_REG_BANK_COUNT) {
        qemu_fprintf(f,
                     "R%-2d=%02x R%-2d=%02x R%-2d=%02x R%-2d=%02x "
                     "R%-2d=%02x R%-2d=%02x R%-2d=%02x R%-2d=%02x\n",
                     i, mcs251_cpu_get_reg8(env, i),
                     i + 1, mcs251_cpu_get_reg8(env, i + 1),
                     i + 2, mcs251_cpu_get_reg8(env, i + 2),
                     i + 3, mcs251_cpu_get_reg8(env, i + 3),
                     i + 4, mcs251_cpu_get_reg8(env, i + 4),
                     i + 5, mcs251_cpu_get_reg8(env, i + 5),
                     i + 6, mcs251_cpu_get_reg8(env, i + 6),
                     i + 7, mcs251_cpu_get_reg8(env, i + 7));
    }
#endif
}

static void mcs251_cpu_disas_set_info(const CPUState *cs,
                                      disassemble_info *info)
{
    info->endian = BFD_ENDIAN_BIG;
#ifndef TARGET_MCS251
    info->mach = MCS251_DISAS_MCS51;
#else
    const MCS251CPU *cpu = MCS251_CPU(cs);
    const CPUMCS251State *env = &cpu->env;

    info->mach = FIELD_EX8(env->auxr2, AUXR2, CPUMODE);
#endif
    info->print_insn = mcs251_print_insn;
}

hwaddr mcs251_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    return addr & MCS_TARGET_ADDR_MASK;
}

bool mcs251_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                         MMUAccessType access_type, int mmu_idx,
                         bool probe, uintptr_t retaddr)
{
#ifndef TARGET_MCS251
    hwaddr vpage = address & TARGET_PAGE_MASK;
    hwaddr ppage;
    int prot;

    if (mmu_idx == MCS51_MMU_CODE_IDX) {
        ppage = MCS51_CODE_PHYS_BASE + vpage;
        prot = PAGE_READ | PAGE_EXEC;
    } else {
        g_assert(mmu_idx == MCS51_MMU_DATA_IDX);
        ppage = MCS51_IDATA_PHYS_BASE + vpage;
        prot = PAGE_READ | PAGE_WRITE;
    }
    tlb_set_page(cs, vpage, ppage, prot, mmu_idx, TARGET_PAGE_SIZE);
    return true;
#else
    CPUMCS251State *env = cpu_env(cs);
    hwaddr vpage = address & TARGET_PAGE_MASK;
    hwaddr ppage = vpage;
    int prot = vpage < 0x800000 ?
               PAGE_READ | PAGE_WRITE :
               PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    if (access_type == MMU_INST_FETCH && vpage < 0x800000) {
        prot = 0;
    } else if (vpage >= 0x7e0000 && vpage < 0x7f0000 &&
        !FIELD_EX8(env->p_sw2, P_SW2, EAXFR)) {
        prot = 0;
    } else if (vpage >= 0x7f0000 && vpage < 0x800000 &&
               !FIELD_EX8(env->auxr, AUXR, RAMEXE)) {
        prot = 0;
    } else if (vpage >= 0x030000 && vpage < 0x031000) {
        if (FIELD_EX8(env->ckcon, CKCON, EAXRAM)) {
            prot = 0;
        } else {
            prot = PAGE_READ | PAGE_WRITE;
        }
    } else if (vpage >= 0x800000 && vpage < 0x801000) {
        if (FIELD_EX8(env->ckcon, CKCON, EAXRAM)) {
            /*
             * The alias MemoryRegion is read-only.  Keep write permission
             * in the TLB so attempted stores reach the ROM handling path
             * and are ignored instead of repeatedly faulting TLB fill.
             */
            prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        } else {
            prot = 0;
        }
    }

    if (!prot) {
        if (probe) {
            return false;
        }
        /*
         * The initial model treats disabled/reserved apertures as an
         * unassigned bus: reads return zero and writes are ignored.
         */
        ppage = MCS251_DISABLED_PHYS_BASE;
        switch (access_type) {
        case MMU_DATA_LOAD:
            prot = PAGE_READ;
            break;
        case MMU_DATA_STORE:
            prot = PAGE_WRITE;
            break;
        case MMU_INST_FETCH:
            prot = PAGE_EXEC;
            break;
        default:
            g_assert_not_reached();
        }
    }

    tlb_set_page(cs, vpage, ppage, prot, mmu_idx, TARGET_PAGE_SIZE);
    return true;
#endif
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps mcs251_sysemu_ops = {
    .has_work = mcs251_cpu_has_work,
    .get_phys_addr_debug = mcs251_cpu_get_phys_addr_debug,
};

static const TCGCPUOps mcs251_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = false,
    .initialize = mcs251_translate_init,
    .translate_code = mcs251_translate_code,
    .get_tb_cpu_state = mcs251_get_tb_cpu_state,
    .synchronize_from_tb = mcs251_cpu_synchronize_from_tb,
    .restore_state_to_opc = mcs251_restore_state_to_opc,
    .mmu_index = mcs251_cpu_mmu_index,
    .cpu_exec_interrupt = mcs251_cpu_exec_interrupt,
    .cpu_exec_halt = mcs251_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .tlb_fill = mcs251_cpu_tlb_fill,
    .do_interrupt = mcs251_cpu_do_interrupt,
    .pointer_wrap = cpu_pointer_wrap_uint32,
};

static void mcs251_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    MCS251CPUClass *mcc = MCS251_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, mcs251_cpu_realize,
                                    &mcc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, mcs251_cpu_reset_hold, NULL,
                                       &mcc->parent_phases);

    cc->class_by_name = mcs251_cpu_class_by_name;
    cc->dump_state = mcs251_cpu_dump_state;
    cc->set_pc = mcs251_cpu_set_pc;
    cc->get_pc = mcs251_cpu_get_pc;
    cc->sysemu_ops = &mcs251_sysemu_ops;
    cc->disas_set_info = mcs251_cpu_disas_set_info;
    cc->gdb_read_register = mcs251_cpu_gdb_read_register;
    cc->gdb_write_register = mcs251_cpu_gdb_write_register;
#ifndef TARGET_MCS251
    cc->gdb_core_xml_file = "mcs51-core.xml";
#else
    cc->gdb_core_xml_file = "mcs251-core.xml";
#endif
    cc->tcg_ops = &mcs251_tcg_ops;
    dc->vmsd = &vms_mcs251_cpu;
}

static const TypeInfo mcs51_cpu_types[] = {
    {
        .name = TYPE_MCS51_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(MCS251CPU),
        .instance_align = __alignof(MCS251CPU),
        .instance_init = mcs251_cpu_init,
        .class_size = sizeof(MCS251CPUClass),
        .class_init = mcs251_cpu_class_init,
#ifdef TARGET_MCS251
        .abstract = true,
#endif
    },
#ifdef TARGET_MCS251
    {
        .name = TYPE_MCS251_CPU,
        .parent = TYPE_MCS51_CPU,
    },
#endif
};

DEFINE_TYPES(mcs51_cpu_types)

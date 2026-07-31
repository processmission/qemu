/*
 * STC32G MCS-251 CPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "disas/dis-asm.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "system/address-spaces.h"
#include "accel/tcg/cpu-ops.h"
#include "tcg/debug-assert.h"

enum {
    SFR_P0 = 0x80,
    SFR_SP = 0x81,
    SFR_DPL = 0x82,
    SFR_DPH = 0x83,
    SFR_DPXL = 0x84,
    SFR_SPH = 0x85,
    SFR_PCON = 0x87,
    SFR_TCON = 0x88,
    SFR_AUXR = 0x8e,
    SFR_INTCLKO = 0x8f,
    SFR_AUXR2 = 0x97,
    SFR_IE = 0xa8,
    SFR_TA = 0xae,
    SFR_IPH = 0xb7,
    SFR_IP = 0xb8,
    SFR_P_SW2 = 0xba,
    SFR_PSW = 0xd0,
    SFR_PSW1 = 0xd1,
    SFR_ACC = 0xe0,
    SFR_DPS = 0xe3,
    SFR_CKCON = 0xea,
    SFR_MXAX = 0xeb,
    SFR_B = 0xf0,
};

static void mcs251_cpu_set_pc(CPUState *cs, vaddr value)
{
    MCS251CPU *cpu = MCS251_CPU(cs);

    cpu->env.pc = value & MCS251_ADDR_MASK;
}

static vaddr mcs251_cpu_get_pc(CPUState *cs)
{
    MCS251CPU *cpu = MCS251_CPU(cs);

    return cpu->env.pc;
}

static bool mcs251_cpu_has_work(CPUState *cs)
{
    CPUMCS251State *env = cpu_env(cs);

    return env->irq_pending != 0 ||
           cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_RESET);
}

static int mcs251_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

static TCGTBCPUState mcs251_get_tb_cpu_state(CPUState *cs)
{
    CPUMCS251State *env = cpu_env(cs);

    return (TCGTBCPUState) { .pc = env->pc };
}

static void mcs251_cpu_synchronize_from_tb(CPUState *cs,
                                           const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu_env(cs)->pc = tb->pc & MCS251_ADDR_MASK;
}

static void mcs251_restore_state_to_opc(CPUState *cs,
                                        const TranslationBlock *tb,
                                        const uint64_t *data)
{
    cpu_env(cs)->pc = data[0] & MCS251_ADDR_MASK;
}

static uint8_t mcs251_cpu_parity(CPUMCS251State *env)
{
    return ctpop8(mcs251_cpu_get_reg8(env, 11)) & 1;
}

uint8_t mcs251_cpu_get_psw(CPUMCS251State *env)
{
    return (env->flag_c << 7) |
           (env->flag_ac << 6) |
           (env->flag_f0 << 5) |
           (env->reg_bank << 3) |
           (env->flag_ov << 2) |
           (env->flag_f1 << 1) |
           mcs251_cpu_parity(env);
}

void mcs251_cpu_set_psw(CPUMCS251State *env, uint8_t value)
{
    env->flag_c = extract8(value, 7, 1);
    env->flag_ac = extract8(value, 6, 1);
    env->flag_f0 = extract8(value, 5, 1);
    env->reg_bank = extract8(value, 3, 2);
    env->flag_ov = extract8(value, 2, 1);
    env->flag_f1 = extract8(value, 1, 1);
}

uint8_t mcs251_cpu_get_psw1(CPUMCS251State *env)
{
    return (env->flag_c << 7) |
           (env->flag_ac << 6) |
           (env->flag_n << 5) |
           (env->reg_bank << 3) |
           (env->flag_ov << 2) |
           (env->flag_z << 1);
}

void mcs251_cpu_set_psw1(CPUMCS251State *env, uint8_t value)
{
    env->flag_c = extract8(value, 7, 1);
    env->flag_ac = extract8(value, 6, 1);
    env->flag_n = extract8(value, 5, 1);
    env->reg_bank = extract8(value, 3, 2);
    env->flag_ov = extract8(value, 2, 1);
    env->flag_z = extract8(value, 1, 1);
}

uint8_t mcs251_cpu_get_reg8(CPUMCS251State *env, unsigned reg)
{
    if (reg < 8) {
        CPUState *cs = env_cpu(env);
        hwaddr addr = env->reg_bank * 8 + reg;

        return address_space_ldub(cs->as, addr, MEMTXATTRS_UNSPECIFIED, NULL);
    }
    if (reg >= 56 && reg < 60) {
        unsigned shift = (59 - reg) * 8;

        return env->dptr[env->dps & 1] >> shift;
    }
    if ((reg < 32) || (reg >= 60 && reg < 64)) {
        return env->regs[reg];
    }
    return 0;
}

void mcs251_cpu_set_reg8(CPUMCS251State *env, unsigned reg, uint8_t value)
{
    if (reg < 8) {
        CPUState *cs = env_cpu(env);
        hwaddr addr = env->reg_bank * 8 + reg;

        address_space_stb(cs->as, addr, value, MEMTXATTRS_UNSPECIFIED, NULL);
    } else if (reg >= 56 && reg < 60) {
        unsigned shift = (59 - reg) * 8;
        uint32_t mask = 0xffu << shift;
        unsigned selected = env->dps & 1;

        env->dptr[selected] = (env->dptr[selected] & ~mask) |
                              ((uint32_t)value << shift);
    } else if ((reg < 32) || (reg >= 60 && reg < 64)) {
        env->regs[reg] = value;
    }
}

uint32_t mcs251_cpu_get_reg(CPUMCS251State *env, unsigned reg,
                            unsigned bytes)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < bytes; i++) {
        value = (value << 8) | mcs251_cpu_get_reg8(env, reg + i);
    }
    return value;
}

void mcs251_cpu_set_reg(CPUMCS251State *env, unsigned reg, unsigned bytes,
                        uint32_t value)
{
    unsigned i;

    for (i = 0; i < bytes; i++) {
        unsigned shift = (bytes - i - 1) * 8;

        mcs251_cpu_set_reg8(env, reg + i, value >> shift);
    }
}

uint8_t mcs251_cpu_direct_read(CPUMCS251State *env, uint8_t addr)
{
    CPUState *cs = env_cpu(env);

    if (addr < 0x80) {
        return address_space_ldub(cs->as, addr, MEMTXATTRS_UNSPECIFIED, NULL);
    }
    return address_space_ldub(cs->as, MCS251_SFR_PHYS_BASE + addr - 0x80,
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

    if (addr < 0x80) {
        address_space_stb(cs->as, addr, value, MEMTXATTRS_UNSPECIFIED, NULL);
    } else {
        address_space_stb(cs->as, MCS251_SFR_PHYS_BASE + addr - 0x80,
                          value, MEMTXATTRS_UNSPECIFIED, NULL);
    }
}

static uint64_t mcs251_cpu_sfr_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    CPUMCS251State *env = opaque;
    uint8_t addr = offset + 0x80;

    switch (addr) {
    case SFR_SP:
        return mcs251_cpu_get_reg8(env, 63);
    case SFR_DPL:
        return mcs251_cpu_get_reg8(env, 59);
    case SFR_DPH:
        return mcs251_cpu_get_reg8(env, 58);
    case SFR_DPXL:
        return mcs251_cpu_get_reg8(env, 57);
    case SFR_SPH:
        return mcs251_cpu_get_reg8(env, 62);
    case SFR_PCON:
        return env->pcon;
    case SFR_TCON:
        return env->tcon;
    case SFR_AUXR:
        return env->auxr;
    case SFR_INTCLKO:
        return env->intclko;
    case SFR_AUXR2:
        return env->auxr2;
    case SFR_IE:
        return env->ie;
    case SFR_IPH:
        return env->iph;
    case SFR_IP:
        return env->ip;
    case SFR_P_SW2:
        return env->p_sw2;
    case SFR_PSW:
        return mcs251_cpu_get_psw(env);
    case SFR_PSW1:
        return mcs251_cpu_get_psw1(env);
    case SFR_ACC:
        return mcs251_cpu_get_reg8(env, 11);
    case SFR_DPS:
        return env->dps;
    case SFR_CKCON:
        return env->ckcon;
    case SFR_MXAX:
        return env->mxax;
    case SFR_B:
        return mcs251_cpu_get_reg8(env, 10);
    default:
        return 0;
    }
}

static void mcs251_cpu_sfr_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    CPUMCS251State *env = opaque;
    CPUState *cs = env_cpu(env);
    uint8_t addr = offset + 0x80;
    uint8_t byte = value;
    bool flush = false;

    switch (addr) {
    case SFR_SP:
        mcs251_cpu_set_reg8(env, 63, byte);
        break;
    case SFR_DPL:
        mcs251_cpu_set_reg8(env, 59, byte);
        break;
    case SFR_DPH:
        mcs251_cpu_set_reg8(env, 58, byte);
        break;
    case SFR_DPXL:
        mcs251_cpu_set_reg8(env, 57, byte);
        break;
    case SFR_SPH:
        mcs251_cpu_set_reg8(env, 62, byte);
        break;
    case SFR_PCON:
        env->pcon = byte;
        break;
    case SFR_TCON:
        env->tcon = byte;
        break;
    case SFR_AUXR:
        flush = (env->auxr ^ byte) & 0x02;
        env->auxr = byte;
        break;
    case SFR_INTCLKO:
        env->intclko = byte;
        break;
    case SFR_AUXR2:
        /*
         * The STC manual defines Source mode as the reset mode but does not
         * explicitly publish CPUMODE polarity. The model's documented
         * inference is 0=Source and 1=Binary.
         */
        env->auxr2 = byte & 0x40;
        break;
    case SFR_IE:
        env->ie = byte & 0x9f;
        if (env->timer0_mode3 && (env->ie & 0x02)) {
            env->timer0_mode3_armed = true;
        }
        break;
    case SFR_TA:
        env->ta_touched = true;
        if (byte == 0xaa && env->ta_stage == 0) {
            env->ta_stage = 1;
        } else if (byte == 0x55 && env->ta_stage == 1) {
            env->ta_stage = 2;
        } else {
            env->ta_stage = 0;
        }
        break;
    case SFR_IPH:
        env->iph = byte & 0x1f;
        break;
    case SFR_IP:
        env->ip = byte & 0x1f;
        break;
    case SFR_P_SW2:
        flush = (env->p_sw2 ^ byte) & 0x80;
        env->p_sw2 = byte & 0x80;
        break;
    case SFR_PSW:
        mcs251_cpu_set_psw(env, byte);
        break;
    case SFR_PSW1:
        mcs251_cpu_set_psw1(env, byte);
        break;
    case SFR_ACC:
        mcs251_cpu_set_reg8(env, 11, byte);
        break;
    case SFR_DPS:
        byte &= 0xf9;
        if (env->ta_stage != 2) {
            if ((byte & 0x18) == 0x08) {
                byte &= ~0x18;
            } else if ((byte & 0x18) == 0x10) {
                byte |= 0x08;
            }
        }
        env->dps = byte;
        env->ta_stage = 0;
        env->ta_touched = true;
        break;
    case SFR_CKCON:
        flush = (env->ckcon ^ byte) & 0x80;
        env->ckcon = byte;
        break;
    case SFR_MXAX:
        env->mxax = byte;
        break;
    case SFR_B:
        mcs251_cpu_set_reg8(env, 10, byte);
        break;
    default:
        break;
    }

    if (flush) {
        tlb_flush(cs);
    }
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
    env->pc = MCS251_RESET_PC;
    env->dptr[0] = 0x00010000;
    env->dptr[1] = 0x00010000;
    env->regs[63] = 0x07;
    env->pcon = 0x30;
    env->auxr = 0x01;
    env->ckcon = 0x07;
    env->mxax = 0x01;
    env->irq_ack = UINT32_MAX;
    env->irq_level = UINT32_MAX;

    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_RESET);
}

static ObjectClass *mcs251_cpu_class_by_name(const char *cpu_model)
{
    g_autofree char *typename = NULL;
    ObjectClass *oc;

    oc = object_class_by_name(cpu_model);
    if (oc && object_class_dynamic_cast(oc, TYPE_MCS251_CPU)) {
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
                          &cpu->env, "mcs251-cpu-sfr", 0x80);
    memory_region_init_io(&cpu->disabled, OBJECT(cpu),
                          &mcs251_cpu_disabled_ops, &cpu->env,
                          "mcs251-disabled-access", TARGET_PAGE_SIZE);

    qemu_init_vcpu(cs);
    cpu_reset(cs);
    mcc->parent_realize(dev, errp);
}

static void mcs251_cpu_set_irq(void *opaque, int irq, int level)
{
    MCS251CPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    uint32_t mask = 1u << irq;

    if (level) {
        cpu->env.irq_pending |= mask;
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu->env.irq_pending &= ~mask;
        if (!cpu->env.irq_pending) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        }
    }
}

static void mcs251_cpu_init(Object *obj)
{
    MCS251CPU *cpu = MCS251_CPU(obj);

    qdev_init_gpio_in(DEVICE(cpu), mcs251_cpu_set_irq, MCS251_NUM_IRQS);
}

static void mcs251_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUMCS251State *env = cpu_env(cs);
    int i;

    qemu_fprintf(f, "PC=%06x SPX=%08x DPX=%08x PSW=%02x PSW1=%02x "
                 "mode=%s\n",
                 env->pc, mcs251_cpu_get_reg(env, 60, 4),
                 mcs251_cpu_get_reg(env, 56, 4), mcs251_cpu_get_psw(env),
                 mcs251_cpu_get_psw1(env),
                 env->auxr2 & 0x40 ? "binary" : "source");
    for (i = 0; i < 32; i += 8) {
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
}

static void mcs251_cpu_disas_set_info(const CPUState *cs,
                                      disassemble_info *info)
{
    info->endian = BFD_ENDIAN_BIG;
    info->print_insn = mcs251_print_insn;
}

hwaddr mcs251_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    return addr & MCS251_ADDR_MASK;
}

bool mcs251_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                         MMUAccessType access_type, int mmu_idx,
                         bool probe, uintptr_t retaddr)
{
    CPUMCS251State *env = cpu_env(cs);
    hwaddr vpage = address & TARGET_PAGE_MASK;
    hwaddr ppage = vpage;
    int prot = vpage < 0x800000 ?
               PAGE_READ | PAGE_WRITE :
               PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    if (access_type == MMU_INST_FETCH && vpage < 0x800000) {
        prot = 0;
    } else if (vpage >= 0x7e0000 && vpage < 0x7f0000 &&
        !(env->p_sw2 & 0x80)) {
        prot = 0;
    } else if (vpage >= 0x7f0000 && vpage < 0x800000 &&
               !(env->auxr & 0x02)) {
        prot = 0;
    } else if (vpage >= 0x030000 && vpage < 0x031000) {
        if (env->ckcon & 0x80) {
            prot = 0;
        } else {
            prot = PAGE_READ | PAGE_WRITE;
        }
    } else if (vpage >= 0x800000 && vpage < 0x801000) {
        if (env->ckcon & 0x80) {
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
    cc->gdb_core_xml_file = "mcs251-core.xml";
    cc->tcg_ops = &mcs251_tcg_ops;
    dc->vmsd = &vms_mcs251_cpu;
}

static const TypeInfo mcs251_cpu_types[] = {
    {
        .name = TYPE_MCS251_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(MCS251CPU),
        .instance_align = __alignof(MCS251CPU),
        .instance_init = mcs251_cpu_init,
        .class_size = sizeof(MCS251CPUClass),
        .class_init = mcs251_cpu_class_init,
        .abstract = true,
    }, {
        .name = TYPE_STC32G_CPU,
        .parent = TYPE_MCS251_CPU,
    },
};

DEFINE_TYPES(mcs251_cpu_types)

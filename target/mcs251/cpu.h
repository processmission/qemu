/*
 * STC32G MCS-251 CPU definition
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MCS251_CPU_H
#define TARGET_MCS251_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "system/memory.h"

#ifdef CONFIG_USER_ONLY
#error "MCS-251 does not support user-mode emulation"
#endif

#define CPU_RESOLVING_TYPE TYPE_MCS251_CPU

#define MCS251_ADDR_MASK 0x00ffffffu
#define MCS251_RESET_PC 0x00ff0000u
#define MCS251_SFR_PHYS_BASE 0x01000000u
#define MCS251_DISABLED_PHYS_BASE 0x01000400u

#define MCS251_NUM_REG_POSITIONS 64
#define MCS251_NUM_IRQS 5
#define MCS251_MAX_IRQ_DEPTH 8

enum {
    MCS251_IRQ_INT0,
    MCS251_IRQ_TIMER0,
    MCS251_IRQ_INT1,
    MCS251_IRQ_TIMER1,
    MCS251_IRQ_UART1,
};

typedef struct CPUArchState {
    uint32_t pc;

    /*
     * Each entry stores one byte. Positions 0-7 are architectural aliases
     * of the currently selected edata register bank and are accessed through
     * mcs251_cpu_get_reg8()/mcs251_cpu_set_reg8().
     */
    uint32_t regs[MCS251_NUM_REG_POSITIONS];
    uint32_t dptr[2];

    uint32_t flag_c;
    uint32_t flag_ac;
    uint32_t flag_ov;
    uint32_t flag_n;
    uint32_t flag_z;
    uint32_t flag_f0;
    uint32_t flag_f1;
    uint32_t reg_bank;

    uint32_t pcon;
    uint32_t auxr;
    uint32_t intclko;
    uint32_t auxr2;
    uint32_t p_sw2;
    uint32_t dps;
    uint32_t ckcon;
    uint32_t mxax;
    uint32_t ta_stage;

    uint32_t tcon;
    uint32_t ie;
    uint32_t ip;
    uint32_t iph;

    uint32_t irq_pending;
    uint32_t irq_ack;
    uint32_t irq_level;
    uint32_t irq_depth;
    uint32_t irq_level_stack[MCS251_MAX_IRQ_DEPTH];
    bool direct_rmw;
    bool ta_touched;
    bool timer0_mode3;
    bool timer0_mode3_armed;
} CPUMCS251State;

struct ArchCPU {
    CPUState parent_obj;

    CPUMCS251State env;
    MemoryRegion sfr;
    MemoryRegion disabled;
};

struct MCS251CPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

extern const VMStateDescription vms_mcs251_cpu;

uint8_t mcs251_cpu_get_reg8(CPUMCS251State *env, unsigned reg);
void mcs251_cpu_set_reg8(CPUMCS251State *env, unsigned reg, uint8_t value);
uint32_t mcs251_cpu_get_reg(CPUMCS251State *env, unsigned reg,
                           unsigned bytes);
void mcs251_cpu_set_reg(CPUMCS251State *env, unsigned reg, unsigned bytes,
                        uint32_t value);

uint8_t mcs251_cpu_get_psw(CPUMCS251State *env);
void mcs251_cpu_set_psw(CPUMCS251State *env, uint8_t value);
uint8_t mcs251_cpu_get_psw1(CPUMCS251State *env);
void mcs251_cpu_set_psw1(CPUMCS251State *env, uint8_t value);

uint8_t mcs251_cpu_direct_read(CPUMCS251State *env, uint8_t addr);
uint8_t mcs251_cpu_direct_rmw_read(CPUMCS251State *env, uint8_t addr);
void mcs251_cpu_direct_write(CPUMCS251State *env, uint8_t addr,
                             uint8_t value);

bool mcs251_cpu_exec_interrupt(CPUState *cs, int interrupt_request);
void mcs251_cpu_do_interrupt(CPUState *cs);
bool mcs251_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                         MMUAccessType access_type, int mmu_idx,
                         bool probe, uintptr_t retaddr);
hwaddr mcs251_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr);

void mcs251_translate_init(void);
void mcs251_translate_code(CPUState *cs, TranslationBlock *tb,
                           int *max_insns, vaddr pc, void *host_pc);

int mcs251_cpu_gdb_read_register(CPUState *cs, GByteArray *buf, int reg);
int mcs251_cpu_gdb_write_register(CPUState *cs, uint8_t *buf, int reg);
int mcs251_print_insn(bfd_vma addr, disassemble_info *info);

#endif

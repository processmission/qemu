/*
 * MCS-51 family CPU definition
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MCS51_CPU_H
#define TARGET_MCS51_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "hw/core/registerfields.h"
#include "system/memory.h"

#ifdef CONFIG_USER_ONLY
#error "MCS-51 family does not support user-mode emulation"
#endif

#define CPU_RESOLVING_TYPE TYPE_MCS51_CPU

#define MCS51_ADDR_MASK 0x0000ffffu
#define MCS51_RESET_PC 0x00000000u
#define MCS51_MMU_CODE_IDX 0
#define MCS51_MMU_DATA_IDX 1
#define MCS51_CODE_PHYS_BASE 0x00000000u
#define MCS51_IDATA_PHYS_BASE 0x00800000u
#define MCS51_XDATA_PHYS_BASE 0x00810000u
#define MCS51_XFR_PHYS_BASE 0x00c00000u
#define MCS51_XFR_VIRT_BASE 0x0000fa00u
#define MCS51_SFR_PHYS_BASE 0x01000000u
#define MCS51_DISABLED_PHYS_BASE 0x01000400u
#define MCS251_ADDR_MASK 0x00ffffffu
#define MCS251_RESET_PC 0x00ff0000u
#define MCS251_SFR_PHYS_BASE MCS51_SFR_PHYS_BASE
#define MCS251_DISABLED_PHYS_BASE MCS51_DISABLED_PHYS_BASE

#define MCS251_NUM_REG_POSITIONS 64
#define MCS251_NUM_IRQS 64
#define MCS251_MAX_IRQ_DEPTH 8
#define MCS251_MAX_SFR_WRITE_NOTIFIERS 5
#define MCS251_TB_FLAG_BINARY BIT(0)
#define MCS251_OPCODE_ESCAPE 0xa5
#define MCS251_DPTR_DR_CODE 14

enum MCS251DisassemblerMode {
    MCS251_DISAS_SOURCE,
    MCS251_DISAS_BINARY,
    MCS251_DISAS_MCS51,
};

enum MCS251NativeOperandMode {
    MCS251_MODE_BYTE_IMMEDIATE = 0x0,
    MCS251_MODE_BYTE_DIRECT8 = 0x1,
    MCS251_MODE_BYTE_DIRECT16 = 0x3,
    MCS251_MODE_WORD_IMMEDIATE = 0x4,
    MCS251_MODE_WORD_DIRECT8 = 0x5,
    MCS251_MODE_WORD_DIRECT16 = 0x7,
    MCS251_MODE_DWORD_ZERO_IMMEDIATE = 0x8,
    MCS251_MODE_WORD_INDIRECT_WR = 0x8,
    MCS251_MODE_BYTE_INDIRECT_WR = 0x9,
    MCS251_MODE_WORD_INDIRECT_DR = 0xa,
    MCS251_MODE_BYTE_INDIRECT_DR = 0xb,
    MCS251_MODE_DWORD_SIGNED_IMMEDIATE = 0xc,
    MCS251_MODE_MOVH = 0xc,
    MCS251_MODE_DWORD_DIRECT8 = 0xd,
    MCS251_MODE_DWORD_DIRECT16 = 0xf,
};

enum MCS251NativeBitOperation {
    MCS251_BIT_JBC = 0x02,
    MCS251_BIT_JB = 0x04,
    MCS251_BIT_JNB = 0x06,
    MCS251_BIT_ORL = 0x0e,
    MCS251_BIT_ANL = 0x10,
    MCS251_BIT_MOV_FROM_C = 0x12,
    MCS251_BIT_MOV_TO_C = 0x14,
    MCS251_BIT_CPL = 0x16,
    MCS251_BIT_CLR = 0x18,
    MCS251_BIT_SETB = 0x1a,
    MCS251_BIT_ORL_NOT = 0x1c,
    MCS251_BIT_ANL_NOT = 0x1e,
};

enum MCS251NativeRegisterWidth {
    MCS251_WIDTH_BYTE = 0,
    MCS251_WIDTH_WORD = 1,
    MCS251_WIDTH_DWORD = 3,
};

enum MCS251NativeIncrementStep {
    MCS251_INCREMENT_STEP_RESERVED = 3,
};

enum MCS251NativeControlMode {
    MCS251_CONTROL_NEAR_WR = 4,
    MCS251_CONTROL_EXTENDED_DR = 8,
};

enum MCS251NativeStackMode {
    MCS251_STACK_BYTE_REGISTER = 8,
    MCS251_STACK_WORD_REGISTER = 9,
    MCS251_STACK_DWORD_REGISTER = 11,
};

enum {
    MCS251_REG_BANK_FIRST,
    MCS251_REG_BANK_COUNT = 8,
    MCS251_REG_GENERAL_FIRST = 0,
    MCS251_REG_GENERAL_COUNT = 32,
    MCS251_REG_B = 10,
    MCS251_REG_ACC = 11,
    MCS251_REG_DPTR_FIRST = 56,
    MCS251_REG_DPTR_COUNT = 4,
    MCS251_REG_DPXL = 57,
    MCS251_REG_DPH = 58,
    MCS251_REG_DPL = 59,
    MCS251_REG_SPX = 60,
    MCS251_REG_SPH = 62,
    MCS251_REG_SP = 63,
};

enum {
    MCS251_SFR_BASE = 0x80,
    MCS251_SFR_P0 = 0x80,
    MCS251_SFR_SP = 0x81,
    MCS251_SFR_DPL = 0x82,
    MCS251_SFR_DPH = 0x83,
    MCS251_SFR_DPXL = 0x84,
    MCS251_SFR_SPH = 0x85,
    MCS251_SFR_PCON = 0x87,
    MCS251_SFR_TCON = 0x88,
    MCS251_SFR_AUXR = 0x8e,
    MCS251_SFR_INTCLKO = 0x8f,
    MCS251_SFR_AUXR2 = 0x97,
    MCS251_SFR_P2 = 0xa0,
    MCS251_SFR_IE = 0xa8,
    MCS251_SFR_TA = 0xae,
    MCS251_SFR_IPH = 0xb7,
    MCS251_SFR_IP = 0xb8,
    MCS251_SFR_P_SW2 = 0xba,
    MCS251_SFR_PSW = 0xd0,
    MCS251_SFR_PSW1 = 0xd1,
    MCS251_SFR_ACC = 0xe0,
    MCS251_SFR_DPS = 0xe3,
    MCS251_SFR_DPL1 = 0xe4,
    MCS251_SFR_DPH1 = 0xe5,
    MCS251_SFR_CKCON = 0xea,
    MCS251_SFR_MXAX = 0xeb,
    MCS251_SFR_B = 0xf0,
};

#define MCS251_TA_FIRST_KEY 0xaa
#define MCS251_TA_SECOND_KEY 0x55

enum {
    MCS251_TA_STAGE_LOCKED,
    MCS251_TA_STAGE_FIRST_KEY,
    MCS251_TA_STAGE_UNLOCKED,
};

FIELD(PSW, P, 0, 1)
FIELD(PSW, F1, 1, 1)
FIELD(PSW, OV, 2, 1)
FIELD(PSW, RS, 3, 2)
FIELD(PSW, F0, 5, 1)
FIELD(PSW, AC, 6, 1)
FIELD(PSW, C, 7, 1)

FIELD(PSW1, Z, 1, 1)
FIELD(PSW1, OV, 2, 1)
FIELD(PSW1, RS, 3, 2)
FIELD(PSW1, N, 5, 1)
FIELD(PSW1, AC, 6, 1)
FIELD(PSW1, C, 7, 1)

FIELD(TCON, IT0, 0, 1)
FIELD(TCON, IE0, 1, 1)
FIELD(TCON, IT1, 2, 1)
FIELD(TCON, IE1, 3, 1)
FIELD(TCON, TR0, 4, 1)
FIELD(TCON, TF0, 5, 1)
FIELD(TCON, TR1, 6, 1)
FIELD(TCON, TF1, 7, 1)

FIELD(AUXR, RAMEXE, 1, 1)
FIELD(AUXR, EXTRAM, 1, 1)
FIELD(AUXR, T1X12, 6, 1)
FIELD(AUXR, T0X12, 7, 1)
FIELD(AUXR2, CPUMODE, 6, 1)
FIELD(PCON, IDL, 0, 1)
FIELD(PCON, PD, 1, 1)
FIELD(P_SW2, EAXFR, 7, 1)
FIELD(CKCON, EAXRAM, 7, 1)

FIELD(IE, EX0, 0, 1)
FIELD(IE, ET0, 1, 1)
FIELD(IE, EX1, 2, 1)
FIELD(IE, ET1, 3, 1)
FIELD(IE, ES, 4, 1)
FIELD(IE, EADC, 5, 1)
FIELD(IE, ELVD, 6, 1)
FIELD(IE, EA, 7, 1)

FIELD(IP, PX0, 0, 1)
FIELD(IP, PT0, 1, 1)
FIELD(IP, PX1, 2, 1)
FIELD(IP, PT1, 3, 1)
FIELD(IP, PS, 4, 1)
FIELD(IP, PADC, 5, 1)
FIELD(IP, PLVD, 6, 1)
FIELD(IP, PPCA, 7, 1)

FIELD(DPS, SEL, 0, 1)
FIELD(DPS, AU0, 3, 1)
FIELD(DPS, AU1, 4, 1)
FIELD(DPS, TSL, 5, 1)
FIELD(DPS, ID0, 6, 1)
FIELD(DPS, ID1, 7, 1)

FIELD(MCS251_OPCODE, RN, 0, 3)
FIELD(MCS251_OPCODE, RI, 0, 1)
FIELD(MCS251_OPCODE, WIDTH, 0, 2)
FIELD(MCS251_OPCODE, LOW_NIBBLE, 0, 4)
FIELD(MCS251_OPCODE, LOW5, 0, 5)
FIELD(MCS251_OPCODE, GROUP5, 3, 5)
FIELD(MCS251_OPCODE, CLASS6, 2, 6)
FIELD(MCS251_OPCODE, CLASS7, 1, 7)
FIELD(MCS251_OPCODE, HIGH_NIBBLE, 4, 4)
FIELD(MCS251_OPCODE, PAGE, 5, 3)
FIELD(MCS251_OPCODE, NEGATE, 5, 1)
FIELD(MCS251_OPCODE, STORE, 4, 1)
FIELD(MCS251_OPCODE, LONG_POINTER, 5, 1)
FIELD(MCS251_OPCODE, WIDE, 6, 1)

FIELD(MCS251_SPECIFIER, MODE, 0, 4)
FIELD(MCS251_SPECIFIER, CODE, 4, 4)
FIELD(MCS251_BIT_SPECIFIER, BIT, 0, 3)
FIELD(MCS251_BIT_SPECIFIER, OPERATION, 3, 5)
FIELD(MCS251_INCDEC_MODE, STEP, 0, 2)
FIELD(MCS251_INCDEC_MODE, WIDTH, 2, 2)

#define MCS251_IE_WRITABLE_MASK \
    (R_IE_EX0_MASK | R_IE_ET0_MASK | R_IE_EX1_MASK | R_IE_ET1_MASK | \
     R_IE_ES_MASK | R_IE_EADC_MASK | R_IE_ELVD_MASK | R_IE_EA_MASK)
#define MCS251_IP_WRITABLE_MASK \
    (R_IP_PX0_MASK | R_IP_PT0_MASK | R_IP_PX1_MASK | R_IP_PT1_MASK | \
     R_IP_PS_MASK | R_IP_PADC_MASK | R_IP_PLVD_MASK | R_IP_PPCA_MASK)
#define MCS251_DPS_WRITABLE_MASK \
    (R_DPS_SEL_MASK | R_DPS_AU0_MASK | R_DPS_AU1_MASK | R_DPS_TSL_MASK | \
     R_DPS_ID0_MASK | R_DPS_ID1_MASK)

enum {
    MCS251_IRQ_INT0,
    MCS251_IRQ_TIMER0,
    MCS251_IRQ_INT1,
    MCS251_IRQ_TIMER1,
    MCS251_IRQ_UART1,
    MCS251_IRQ_ADC,
    MCS251_IRQ_LVD,
    MCS251_IRQ_PCA,
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
    uint32_t p2;
    uint32_t p_sw2;
    uint32_t dps;
    uint32_t ckcon;
    uint32_t mxax;
    uint32_t ta_stage;

    uint32_t tcon;
    uint32_t ie;
    uint32_t ip;
    uint32_t iph;

    uint64_t irq_pending;
    uint32_t irq_ack;
    uint32_t irq_level;
    uint32_t irq_depth;
    uint32_t irq_level_stack[MCS251_MAX_IRQ_DEPTH];
    bool direct_rmw;
    bool ta_touched;
    bool timer0_mode3;
    bool timer0_mode3_armed;
} CPUMCS251State;

typedef void (*MCS251SFRImmediateWrite)(void *opaque, uint8_t addr,
                                       uint8_t value);
typedef void (*MCS251SFRWriteNotifier)(void *opaque, uint8_t addr,
                                       uint8_t value);

struct ArchCPU {
    CPUState parent_obj;

    CPUMCS251State env;
    MemoryRegion sfr;
    MemoryRegion disabled;
    MCS251SFRImmediateWrite sfr_immediate_write;
    void *sfr_immediate_opaque;
    uint32_t irq_vector[MCS251_NUM_IRQS];
    uint8_t irq_enabled[MCS251_NUM_IRQS];
    uint8_t irq_priority[MCS251_NUM_IRQS];
    uint8_t irq_auto_clear[MCS251_NUM_IRQS];
    MCS251SFRWriteNotifier sfr_write_notifier[
        MCS251_MAX_SFR_WRITE_NOTIFIERS];
    void *sfr_write_notifier_opaque[MCS251_MAX_SFR_WRITE_NOTIFIERS];
    unsigned sfr_write_notifier_count;
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
void mcs251_cpu_direct_write_immediate(CPUMCS251State *env, uint8_t addr,
                                       uint8_t value);
void mcs251_cpu_set_sfr_immediate_write(MCS251CPU *cpu,
                                        MCS251SFRImmediateWrite callback,
                                        void *opaque);
void mcs251_cpu_add_sfr_write_notifier(MCS251CPU *cpu,
                                       MCS251SFRWriteNotifier callback,
                                       void *opaque);
void mcs251_cpu_notify_sfr_write(MCS251CPU *cpu, uint8_t addr,
                                 uint8_t value);
void mcs251_cpu_configure_irq(MCS251CPU *cpu, unsigned irq,
                              uint32_t vector, unsigned priority,
                              bool enabled, bool auto_clear);
void mcs251_cpu_sync_irq_configuration(MCS251CPU *cpu);

bool mcs251_cpu_has_interrupt(CPUState *cs);
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

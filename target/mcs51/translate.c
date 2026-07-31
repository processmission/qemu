/*
 * MCS-51 family TCG translation
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "internals.h"
#include "decode.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translation-block.h"
#include "exec/translator.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPUMCS251State *env;
    MCS251DecodedInsn decoded;
} DisasContext;

typedef struct arg_opcode {
    int padding;
} arg_opcode;

static bool decode_mcs51(DisasContext *ctx, uint16_t insn);
#include "decode-mcs51.c.inc"

static bool decode_mcs251(DisasContext *ctx, uint16_t insn);
#include "decode-mcs251.c.inc"

void mcs251_translate_init(void)
{
}

static bool mcs251_translate_read(void *opaque, uint32_t address,
                                  uint8_t *value)
{
    DisasContext *ctx = opaque;

    *value = translator_ldub(ctx->env, &ctx->base, address);
    return true;
}

static bool mcs251_trans_execute(DisasContext *ctx)
{
    gen_helper_mcs251_execute(
        tcg_env, tcg_constant_i32(ctx->decoded.first_opcode));
    return true;
}

#define TRANS_EXECUTE(NAME)                                              \
    static bool trans_##NAME(DisasContext *ctx, arg_opcode a[1])         \
    {                                                                    \
        (void)a;                                                         \
        return mcs251_trans_execute(ctx);                                \
    }

TRANS_EXECUTE(BIN_NOP)
TRANS_EXECUTE(BIN_AJMP)
TRANS_EXECUTE(BIN_LJMP)
TRANS_EXECUTE(BIN_RR)
TRANS_EXECUTE(BIN_INC)
TRANS_EXECUTE(BIN_JBC)
TRANS_EXECUTE(BIN_ACALL)
TRANS_EXECUTE(BIN_LCALL)
TRANS_EXECUTE(BIN_RRC)
TRANS_EXECUTE(BIN_DEC)
TRANS_EXECUTE(BIN_JB)
TRANS_EXECUTE(BIN_RET)
TRANS_EXECUTE(BIN_RL)
TRANS_EXECUTE(BIN_ADD)
TRANS_EXECUTE(BIN_JNB)
TRANS_EXECUTE(BIN_RETI)
TRANS_EXECUTE(BIN_RLC)
TRANS_EXECUTE(BIN_ADDC)
TRANS_EXECUTE(BIN_JC)
TRANS_EXECUTE(BIN_ORL)
TRANS_EXECUTE(BIN_JNC)
TRANS_EXECUTE(BIN_ANL)
TRANS_EXECUTE(BIN_JZ)
TRANS_EXECUTE(BIN_XRL)
TRANS_EXECUTE(BIN_JNZ)
TRANS_EXECUTE(BIN_ORL_C)
TRANS_EXECUTE(BIN_JMP)
TRANS_EXECUTE(BIN_MOV)
TRANS_EXECUTE(BIN_SJMP)
TRANS_EXECUTE(BIN_ANL_C)
TRANS_EXECUTE(BIN_MOVC)
TRANS_EXECUTE(BIN_DIV)
TRANS_EXECUTE(BIN_SUBB)
TRANS_EXECUTE(BIN_MUL)
TRANS_EXECUTE(BIN_ESC)
TRANS_EXECUTE(BIN_CPL)
TRANS_EXECUTE(BIN_CJNE)
TRANS_EXECUTE(BIN_PUSH)
TRANS_EXECUTE(BIN_CLR)
TRANS_EXECUTE(BIN_SWAP)
TRANS_EXECUTE(BIN_XCH)
TRANS_EXECUTE(BIN_POP)
TRANS_EXECUTE(BIN_SETB)
TRANS_EXECUTE(BIN_DA)
TRANS_EXECUTE(BIN_DJNZ)
TRANS_EXECUTE(BIN_XCHD)
TRANS_EXECUTE(BIN_MOVX)
TRANS_EXECUTE(BIN_RESERVED)

TRANS_EXECUTE(SRC_BRANCH)
TRANS_EXECUTE(SRC_DISPLACEMENT)
TRANS_EXECUTE(SRC_EXTEND)
TRANS_EXECUTE(SRC_INCDEC)
TRANS_EXECUTE(SRC_SHIFT)
TRANS_EXECUTE(SRC_ADD)
TRANS_EXECUTE(SRC_ORL)
TRANS_EXECUTE(SRC_ANL)
TRANS_EXECUTE(SRC_XRL)
TRANS_EXECUTE(SRC_MOV)
TRANS_EXECUTE(SRC_INDIRECT_JUMP)
TRANS_EXECUTE(SRC_EJMP)
TRANS_EXECUTE(SRC_DIV)
TRANS_EXECUTE(SRC_ECALL)
TRANS_EXECUTE(SRC_SUB)
TRANS_EXECUTE(SRC_BIT)
TRANS_EXECUTE(SRC_ERET)
TRANS_EXECUTE(SRC_MUL)
TRANS_EXECUTE(SRC_TRAP)
TRANS_EXECUTE(SRC_CMP)
TRANS_EXECUTE(SRC_PUSH)
TRANS_EXECUTE(SRC_POP)
TRANS_EXECUTE(SRC_RESERVED)

#undef TRANS_EXECUTE

static void mcs251_tr_init_disas_context(DisasContextBase *db,
                                         CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

    ctx->env = cpu_env(cs);
    ctx->base.max_insns = 1;
}

static void mcs251_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
}

static void mcs251_tr_insn_start(DisasContextBase *db, CPUState *cs)
{
    tcg_gen_insn_start(db->pc_next, 0, 0);
}

static void mcs251_tr_translate_insn(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);
    bool decoded;

#ifndef TARGET_MCS251
    decoded = mcs251_decode_insn(db->pc_next, false, false,
                                mcs251_translate_read, ctx,
                                &ctx->decoded);
#else
    decoded = mcs251_decode_insn(
        db->pc_next,
        !(db->tb->flags & MCS251_TB_FLAG_BINARY),
        true,
        mcs251_translate_read, ctx, &ctx->decoded);
#endif
    g_assert(decoded);
    db->pc_next = (db->pc_next + ctx->decoded.length) &
                  MCS_TARGET_ADDR_MASK;

    if (ctx->decoded.source_mode &&
        FIELD_EX8(ctx->decoded.opcode,
                  MCS251_OPCODE, LOW_NIBBLE) > 5) {
        decoded = decode_mcs251(ctx, ctx->decoded.opcode);
    } else {
        decoded = decode_mcs51(ctx, ctx->decoded.opcode);
    }
    g_assert(decoded);
    db->is_jmp = DISAS_NORETURN;
}

static void mcs251_tr_tb_stop(DisasContextBase *db, CPUState *cs)
{
    tcg_gen_exit_tb(NULL, 0);
}

static const TranslatorOps mcs251_tr_ops = {
    .init_disas_context = mcs251_tr_init_disas_context,
    .tb_start = mcs251_tr_tb_start,
    .insn_start = mcs251_tr_insn_start,
    .translate_insn = mcs251_tr_translate_insn,
    .tb_stop = mcs251_tr_tb_stop,
};

void mcs251_translate_code(CPUState *cs, TranslationBlock *tb,
                           int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc = { };

    translator_loop(cs, tb, max_insns, pc, host_pc, &mcs251_tr_ops, &dc.base,
                    TCG_TYPE_VA);
}

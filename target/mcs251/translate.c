/*
 * STC32G MCS-251 TCG translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPUMCS251State *env;
} DisasContext;

void mcs251_translate_init(void)
{
}

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
    gen_helper_mcs251_execute(tcg_env);
    /*
     * The first implementation executes one architectural instruction in
     * the helper. Advancing by one byte here gives the TB a non-zero code
     * range; the architectural PC is updated by the helper itself.
     */
    db->pc_next++;
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

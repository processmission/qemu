/*
 * QTest for the NXP S32K5 board machine model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define S32K5_MACHINE "s32k566-cvb-r52"

#define S32K5_MC_ME_BASE 0x40498000ULL
#define S32K5_LPUART0_BASE 0x400e0000ULL
#define S32K5_ARCH_TIMER_FREQ 4000000ULL

#define MC_ME_CTL_KEY 0x000
#define MC_ME_MODE_CONF 0x004
#define MC_ME_MODE_UPD 0x008
#define MC_ME_MODE_STAT 0x00c
#define MC_ME_PRTN2_PUPD 0x504
#define MC_ME_PRTN2_COFB1_STAT 0x514
#define MC_ME_PRTN2_COFB1_CLKEN 0x534

#define MC_ME_CTL_KEY_DIRECT 0x5af0
#define MC_ME_CTL_KEY_INVERTED 0xa50f
#define MC_ME_MODE_UPD_MODE_UPD (1U << 0)
#define MC_ME_PRTN2_PUPD_PCUD (1U << 0)
#define MC_ME_SWT_STARTUP_REQ (1U << 10)

#define LPUART_VERID 0x00
#define LPUART_BAUD  0x10
#define LPUART_STAT  0x14
#define LPUART_CTRL  0x18
#define LPUART_DATA  0x1c
#define LPUART_FIFO  0x28

#define LPUART_STAT_TC   (1U << 22)
#define LPUART_STAT_TDRE (1U << 23)
#define LPUART_CTRL_TIE  (1U << 23)
#define LPUART_FIFO_RXEMPT (1U << 22)
#define LPUART_FIFO_TXEMPT (1U << 23)

#define LPUART_STAT_TX_READY (LPUART_STAT_TDRE | LPUART_STAT_TC)
#define LPUART_FIFO_EMPTY (LPUART_FIFO_TXEMPT | LPUART_FIFO_RXEMPT)

static QTestState *s32k5_qtest_start(void)
{
    return qtest_init("-machine " S32K5_MACHINE " -serial null");
}

static char *s32k5_get_first_cpu_path(QTestState *qts)
{
    QDict *resp;
    QList *cpus;
    QObject *cpu_obj;
    const QDict *cpu;
    char *path;

    resp = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(qdict_haskey(resp, "return"));
    cpus = qdict_get_qlist(resp, "return");
    g_assert(cpus);

    cpu_obj = qlist_pop(cpus);
    g_assert(cpu_obj);
    cpu = qobject_to(QDict, cpu_obj);
    g_assert(qdict_haskey(cpu, "qom-path"));
    path = g_strdup(qdict_get_str(cpu, "qom-path"));

    qobject_unref(cpu_obj);
    qobject_unref(resp);

    return path;
}

static uint64_t s32k5_qom_get_uint(QTestState *qts, const char *path,
                                   const char *property)
{
    QDict *resp;
    uint64_t value;

    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                          "{ 'path': %s, 'property': %s } }",
                     path, property);
    g_assert(qdict_haskey(resp, "return"));
    value = qdict_get_uint(resp, "return");
    qobject_unref(resp);

    return value;
}

static uint32_t mc_me_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, S32K5_MC_ME_BASE + offset);
}

static void mc_me_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, S32K5_MC_ME_BASE + offset, value);
}

static uint32_t lpuart_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, S32K5_LPUART0_BASE + offset);
}

static void lpuart_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, S32K5_LPUART0_BASE + offset, value);
}

static void test_machine_constructs(void)
{
    QTestState *qts = s32k5_qtest_start();

    qtest_quit(qts);
}

static void test_cpu_timer_frequency(void)
{
    QTestState *qts = s32k5_qtest_start();
    char *cpu_path = s32k5_get_first_cpu_path(qts);

    g_assert_cmpuint(s32k5_qom_get_uint(qts, cpu_path, "cntfrq"), ==,
                     S32K5_ARCH_TIMER_FREQ);

    g_free(cpu_path);
    qtest_quit(qts);
}

static void test_mc_me_partition_update(void)
{
    QTestState *qts = s32k5_qtest_start();

    mc_me_writel(qts, MC_ME_PRTN2_COFB1_CLKEN, MC_ME_SWT_STARTUP_REQ);
    mc_me_writel(qts, MC_ME_PRTN2_PUPD, MC_ME_PRTN2_PUPD_PCUD);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_PRTN2_PUPD), ==,
                    MC_ME_PRTN2_PUPD_PCUD);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_PRTN2_COFB1_STAT), ==, 0);

    mc_me_writel(qts, MC_ME_CTL_KEY, MC_ME_CTL_KEY_DIRECT);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_PRTN2_PUPD), ==,
                    MC_ME_PRTN2_PUPD_PCUD);

    mc_me_writel(qts, MC_ME_CTL_KEY, MC_ME_CTL_KEY_INVERTED);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_PRTN2_PUPD), ==, 0);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_PRTN2_COFB1_STAT), ==,
                    MC_ME_SWT_STARTUP_REQ);

    qtest_quit(qts);
}

static void test_mc_me_mode_update(void)
{
    QTestState *qts = s32k5_qtest_start();

    mc_me_writel(qts, MC_ME_MODE_CONF, 1);
    mc_me_writel(qts, MC_ME_MODE_UPD, MC_ME_MODE_UPD_MODE_UPD);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_MODE_UPD), ==,
                    MC_ME_MODE_UPD_MODE_UPD);

    mc_me_writel(qts, MC_ME_CTL_KEY, MC_ME_CTL_KEY_DIRECT);
    mc_me_writel(qts, MC_ME_CTL_KEY, MC_ME_CTL_KEY_INVERTED);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_MODE_UPD), ==, 0);
    g_assert_cmphex(mc_me_readl(qts, MC_ME_MODE_STAT), ==, 1);

    qtest_quit(qts);
}

static void test_lpuart_reset_values(void)
{
    QTestState *qts = s32k5_qtest_start();

    g_assert_cmphex(lpuart_readl(qts, LPUART_VERID), ==, 0x04010003);
    g_assert_cmphex(lpuart_readl(qts, LPUART_BAUD), ==, 0x0f000004);
    g_assert_cmphex(lpuart_readl(qts, LPUART_STAT) & LPUART_STAT_TX_READY,
                    ==, LPUART_STAT_TX_READY);
    g_assert_cmphex(lpuart_readl(qts, LPUART_FIFO) & LPUART_FIFO_EMPTY,
                    ==, LPUART_FIFO_EMPTY);

    qtest_quit(qts);
}

static void test_lpuart_read_only_bits(void)
{
    QTestState *qts = s32k5_qtest_start();

    lpuart_writel(qts, LPUART_VERID, 0);
    g_assert_cmphex(lpuart_readl(qts, LPUART_VERID), ==, 0x04010003);

    lpuart_writel(qts, LPUART_STAT, 0);
    g_assert_cmphex(lpuart_readl(qts, LPUART_STAT) & LPUART_STAT_TX_READY,
                    ==, LPUART_STAT_TX_READY);

    qtest_quit(qts);
}

static void test_lpuart_data_write_sets_tx_ready(void)
{
    QTestState *qts = s32k5_qtest_start();

    lpuart_writel(qts, LPUART_CTRL, LPUART_CTRL_TIE);
    g_assert_cmphex(lpuart_readl(qts, LPUART_CTRL), ==, LPUART_CTRL_TIE);

    lpuart_writel(qts, LPUART_DATA, 'A');
    g_assert_cmphex(lpuart_readl(qts, LPUART_STAT) & LPUART_STAT_TX_READY,
                    ==, LPUART_STAT_TX_READY);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/s32k5/machine/constructs", test_machine_constructs);
    qtest_add_func("/s32k5/cpu/timer-frequency", test_cpu_timer_frequency);
    qtest_add_func("/s32k5/mc-me/partition-update",
                   test_mc_me_partition_update);
    qtest_add_func("/s32k5/mc-me/mode-update", test_mc_me_mode_update);
    qtest_add_func("/s32k5/lpuart/reset", test_lpuart_reset_values);
    qtest_add_func("/s32k5/lpuart/read-only", test_lpuart_read_only_bits);
    qtest_add_func("/s32k5/lpuart/data-write",
                   test_lpuart_data_write_sets_tx_ready);

    return g_test_run();
}

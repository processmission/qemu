/*
 * QTest testcase for K230 sysctl blocks
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define K230_RMU_BASE  0x91101000
#define K230_BOOT_BASE 0x91102000
#define K230_FLASH_BASE 0xc0000000
#define K230_KPU_CFG_BASE 0x80400000
#define K230_CLINT_BASE 0xf04000000ULL
#define K230_PLIC_BASE 0xf00000000ULL
#define K230_GSDMA_BASE 0x80800000
#define K230_FFT_BASE 0x80400800
#define K230_AI_2D_BASE 0x80400c00
#define K230_NON_AI_2D_BASE 0x8080c000
#define K230_ISP_BASE 0x90000000
#define K230_DEWARP_BASE 0x90008000
#define K230_RX_CSI_BASE 0x90009000
#define K230_DPU_BASE 0x90a00000
#define K230_UART3_BASE 0x91403000
#define K230_I2C0_BASE 0x91405000

#define K230_CPU1_RST_CTL      0x0c
#define K230_CPU1_HART_RSTVEC  0x104
#define K230_CLINT_SSIP0       0xc000
#define K230_CLINT_STIMECMPL0  0xd000
#define K230_CLINT_STIMECMPH0  0xd004
#define K230_I2C_TAR           0x04
#define K230_I2C_DATA_CMD      0x10
#define K230_I2C_ENABLE        0x6c
#define K230_I2C_CMD_READ      (1u << 8)
#define K230_I2C_CMD_STOP      (1u << 9)
#define K230_I2C_CMD_RESTART   (1u << 10)
#define K230_PLIC_ENABLE_S     0x2080
#define K230_PLIC_CONTEXT_S    0x201000
#define K230_PLIC_THRESHOLD    0x00
#define K230_PLIC_CLAIM        0x04
#define K230_ISP_MAIN_STATUS   0x05c4
#define K230_ISP_MAIN_CLEAR    0x05c8
#define K230_ISP_MCM_CTRL      0x1300
#define K230_ISP_MCM0_STATUS   0x16d0
#define K230_ISP_MCM0_CLEAR    0x16d8
#define K230_ISP_MCM2_STATUS   0x56d8
#define K230_ISP_MCM2_CLEAR    0x56dc
#define K230_ISP_TOP_STATUS    0x3d60
#define K230_ISP_FE_START      0x3d64
#define K230_ISP_FE_MI_STATUS  0x3d74
#define K230_ISP_FE_MI_CLEAR   0x3d78
#define K230_ISP_TOP_PENDING   (1u << 0)
#define K230_ISP_TOP_ACK       (1u << 1)
#define K230_ISP_FE_START_CMD  (1u << 16)
#define K230_ISP_MCM_CH0_ENABLE (1u << 6)
#define K230_ISP_MCM_CH1_ENABLE (1u << 7)
#define K230_ISP_MCM_CH2_ENABLE (1u << 17)
#define K230_ISP_MCM_CH2_STATUS (1u << 14)
#define K230_ISP_MCM_CH0_BUFFER (1u << 0)
#define K230_ISP_MCM_CH1_BUFFER (1u << 3)
#define K230_ISP_MCM_CH0_DONE    (1u << 6)
#define K230_ISP_MCM_CH1_DONE    (1u << 7)
#define K230_ISP_MI_FRAME_STATUS (1u << 8)
#define K230_ISP_MCM_FRAME_NS    (1000000000 / 30)
#define K230_DWE_CTRL_LOW       0x004
#define K230_DWE_DMA_START0     0x010
#define K230_DWE_DMA_START1     0x014
#define K230_DWE_IRQ_STATUS_LOW 0x070
#define K230_DWE_BUS_CTRL_LOW   0x074
#define K230_DWE_CTRL           0xc04
#define K230_DWE_IRQ_STATUS     0xc70
#define K230_DWE_IRQ_CLEAR      0xd00
#define K230_DWE_START          (1u << 1)
#define K230_DWE_BUS_ENABLE     (1u << 31)
#define K230_FE_START           0xd04
#define K230_FE_IRQ_STATUS      0xd14
#define K230_FE_IRQ_CLEAR       0xd18
#define K230_FE_START_CMD       (1u << 16)
#define K230_VSE_IRQ_STATUS     0xa50
#define K230_VSE_IRQ_CLEAR      0xa58
#define K230_RX_CSI_HOST2_ENABLE 0x1008
#define K230_RX_CSI_HOST2_PHY_STATE 0x1014
#define K230_RX_CSI_PHY_STOPSTATE (1u << 16)
#define K230_RX_CSI_DIS_FRAME_M  0x008c
#define K230_RX_CSI_DIS_FRAME_N  0x0090
#define K230_RX_CSI_DIS_FRAME_EN 0x0094
#define K230_RX_CSI_PHY_CTRL0    0x0850
#define K230_RX_CSI_PHY_DATA0    0x0854
#define K230_RX_CSI_PHY_CTRL1    0x0858
#define K230_RX_CSI_PHY_DATA1    0x085c
#define K230_GNNE_STATUS      0x130
#define K230_GSDMA_INT_STAT   0x08
#define K230_GSDMA_CH0_CTL    0x50
#define K230_GSDMA_CH0_STATUS 0x54
#define K230_GSDMA_DONE_CH0   (1u << 0)
#define K230_NON_AI_2D_SRC_SIZE      0x000
#define K230_NON_AI_2D_SRC_CH0_ADDR  0x004
#define K230_NON_AI_2D_SRC_CH1_ADDR  0x008
#define K230_NON_AI_2D_SRC_CH2_ADDR  0x00c
#define K230_NON_AI_2D_SRC_STRIDE01  0x010
#define K230_NON_AI_2D_SRC_STRIDE2   0x014
#define K230_NON_AI_2D_FMT           0x018
#define K230_NON_AI_2D_DST_CH0_ADDR  0x13c
#define K230_NON_AI_2D_DST_CH1_ADDR  0x140
#define K230_NON_AI_2D_DST_CH2_ADDR  0x144
#define K230_NON_AI_2D_DST_STRIDE01  0x148
#define K230_NON_AI_2D_DST_STRIDE2   0x14c
#define K230_NON_AI_2D_MAIN_CFG      0x3a0
#define K230_NON_AI_2D_INTR_STATUS   0x3a8
#define K230_NON_AI_2D_INTR_CLEAR    0x3ac
#define K230_NON_AI_2D_STOP_BUSY     0x3b0
#define K230_NON_AI_2D_CALC_EN       (1u << 0)
#define K230_NON_AI_2D_FMT_I420      2
#define K230_NON_AI_2D_MODE_OSD      (1u << 4)
#define K230_NON_AI_2D_INTR_MASK     (1u << 16)
#define K230_DPU_STATUS       0x1f4
#define K230_DPU_IRQ_CLEAR    0x1fc
#define K230_DPU_START        0x200
#define K230_DPU_START_CH1    0x380
#define K230_DPU_DONE         0x3
#define K230_DPU_CLEAR_ACK    0x20002
#define K230_AI2D_CALC_ENABLE 0x80
#define K230_AI2D_JOB         0x8c
#define K230_AI2D_CLEAR       0xa0
#define K230_AI2D_LEGACY_START 0xc0
#define K230_FFT_IRQ           190
#define K230_AI2D_IRQ          191
#define K230_ISP_MI_IRQ        127
#define K230_ISP_FE_IRQ        128
#define K230_ISP_IRQ           129
#define K230_DWE_IRQ           130
#define K230_FE_IRQ            131
#define K230_DMA_IRQ           140
#define K230_NON_AI_2D_IRQ     141
#define K230_DPU_IRQ           186
#define K230_VSE_IRQ           204
#define K230_CPU1_RST_REQ      (1u << 0)
#define K230_CPU1_RST_DONE     (1u << 12)
#define K230_CPU1_PRST_DONE    (1u << 13)
#define K230_CPU1_RST_REQ_WEN  (1u << 16)
#define K230_CPU1_RST_DONE_WEN (1u << 28)
#define K230_CPU1_RST_CTL_RESET \
    (K230_CPU1_PRST_DONE | K230_CPU1_RST_REQ)
#define K230_NON_AI_2D_TEST_SRC_Y    0x01000000
#define K230_NON_AI_2D_TEST_SRC_U    0x01000100
#define K230_NON_AI_2D_TEST_SRC_V    0x01000200
#define K230_NON_AI_2D_TEST_DST_Y    0x01000300
#define K230_NON_AI_2D_TEST_DST_U    0x01000400
#define K230_NON_AI_2D_TEST_DST_V    0x01000500
#define K230_FLASH_TEST_OFFSET       0x6000

static void k230_test_cpu1_reset_sequence(const char *machine_args)
{
    QTestState *qts = qtest_init(machine_args);

    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_RST_CTL_RESET);

    qtest_writel(qts, K230_BOOT_BASE + K230_CPU1_HART_RSTVEC, 0x80200000);

    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_DONE_WEN | K230_CPU1_RST_DONE);
    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_RST_CTL_RESET);

    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_REQ_WEN | K230_CPU1_RST_REQ);
    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_RST_CTL_RESET);

    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_REQ_WEN);
    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_PRST_DONE | K230_CPU1_RST_DONE);

    qtest_quit(qts);
}

static void test_cpu1_reset_sequence_smp1(void)
{
    k230_test_cpu1_reset_sequence("-machine k230-canmv -smp 1");
}

static void test_cpu1_reset_sequence_smp2(void)
{
    k230_test_cpu1_reset_sequence("-machine k230-canmv -smp 2");
}

static QDict *k230_query_cpu(QTestState *qts, int cpu_index)
{
    QDict *resp;
    QList *cpus;
    QListEntry *entry;

    resp = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(qdict_haskey(resp, "return"));
    cpus = qdict_get_qlist(resp, "return");

    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, entry->value);

        if (qdict_get_int(cpu, "cpu-index") == cpu_index) {
            return resp;
        }
    }

    qobject_unref(resp);
    return NULL;
}

static bool k230_qom_get_bool(QTestState *qts, const char *path,
                              const char *property)
{
    QDict *resp;
    bool value;

    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                          "{ 'path': %s, 'property': %s } }",
                          path, property);
    g_assert(qdict_haskey(resp, "return"));
    value = qdict_get_bool(resp, "return");
    qobject_unref(resp);

    return value;
}

static void test_smp1_topology(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv -smp 1");
    QDict *resp = k230_query_cpu(qts, 0);
    QList *cpus;
    const QListEntry *entry;
    QDict *cpu;

    g_assert_nonnull(resp);
    cpus = qdict_get_qlist(resp, "return");
    entry = qlist_first(cpus);
    cpu = qobject_to(QDict, entry->value);
    g_assert_cmpint(qlist_size(cpus), ==, 1);
    g_assert_cmpstr(qdict_get_str(cpu, "qom-type"), ==,
                    "thead-c908-riscv-cpu");

    qobject_unref(resp);
    qtest_quit(qts);
}

static void test_smp2_topology(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv -smp 2");
    QDict *resp = k230_query_cpu(qts, 1);
    QList *cpus;
    QDict *cpu1 = NULL;
    QListEntry *entry;

    g_assert_nonnull(resp);
    cpus = qdict_get_qlist(resp, "return");
    g_assert_cmpint(qlist_size(cpus), ==, 2);
    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, entry->value);

        if (qdict_get_int(cpu, "cpu-index") == 1) {
            cpu1 = cpu;
        }
    }

    g_assert_nonnull(cpu1);
    g_assert_cmpstr(qdict_get_str(cpu1, "qom-type"), ==,
                    "thead-c908v-riscv-cpu");
    g_assert_cmpstr(qdict_get_str(cpu1, "qom-path"), ==,
                    "/machine/soc/c908v-cpu/harts[0]");
    g_assert_true(k230_qom_get_bool(qts, "/machine/soc/c908-cpu/harts[0]",
                                    "start-powered-off"));
    g_assert_false(k230_qom_get_bool(qts, "/machine/soc/c908v-cpu/harts[0]",
                                     "start-powered-off"));

    qobject_unref(resp);
    qtest_quit(qts);
}

static void test_smp2_boot_both_cores_topology(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv,boot-both-cores=on -smp 2");
    QDict *resp = k230_query_cpu(qts, 1);
    QList *cpus;
    QDict *cpu1 = NULL;
    QListEntry *entry;

    g_assert_nonnull(resp);
    cpus = qdict_get_qlist(resp, "return");
    g_assert_cmpint(qlist_size(cpus), ==, 2);
    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, entry->value);

        if (qdict_get_int(cpu, "cpu-index") == 1) {
            cpu1 = cpu;
        }
    }

    g_assert_nonnull(cpu1);
    g_assert_cmpstr(qdict_get_str(cpu1, "qom-type"), ==,
                    "thead-c908v-riscv-cpu");
    g_assert_false(k230_qom_get_bool(qts, "/machine/soc/c908-cpu/harts[0]",
                                     "start-powered-off"));
    g_assert_false(k230_qom_get_bool(qts, "/machine/soc/c908v-cpu/harts[0]",
                                     "start-powered-off"));

    qobject_unref(resp);
    qtest_quit(qts);
}

static void test_clint_smode_regs(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv -smp 1");

    qtest_writel(qts, K230_CLINT_BASE + K230_CLINT_SSIP0, 1);
    g_assert_cmphex(qtest_readl(qts, K230_CLINT_BASE + K230_CLINT_SSIP0), ==,
                    1);

    qtest_writel(qts, K230_CLINT_BASE + K230_CLINT_SSIP0, 0);
    g_assert_cmphex(qtest_readl(qts, K230_CLINT_BASE + K230_CLINT_SSIP0), ==,
                    0);

    qtest_writel(qts, K230_CLINT_BASE + K230_CLINT_STIMECMPL0, 0x11223344);
    qtest_writel(qts, K230_CLINT_BASE + K230_CLINT_STIMECMPH0, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, K230_CLINT_BASE + K230_CLINT_STIMECMPL0),
                    ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, K230_CLINT_BASE + K230_CLINT_STIMECMPH0),
                    ==, 0x55667788);

    qtest_quit(qts);
}

static uint32_t k230_i2c_read_reg8(QTestState *qts, uint64_t base,
                                   uint16_t reg)
{
    qtest_writel(qts, base + K230_I2C_ENABLE, 1);
    qtest_writel(qts, base + K230_I2C_TAR, 0x36);
    qtest_writel(qts, base + K230_I2C_DATA_CMD, reg >> 8);
    qtest_writel(qts, base + K230_I2C_DATA_CMD, reg & 0xff);
    qtest_writel(qts, base + K230_I2C_DATA_CMD,
                 K230_I2C_CMD_READ | K230_I2C_CMD_RESTART |
                 K230_I2C_CMD_STOP);

    return qtest_readl(qts, base + K230_I2C_DATA_CMD);
}

static void k230_plic_enable_irq(QTestState *qts, unsigned int irq)
{
    uint32_t enable;
    uint64_t enable_addr;

    qtest_writel(qts, K230_PLIC_BASE + irq * 4, 1);
    enable_addr = K230_PLIC_BASE + K230_PLIC_ENABLE_S + (irq / 32) * 4;
    enable = qtest_readl(qts, enable_addr);
    qtest_writel(qts, enable_addr, enable | (1u << (irq % 32)));
    qtest_writel(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                 K230_PLIC_THRESHOLD, 0);
}

static uint32_t k230_plic_claim(QTestState *qts)
{
    return qtest_readl(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                       K230_PLIC_CLAIM);
}

static void k230_plic_complete(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                 K230_PLIC_CLAIM, irq);
}

static void test_ov5647_chip_id(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv");

    g_assert_cmphex(k230_i2c_read_reg8(qts, K230_I2C0_BASE, 0x300a), ==, 0x56);
    g_assert_cmphex(k230_i2c_read_reg8(qts, K230_I2C0_BASE, 0x300b), ==, 0x47);

    qtest_quit(qts);
}

static void test_flash_xip_writable(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv");
    uint64_t addr = K230_FLASH_BASE + K230_FLASH_TEST_OFFSET;

    g_assert_cmphex(qtest_readl(qts, addr), ==, 0xffffffff);

    qtest_writel(qts, addr, 0x5a17c0de);
    g_assert_cmphex(qtest_readl(qts, addr), ==, 0x5a17c0de);

    qtest_quit(qts);
}

static void test_media_regs_readback(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv");
    static const uint8_t nonai_y_src[16] = {
        0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23,
        0x30, 0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x43,
    };
    static const uint8_t nonai_u_src[4] = { 0x80, 0x81, 0x90, 0x91 };
    static const uint8_t nonai_v_src[4] = { 0xa0, 0xa1, 0xb0, 0xb1 };
    uint8_t nonai_y_dst[sizeof(nonai_y_src)];
    uint8_t nonai_u_dst[sizeof(nonai_u_src)];
    uint8_t nonai_v_dst[sizeof(nonai_v_src)];

    k230_plic_enable_irq(qts, K230_DMA_IRQ);
    k230_plic_enable_irq(qts, K230_NON_AI_2D_IRQ);
    k230_plic_enable_irq(qts, K230_FFT_IRQ);
    k230_plic_enable_irq(qts, K230_AI2D_IRQ);
    k230_plic_enable_irq(qts, K230_ISP_MI_IRQ);
    k230_plic_enable_irq(qts, K230_ISP_FE_IRQ);
    k230_plic_enable_irq(qts, K230_ISP_IRQ);
    k230_plic_enable_irq(qts, K230_DWE_IRQ);
    k230_plic_enable_irq(qts, K230_FE_IRQ);
    k230_plic_enable_irq(qts, K230_DPU_IRQ);
    k230_plic_enable_irq(qts, K230_VSE_IRQ);

    qtest_writel(qts, K230_KPU_CFG_BASE + 0x10, 0x00000001);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);

    qtest_writel(qts, K230_GSDMA_BASE + K230_GSDMA_CH0_CTL, 1);
    g_assert_cmphex(qtest_readl(qts, K230_GSDMA_BASE +
                                K230_GSDMA_INT_STAT), ==,
                    K230_GSDMA_DONE_CH0);
    g_assert_cmphex(qtest_readl(qts, K230_GSDMA_BASE +
                                K230_GSDMA_CH0_STATUS), ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_DMA_IRQ);
    qtest_writel(qts, K230_GSDMA_BASE + K230_GSDMA_INT_STAT,
                 K230_GSDMA_DONE_CH0);
    k230_plic_complete(qts, K230_DMA_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_memwrite(qts, K230_NON_AI_2D_TEST_SRC_Y, nonai_y_src,
                   sizeof(nonai_y_src));
    qtest_memwrite(qts, K230_NON_AI_2D_TEST_SRC_U, nonai_u_src,
                   sizeof(nonai_u_src));
    qtest_memwrite(qts, K230_NON_AI_2D_TEST_SRC_V, nonai_v_src,
                   sizeof(nonai_v_src));
    qtest_memset(qts, K230_NON_AI_2D_TEST_DST_Y, 0, sizeof(nonai_y_dst));
    qtest_memset(qts, K230_NON_AI_2D_TEST_DST_U, 0, sizeof(nonai_u_dst));
    qtest_memset(qts, K230_NON_AI_2D_TEST_DST_V, 0, sizeof(nonai_v_dst));
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_SRC_SIZE,
                 (4 << 16) | 4);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_SRC_CH0_ADDR,
                 K230_NON_AI_2D_TEST_SRC_Y);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_SRC_CH1_ADDR,
                 K230_NON_AI_2D_TEST_SRC_U);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_SRC_CH2_ADDR,
                 K230_NON_AI_2D_TEST_SRC_V);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_DST_CH0_ADDR,
                 K230_NON_AI_2D_TEST_DST_Y);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_DST_CH1_ADDR,
                 K230_NON_AI_2D_TEST_DST_U);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_DST_CH2_ADDR,
                 K230_NON_AI_2D_TEST_DST_V);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_SRC_STRIDE01,
                 4 | (2 << 16));
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_DST_STRIDE01,
                 4 | (2 << 16));
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_SRC_STRIDE2, 2);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_DST_STRIDE2, 2);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_FMT,
                 K230_NON_AI_2D_FMT_I420 |
                 (K230_NON_AI_2D_FMT_I420 << 16));
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_MAIN_CFG,
                 K230_NON_AI_2D_INTR_MASK | K230_NON_AI_2D_MODE_OSD);
    g_assert_cmphex(qtest_readl(qts, K230_NON_AI_2D_BASE +
                                K230_NON_AI_2D_INTR_STATUS), ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_MAIN_CFG,
                 K230_NON_AI_2D_INTR_MASK | K230_NON_AI_2D_MODE_OSD |
                 K230_NON_AI_2D_CALC_EN);
    g_assert_cmphex(qtest_readl(qts, K230_NON_AI_2D_BASE +
                                K230_NON_AI_2D_INTR_STATUS), ==, 1);
    g_assert_cmphex(qtest_readl(qts, K230_NON_AI_2D_BASE +
                                K230_NON_AI_2D_STOP_BUSY), ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_NON_AI_2D_IRQ);
    qtest_memread(qts, K230_NON_AI_2D_TEST_DST_Y, nonai_y_dst,
                  sizeof(nonai_y_dst));
    qtest_memread(qts, K230_NON_AI_2D_TEST_DST_U, nonai_u_dst,
                  sizeof(nonai_u_dst));
    qtest_memread(qts, K230_NON_AI_2D_TEST_DST_V, nonai_v_dst,
                  sizeof(nonai_v_dst));
    g_assert_cmpmem(nonai_y_dst, sizeof(nonai_y_dst),
                    nonai_y_src, sizeof(nonai_y_src));
    g_assert_cmpmem(nonai_u_dst, sizeof(nonai_u_dst),
                    nonai_u_src, sizeof(nonai_u_src));
    g_assert_cmpmem(nonai_v_dst, sizeof(nonai_v_dst),
                    nonai_v_src, sizeof(nonai_v_src));
    qtest_writel(qts, K230_NON_AI_2D_BASE + K230_NON_AI_2D_INTR_CLEAR, 1);
    k230_plic_complete(qts, K230_NON_AI_2D_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_NON_AI_2D_BASE +
                                K230_NON_AI_2D_INTR_STATUS), ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writeb(qts, K230_FFT_BASE + 0x10, 0xa5);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_FFT_IRQ);
    qtest_writeq(qts, K230_FFT_BASE + 0x20, 1);
    k230_plic_complete(qts, K230_FFT_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_AI_2D_BASE + K230_AI2D_CALC_ENABLE, 1);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_AI2D_IRQ);
    qtest_writel(qts, K230_AI_2D_BASE + K230_AI2D_CLEAR, 1);
    k230_plic_complete(qts, K230_AI2D_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_AI_2D_BASE + K230_AI2D_JOB, 1);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_AI2D_IRQ);
    qtest_writel(qts, K230_AI_2D_BASE + K230_AI2D_CLEAR, 1);
    k230_plic_complete(qts, K230_AI2D_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_AI_2D_BASE + K230_AI2D_LEGACY_START, 1);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_AI2D_IRQ);
    qtest_writel(qts, K230_AI_2D_BASE + K230_AI2D_CLEAR, 1);
    k230_plic_complete(qts, K230_AI2D_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_ISP_BASE + K230_ISP_FE_START,
                 K230_ISP_FE_START_CMD);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_ISP_FE_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_FE_MI_STATUS),
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_TOP_STATUS) &
                    K230_ISP_TOP_PENDING, ==, K230_ISP_TOP_PENDING);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_FE_MI_CLEAR, 1);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_TOP_STATUS,
                 K230_ISP_TOP_ACK);
    k230_plic_complete(qts, K230_ISP_FE_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MCM_CTRL,
                 K230_ISP_MCM_CH0_ENABLE | K230_ISP_MCM_CH1_ENABLE |
                 K230_ISP_MCM_CH2_ENABLE);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_ISP_MI_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_MCM0_STATUS),
                    ==, K230_ISP_MCM_CH0_BUFFER |
                        K230_ISP_MCM_CH1_BUFFER |
                        K230_ISP_MCM_CH0_DONE |
                        K230_ISP_MCM_CH1_DONE);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_MCM2_STATUS),
                    ==, K230_ISP_MCM_CH2_STATUS);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MCM0_CLEAR,
                 K230_ISP_MCM_CH0_DONE | K230_ISP_MCM_CH1_DONE);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_MCM0_STATUS),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_MCM2_STATUS),
                    ==, K230_ISP_MCM_CH2_STATUS);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MCM2_CLEAR,
                 K230_ISP_MCM_CH2_STATUS);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_MCM2_STATUS),
                    ==, 0);
    k230_plic_complete(qts, K230_ISP_MI_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_FE_MI_STATUS),
                    ==, 0);
    qtest_clock_step(qts, K230_ISP_MCM_FRAME_NS + 1);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_ISP_MI_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_MCM0_STATUS),
                    ==, K230_ISP_MCM_CH0_BUFFER |
                        K230_ISP_MCM_CH1_BUFFER |
                        K230_ISP_MCM_CH0_DONE |
                        K230_ISP_MCM_CH1_DONE);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_MCM2_STATUS),
                    ==, K230_ISP_MCM_CH2_STATUS);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + K230_ISP_FE_MI_STATUS),
                    ==, K230_ISP_MI_FRAME_STATUS);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MCM0_CLEAR,
                 K230_ISP_MCM_CH0_DONE | K230_ISP_MCM_CH1_DONE);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MCM2_CLEAR,
                 K230_ISP_MCM_CH2_STATUS);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_FE_MI_CLEAR,
                 K230_ISP_MI_FRAME_STATUS);
    k230_plic_complete(qts, K230_ISP_MI_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MCM_CTRL, 0);

    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MAIN_STATUS, 0x20);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_ISP_IRQ);
    qtest_writel(qts, K230_ISP_BASE + K230_ISP_MAIN_CLEAR, 0x20);
    k230_plic_complete(qts, K230_ISP_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_DEWARP_BASE + K230_FE_START,
                 K230_FE_START_CMD);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_FE_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_DEWARP_BASE +
                                K230_FE_IRQ_STATUS), ==, 1);
    qtest_writel(qts, K230_DEWARP_BASE + K230_FE_IRQ_CLEAR, 1);
    k230_plic_complete(qts, K230_FE_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_DEWARP_BASE + K230_DWE_DMA_START0, 1);
    qtest_writel(qts, K230_DEWARP_BASE + K230_DWE_DMA_START1, 1);
    qtest_writel(qts, K230_DEWARP_BASE + K230_DWE_BUS_CTRL_LOW,
                 K230_DWE_BUS_ENABLE);
    g_assert_cmphex(qtest_readl(qts, K230_DEWARP_BASE +
                                K230_DWE_BUS_CTRL_LOW) &
                    K230_DWE_BUS_ENABLE, ==, K230_DWE_BUS_ENABLE);
    qtest_writel(qts, K230_DEWARP_BASE + K230_DWE_CTRL_LOW,
                 K230_DWE_START);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_DWE_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_DEWARP_BASE +
                                K230_DWE_IRQ_STATUS_LOW), ==, 1);
    qtest_writel(qts, K230_DEWARP_BASE + K230_DWE_IRQ_STATUS_LOW, 1);
    k230_plic_complete(qts, K230_DWE_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_DEWARP_BASE + K230_DWE_CTRL,
                 K230_DWE_START);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_DWE_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_DEWARP_BASE +
                                K230_DWE_IRQ_STATUS), ==, 1);
    qtest_writel(qts, K230_DEWARP_BASE + K230_DWE_IRQ_CLEAR, 0x70);
    k230_plic_complete(qts, K230_DWE_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_VSE_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_DEWARP_BASE +
                                K230_VSE_IRQ_STATUS), ==, 0x7);
    qtest_writel(qts, K230_DEWARP_BASE + K230_VSE_IRQ_CLEAR, 0x7);
    k230_plic_complete(qts, K230_VSE_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_writel(qts, K230_DPU_BASE + K230_DPU_IRQ_CLEAR,
                 K230_DPU_CLEAR_ACK);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    qtest_writel(qts, K230_DPU_BASE + K230_DPU_START, 1);
    g_assert_cmphex(qtest_readl(qts, K230_DPU_BASE + K230_DPU_STATUS), ==,
                    K230_DPU_DONE);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_DPU_IRQ);
    qtest_writel(qts, K230_DPU_BASE + K230_DPU_IRQ_CLEAR,
                 K230_DPU_CLEAR_ACK);
    k230_plic_complete(qts, K230_DPU_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_DPU_BASE + K230_DPU_STATUS), ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    qtest_writel(qts, K230_DPU_BASE + K230_DPU_START_CH1, 1);
    g_assert_cmphex(qtest_readl(qts, K230_DPU_BASE + K230_DPU_STATUS), ==,
                    K230_DPU_DONE);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_DPU_IRQ);
    qtest_writel(qts, K230_DPU_BASE + K230_DPU_IRQ_CLEAR,
                 K230_DPU_CLEAR_ACK);
    k230_plic_complete(qts, K230_DPU_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE +
                                K230_RX_CSI_HOST2_PHY_STATE) &
                    K230_RX_CSI_PHY_STOPSTATE, ==,
                    K230_RX_CSI_PHY_STOPSTATE);
    qtest_writel(qts, K230_RX_CSI_BASE + K230_RX_CSI_HOST2_ENABLE, 1);
    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE +
                                K230_RX_CSI_HOST2_PHY_STATE) &
                    K230_RX_CSI_PHY_STOPSTATE, ==,
                    K230_RX_CSI_PHY_STOPSTATE);
    qtest_writel(qts, K230_RX_CSI_BASE + K230_RX_CSI_DIS_FRAME_M, 1);
    qtest_writel(qts, K230_RX_CSI_BASE + K230_RX_CSI_DIS_FRAME_N, 3);
    qtest_writel(qts, K230_RX_CSI_BASE + K230_RX_CSI_DIS_FRAME_EN, 4);
    qtest_writel(qts, K230_RX_CSI_BASE + K230_RX_CSI_PHY_CTRL0,
                 0x00001234);
    qtest_writel(qts, K230_RX_CSI_BASE + K230_RX_CSI_PHY_DATA1,
                 0x00005678);
    qtest_writel(qts, K230_RX_CSI_BASE + K230_RX_CSI_PHY_CTRL1,
                 0x00009abc);
    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE +
                                K230_RX_CSI_DIS_FRAME_M), ==, 1);
    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE +
                                K230_RX_CSI_DIS_FRAME_N), ==, 3);
    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE +
                                K230_RX_CSI_DIS_FRAME_EN), ==, 4);
    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE +
                                K230_RX_CSI_PHY_DATA0), ==, 0x00001234);
    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE +
                                K230_RX_CSI_PHY_DATA1), ==, 0x00005678);

    qtest_writel(qts, K230_NON_AI_2D_BASE + 0x08, 0x5a5a5a5a);
    qtest_writel(qts, K230_ISP_BASE + 0x05bc, 0x0780002f);
    qtest_writel(qts, K230_ISP_BASE + 0x5714, 0x00000001);
    qtest_writel(qts, K230_RX_CSI_BASE + 0x0880, 0x01000100);
    qtest_writel(qts, K230_UART3_BASE + 0xc0, 0x00000002);

    g_assert_cmphex(qtest_readb(qts, K230_FFT_BASE + 0x10), ==, 0xa5);
    g_assert_cmphex(qtest_readl(qts, K230_AI_2D_BASE +
                                K230_AI2D_LEGACY_START), ==,
                    0x00000001);
    g_assert_cmphex(qtest_readl(qts, K230_NON_AI_2D_BASE + 0x08), ==,
                    0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + 0x05bc), ==,
                    0x0780002f);
    g_assert_cmphex(qtest_readl(qts, K230_ISP_BASE + 0x5714), ==,
                    0x00000001);
    g_assert_cmphex(qtest_readl(qts, K230_RX_CSI_BASE + 0x0880), ==,
                    0x01000100);
    g_assert_cmphex(qtest_readl(qts, K230_UART3_BASE + 0xc0), ==,
                    0x00000002);

    qtest_quit(qts);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/k230-sysctl/cpu1-reset-sequence-smp1",
                   test_cpu1_reset_sequence_smp1);
    qtest_add_func("/k230-sysctl/cpu1-reset-sequence-smp2",
                   test_cpu1_reset_sequence_smp2);
    qtest_add_func("/k230-sysctl/smp1-topology", test_smp1_topology);
    qtest_add_func("/k230-sysctl/smp2-topology", test_smp2_topology);
    qtest_add_func("/k230-sysctl/smp2-boot-both-cores-topology",
                   test_smp2_boot_both_cores_topology);
    qtest_add_func("/k230-sysctl/clint-smode-regs", test_clint_smode_regs);
    qtest_add_func("/k230-sysctl/ov5647-chip-id", test_ov5647_chip_id);
    qtest_add_func("/k230-sysctl/flash-xip-writable",
                   test_flash_xip_writable);
    qtest_add_func("/k230-sysctl/media-regs-readback",
                   test_media_regs_readback);

    return g_test_run();
}

/*
 * QTest testcase for K230 I2S/WS2812 and AMP flows
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define K230_I2S_BASE            0x9140f000
#define K230_PDMA_BASE           0x80804000
#define K230_BOOT_BASE           0x91102000
#define K230_RMU_BASE            0x91101000
#define K230_PLIC_BASE           0xf00000000ULL

#define K230_I2S_IER             0x000
#define K230_I2S_ITER            0x008
#define K230_I2S_CER             0x00c
#define K230_I2S_CCR             0x010
#define K230_I2S_TXFFR           0x018
#define K230_I2S_CH0_TER         0x02c
#define K230_I2S_CH0_TCR         0x034
#define K230_I2S_CH0_IMR         0x03c
#define K230_I2S_CH0_TFCR        0x04c
#define K230_I2S_CH0_TFF         0x054
#define K230_I2S_TXDMA           0x1c8
#define K230_I2S_AUDIO_IN_CTRL   0x400
#define K230_I2S_AUDIO_OUT_CTRL  0xc00
#define K230_I2S_COMP_PARAM_2   0x1f0
#define K230_I2S_COMP_PARAM_1   0x1f4
#define K230_I2S_COMP_VERSION   0x1f8
#define K230_I2S_COMP_TYPE      0x1fc
#define K230_I2S_CCR_DMA_ENABLE  (1u << 8)

#define K230_PDMA_CH_EN          0x000
#define K230_PDMA_INT_STAT       0x008
#define K230_PDMA_CH0_CTL        0x020
#define K230_PDMA_CH0_CFG        0x028
#define K230_PDMA_CH0_LLT_SADDR  0x02c
#define K230_PDMA_DEV_SEL0       0x120
#define K230_PDMA_CH0_START      1
#define K230_PDMA_CH0_DONE       1
#define K230_PDMA_HSIZE_4        (2u << 1)
#define K230_PDMA_AUDIO_TX       20
#define K230_PDMA_IRQ            203

#define K230_CPU1_RST_CTL        0x00c
#define K230_CPU1_HART_RSTVEC    0x104
#define K230_CPU1_RST_REQ        (1u << 0)
#define K230_CPU1_RST_DONE       (1u << 12)
#define K230_CPU1_PRST_DONE      (1u << 13)
#define K230_CPU1_RST_REQ_WEN    (1u << 16)
#define K230_CPU1_RST_DONE_WEN   (1u << 28)
#define K230_CPU1_RST_CTL_RESET  \
    (K230_CPU1_PRST_DONE | K230_CPU1_RST_REQ)

#define K230_PLIC_ENABLE_S       0x2080
#define K230_PLIC_CONTEXT_S      0x201000
#define K230_PLIC_THRESHOLD      0x00
#define K230_PLIC_CLAIM          0x04

#define K230_TEST_DATA_ADDR      0x01000000
#define K230_TEST_DESC_ADDR      0x01000100
#define K230_CPU1_TEST_RESETVEC 0x00200000
#define K230_CPU1_ALT_RESETVEC  0x00400000
#define K230_CPU1_TEST_PATTERN_SIZE 16
#define K230_CPU1_DEFER_NS 16000000000LL
#define K230_CPU1_TEST_MARKER   0x00201000

static void write_le32(uint8_t *buf, size_t offset, uint32_t value);


static uint32_t k230_qom_get_u32(QTestState *qts, const char *path,
                                 const char *property)
{
    QDict *resp;
    uint32_t value;

    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                          "{ 'path': %s, 'property': %s } }",
                     path, property);
    g_assert(qdict_haskey(resp, "return"));
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);

    return value;
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

static uint32_t ws2812_encode_byte(uint8_t color)
{
    uint32_t ret = 0;

    for (int i = 3; i >= 0; i--) {
        uint8_t symbol;

        switch ((color >> (i * 2)) & 3) {
        case 0:
            symbol = 0x88;
            break;
        case 1:
            symbol = 0x8e;
            break;
        case 2:
            symbol = 0xe8;
            break;
        case 3:
            symbol = 0xee;
            break;
        default:
            g_assert_not_reached();
        }
        ret |= (uint32_t)symbol << (i * 8);
    }

    return ret;
}

static uint32_t k230_ws2812_prop(QTestState *qts, const char *property)
{
    return k230_qom_get_u32(qts, "/machine/soc/k230-i2s", property);
}

static void k230_assert_cpu1_reset(QTestState *qts)
{
    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_DONE_WEN | K230_CPU1_RST_DONE);
    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_REQ_WEN | K230_CPU1_RST_REQ);
    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_RST_CTL_RESET);
}

static void k230_release_cpu1_reset(QTestState *qts)
{
    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_REQ_WEN);
}

static void k230_write_pattern(QTestState *qts, uint64_t addr,
                               const uint8_t *pattern, size_t size)
{
    qtest_memwrite(qts, addr, pattern, size);
}

static void k230_read_pattern(QTestState *qts, uint64_t addr,
                              uint8_t *buf, size_t size)
{
    qtest_memread(qts, addr, buf, size);
}

static void k230_expect_pattern(QTestState *qts, uint64_t addr,
                                const uint8_t *pattern, size_t size)
{
    uint8_t buf[K230_CPU1_TEST_PATTERN_SIZE];

    g_assert_cmpuint(size, <=, sizeof(buf));
    k230_read_pattern(qts, addr, buf, size);
    g_assert_cmpmem(buf, size, pattern, size);
}

static void k230_expect_zeroed(QTestState *qts, uint64_t addr, size_t size)
{
    uint8_t buf[K230_CPU1_TEST_PATTERN_SIZE];
    uint8_t zeros[K230_CPU1_TEST_PATTERN_SIZE] = { 0 };

    g_assert_cmpuint(size, <=, sizeof(buf));
    k230_read_pattern(qts, addr, buf, size);
    g_assert_cmpmem(buf, size, zeros, size);
}



static void k230_enable_ws2812_tx(QTestState *qts)
{
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_IER, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_ITER, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CER, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CH0_IMR, 0x33);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CH0_TER, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CH0_TCR, 5);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CH0_TFCR, 3);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CCR,
                 K230_I2S_CCR_DMA_ENABLE | (2 << 3) | (1 << 5));
}

static void test_i2s_init_registers(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv");
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_IER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_ITER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_CER), ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_CCR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_CH0_TER), ==,
                    0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_COMP_PARAM_2),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_COMP_PARAM_1),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_COMP_VERSION),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_COMP_TYPE),
                    ==, 0);

    qtest_writeb(qts, K230_I2S_BASE + K230_I2S_IER, 1);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_IER), ==, 0);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_IER, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_IER), ==, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_ITER, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_ITER), ==, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CER, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_CER), ==, 1);

    k230_enable_ws2812_tx(qts);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_AUDIO_IN_CTRL, 1 << 5);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_AUDIO_OUT_CTRL,
                 (2 << 5) | 1);

    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_IER), ==, 1);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_ITER), ==, 1);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_CH0_TER), ==,
                    1);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_CH0_TCR), ==,
                    5);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_CH0_TFCR), ==,
                    3);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_AUDIO_IN_CTRL),
                    ==, 1 << 5);
    g_assert_cmphex(qtest_readl(qts, K230_I2S_BASE + K230_I2S_AUDIO_OUT_CTRL),
                    ==, (2 << 5) | 1);

    qtest_writel(qts, K230_I2S_BASE + K230_I2S_TXFFR, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CH0_TFF, 1);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte-count"), ==, 0);

    qtest_quit(qts);
}

static void test_direct_tx_encoded_words(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv");

    k230_enable_ws2812_tx(qts);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_TXFFR, 1);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CCR, 0);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_TXDMA,
                 ws2812_encode_byte(0x00));
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte-count"), ==, 0);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-invalid-count"), ==, 0);

    qtest_writel(qts, K230_I2S_BASE + K230_I2S_CCR,
                 K230_I2S_CCR_DMA_ENABLE | (2 << 3) | (1 << 5));
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_TXDMA,
                 ws2812_encode_byte(0x00));
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_TXDMA,
                 ws2812_encode_byte(0xff));
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_TXDMA, 0);

    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte-count"), ==, 2);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte0"), ==, 0x00);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte1"), ==, 0xff);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-padding-count"), ==, 1);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-invalid-count"), ==, 0);

    qtest_quit(qts);
}

static void write_le32(uint8_t *buf, size_t offset, uint32_t value)
{
    stl_le_p(buf + offset, value);
}

static void test_pdma_memory_to_i2s(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv");
    uint8_t data[16];
    uint8_t desc[16];

    k230_plic_enable_irq(qts, K230_PDMA_IRQ);
    k230_enable_ws2812_tx(qts);
    qtest_writel(qts, K230_I2S_BASE + K230_I2S_TXFFR, 1);

    write_le32(data, 0, ws2812_encode_byte(0x00));
    write_le32(data, 4, ws2812_encode_byte(0xff));
    write_le32(data, 8, ws2812_encode_byte(0x5a));
    write_le32(data, 12, 0);
    qtest_memwrite(qts, K230_TEST_DATA_ADDR, data, sizeof(data));

    write_le32(desc, 0, sizeof(data));
    write_le32(desc, 4, K230_TEST_DATA_ADDR);
    write_le32(desc, 8, K230_I2S_BASE + K230_I2S_TXDMA);
    write_le32(desc, 12, 0);
    qtest_memwrite(qts, K230_TEST_DESC_ADDR, desc, sizeof(desc));

    qtest_writel(qts, K230_PDMA_BASE + K230_PDMA_CH0_CFG,
                 K230_PDMA_HSIZE_4);
    qtest_writel(qts, K230_PDMA_BASE + K230_PDMA_DEV_SEL0,
                 K230_PDMA_AUDIO_TX);
    qtest_writel(qts, K230_PDMA_BASE + K230_PDMA_CH0_LLT_SADDR,
                 K230_TEST_DESC_ADDR);
    qtest_writel(qts, K230_PDMA_BASE + K230_PDMA_CH_EN, 1);
    qtest_writel(qts, K230_PDMA_BASE + K230_PDMA_CH0_CTL,
                 K230_PDMA_CH0_START);

    g_assert_cmphex(qtest_readl(qts, K230_PDMA_BASE + K230_PDMA_INT_STAT),
                    ==, K230_PDMA_CH0_DONE);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_PDMA_IRQ);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte-count"), ==, 3);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte0"), ==, 0x00);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte1"), ==, 0xff);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-byte2"), ==, 0x5a);
    g_assert_cmpuint(k230_ws2812_prop(qts, "ws2812-padding-count"), ==, 1);

    qtest_writel(qts, K230_PDMA_BASE + K230_PDMA_INT_STAT,
                 K230_PDMA_CH0_DONE);
    k230_plic_complete(qts, K230_PDMA_IRQ);
    g_assert_cmphex(qtest_readl(qts, K230_PDMA_BASE + K230_PDMA_INT_STAT),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    qtest_quit(qts);
}

static void test_amp_exact_reset_sequence(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv -smp 2");
    QDict *cpu1;

    cpu1 = k230_query_cpu(qts, 1);
    g_assert_nonnull(cpu1);
    qobject_unref(cpu1);

    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_RST_CTL_RESET);
    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_DONE_WEN | K230_CPU1_RST_DONE);
    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_REQ_WEN | K230_CPU1_RST_REQ);
    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_RST_CTL_RESET);

    qtest_writel(qts, K230_CPU1_TEST_MARKER, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, K230_CPU1_TEST_MARKER), ==,
                    0xdeadbeef);
    qtest_writel(qts, K230_BOOT_BASE + K230_CPU1_HART_RSTVEC,
                 K230_CPU1_TEST_RESETVEC);
    qtest_writel(qts, K230_RMU_BASE + K230_CPU1_RST_CTL,
                 K230_CPU1_RST_REQ_WEN);
    g_assert_cmphex(qtest_readl(qts, K230_RMU_BASE + K230_CPU1_RST_CTL), ==,
                    K230_CPU1_PRST_DONE | K230_CPU1_RST_DONE);
    g_assert_cmphex(k230_qom_get_u32(qts, "/machine/soc/k230-sysctl-reset",
                                     "last-cpu1-rstvec"), ==,
                    K230_CPU1_TEST_RESETVEC);
    qtest_quit(qts);
}

static void test_amp_deferred_reset_reassert_restores_rtt(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv,boot-both-cores=on -smp 2");
    static const uint8_t pattern[K230_CPU1_TEST_PATTERN_SIZE] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xf0, 0x0f,
    };

    k230_assert_cpu1_reset(qts);
    k230_write_pattern(qts, K230_CPU1_TEST_RESETVEC, pattern, sizeof(pattern));
    qtest_writel(qts, K230_BOOT_BASE + K230_CPU1_HART_RSTVEC,
                 K230_CPU1_TEST_RESETVEC);
    k230_release_cpu1_reset(qts);
    k230_expect_zeroed(qts, K230_CPU1_TEST_RESETVEC, sizeof(pattern));

    k230_assert_cpu1_reset(qts);
    k230_expect_pattern(qts, K230_CPU1_TEST_RESETVEC, pattern,
                        sizeof(pattern));
    qtest_clock_step(qts, K230_CPU1_DEFER_NS);
    k230_expect_pattern(qts, K230_CPU1_TEST_RESETVEC, pattern,
                        sizeof(pattern));

    qtest_quit(qts);
}

static void test_amp_deferred_release_snapshots_rstvec(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv,boot-both-cores=on -smp 2");
    static const uint8_t pattern[K230_CPU1_TEST_PATTERN_SIZE] = {
        0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18,
        0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90,
    };

    k230_assert_cpu1_reset(qts);
    k230_write_pattern(qts, K230_CPU1_TEST_RESETVEC, pattern, sizeof(pattern));
    qtest_writel(qts, K230_BOOT_BASE + K230_CPU1_HART_RSTVEC,
                 K230_CPU1_TEST_RESETVEC);
    k230_release_cpu1_reset(qts);
    k230_expect_zeroed(qts, K230_CPU1_TEST_RESETVEC, sizeof(pattern));

    qtest_writel(qts, K230_BOOT_BASE + K230_CPU1_HART_RSTVEC,
                 K230_CPU1_ALT_RESETVEC);
    qtest_clock_step(qts, K230_CPU1_DEFER_NS);
    k230_expect_pattern(qts, K230_CPU1_TEST_RESETVEC, pattern,
                        sizeof(pattern));
    g_assert_cmphex(k230_qom_get_u32(qts, "/machine/soc/k230-sysctl-reset",
                                     "last-cpu1-rstvec"), ==,
                    K230_CPU1_TEST_RESETVEC);

    qtest_quit(qts);
}

static void test_amp_deferred_release_migration(void)
{
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir = g_dir_make_tmp("k230-mig-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migsock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    static const uint8_t pattern[K230_CPU1_TEST_PATTERN_SIZE] = {
        0xde, 0xad, 0xbe, 0xef, 0x12, 0x34, 0x56, 0x78,
        0x87, 0x65, 0x43, 0x21, 0xca, 0xfe, 0xba, 0xbe,
    };

    g_assert_nonnull(tmpdir);
    src = qtest_init("-machine k230-canmv,boot-both-cores=on -smp 2 -nic none");
    dst = qtest_init("-machine k230-canmv,boot-both-cores=on -smp 2 "
                     "-incoming defer -nic none");

    k230_assert_cpu1_reset(src);
    k230_write_pattern(src, K230_CPU1_TEST_RESETVEC, pattern, sizeof(pattern));
    qtest_writel(src, K230_BOOT_BASE + K230_CPU1_HART_RSTVEC,
                 K230_CPU1_TEST_RESETVEC);
    k230_release_cpu1_reset(src);
    k230_expect_zeroed(src, K230_CPU1_TEST_RESETVEC, sizeof(pattern));

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    k230_expect_zeroed(dst, K230_CPU1_TEST_RESETVEC, sizeof(pattern));
    qtest_clock_step(dst, K230_CPU1_DEFER_NS);
    k230_expect_pattern(dst, K230_CPU1_TEST_RESETVEC, pattern,
                        sizeof(pattern));

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/k230-ws2812/i2s-init-registers",
                   test_i2s_init_registers);
    qtest_add_func("/k230-ws2812/direct-tx-encoded-words",
                   test_direct_tx_encoded_words);
    qtest_add_func("/k230-ws2812/pdma-memory-to-i2s",
                   test_pdma_memory_to_i2s);
    qtest_add_func("/k230-amp/deferred-reset-reassert-restores-rtt",
                   test_amp_deferred_reset_reassert_restores_rtt);
    qtest_add_func("/k230-amp/deferred-release-snapshots-rstvec",
                   test_amp_deferred_release_snapshots_rstvec);
    qtest_add_func("/k230-amp/deferred-release-migration",
                   test_amp_deferred_release_migration);
    qtest_add_func("/k230-amp/exact-reset-sequence",
                   test_amp_exact_reset_sequence);

    return g_test_run();
}

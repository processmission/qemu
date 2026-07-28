/*
 * QTest for the STM32G474 FDCAN subsystem
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "qemu/bitops.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define STM32G474_MACHINE "stm32g474"
#define MCU_QOM_PATH "/machine/mcu"
#define FDCAN_QOM_PATH MCU_QOM_PATH "/fdcan"

#define FDCAN1_BASE 0x40006400ULL
#define FDCAN2_BASE 0x40006800ULL
#define FDCAN3_BASE 0x40006c00ULL
#define FDCAN_MRAM_BASE 0x4000a400ULL
#define FDCAN_MRAM_SIZE 0x9f0
#define FDCAN_MRAM_SLICE_SIZE 0x350

#define FDCAN_CREL 0x000
#define FDCAN_ENDN 0x004
#define FDCAN_DBTP 0x00c
#define FDCAN_TEST 0x010
#define FDCAN_RWD 0x014
#define FDCAN_CCCR 0x018
#define FDCAN_NBTP 0x01c
#define FDCAN_TSCC 0x020
#define FDCAN_TSCV 0x024
#define FDCAN_TOCC 0x028
#define FDCAN_TOCV 0x02c
#define FDCAN_ECR 0x040
#define FDCAN_PSR 0x044
#define FDCAN_TDCR 0x048
#define FDCAN_IR 0x050
#define FDCAN_IE 0x054
#define FDCAN_ILS 0x058
#define FDCAN_ILE 0x05c
#define FDCAN_RXGFC 0x080
#define FDCAN_XIDAM 0x084
#define FDCAN_HPMS 0x088
#define FDCAN_RXF0S 0x090
#define FDCAN_RXF0A 0x094
#define FDCAN_RXF1S 0x098
#define FDCAN_RXF1A 0x09c
#define FDCAN_TXBC 0x0c0
#define FDCAN_TXFQS 0x0c4
#define FDCAN_TXBRP 0x0c8
#define FDCAN_TXBAR 0x0cc
#define FDCAN_TXBCR 0x0d0
#define FDCAN_TXBTO 0x0d4
#define FDCAN_TXBCF 0x0d8
#define FDCAN_TXBTIE 0x0dc
#define FDCAN_TXBCIE 0x0e0
#define FDCAN_TXEFS 0x0e4
#define FDCAN_TXEFA 0x0e8
#define FDCAN_CKDIV 0x100

#define FDCAN_CCCR_INIT BIT(0)
#define FDCAN_CCCR_CCE BIT(1)
#define FDCAN_CCCR_TEST BIT(7)
#define FDCAN_TEST_WRITABLE_MASK 0x00000070U
#define FDCAN_DBTP_MASK 0x009f1fffU
#define FDCAN_IR_TCF BIT(8)
#define FDCAN_ILS_GROUP2 BIT(2)
#define FDCAN_ILE_EINT0 BIT(0)
#define FDCAN_ILE_EINT1 BIT(1)

#define RCC_BASE 0x40021000ULL
#define RCC_APB1RSTR1 0x38
#define RCC_APB1ENR1 0x58
#define RCC_CCIPR 0x88
#define RCC_APB1_FDCAN BIT(25)
#define RCC_CCIPR_FDCAN_SHIFT 24
#define RCC_CCIPR_FDCAN_MASK (3U << RCC_CCIPR_FDCAN_SHIFT)
#define RCC_CCIPR_FDCAN_PCLK1 (2U << RCC_CCIPR_FDCAN_SHIFT)
#define RCC_CCIPR_FDCAN_NONE (3U << RCC_CCIPR_FDCAN_SHIFT)

#define NVIC_ISER 0xe000e100ULL
#define NVIC_ISPR 0xe000e200ULL
#define NVIC_ICPR 0xe000e280ULL

#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)

typedef struct FdcanController {
    const char *name;
    uint64_t base;
    uint32_t mram_offset;
    unsigned int irq[2];
} FdcanController;

typedef struct FdcanResetRegister {
    const char *name;
    uint32_t offset;
    uint32_t reset;
    bool fdcan1_only;
} FdcanResetRegister;

typedef struct MramMarker {
    uint32_t offset;
    uint32_t value;
} MramMarker;

static const FdcanController controllers[] = {
    {
        .name = "FDCAN1",
        .base = FDCAN1_BASE,
        .mram_offset = 0,
        .irq = { 21, 22 },
    }, {
        .name = "FDCAN2",
        .base = FDCAN2_BASE,
        .mram_offset = FDCAN_MRAM_SLICE_SIZE,
        .irq = { 86, 87 },
    }, {
        .name = "FDCAN3",
        .base = FDCAN3_BASE,
        .mram_offset = 2 * FDCAN_MRAM_SLICE_SIZE,
        .irq = { 88, 89 },
    },
};

static const FdcanResetRegister reset_registers[] = {
    { "CREL",   FDCAN_CREL,   0x32141218, false },
    { "ENDN",   FDCAN_ENDN,   0x87654321, false },
    { "DBTP",   FDCAN_DBTP,   0x00000a33, false },
    { "TEST",   FDCAN_TEST,   0x00000000, false },
    { "RWD",    FDCAN_RWD,    0x00000000, false },
    { "CCCR",   FDCAN_CCCR,   0x00000001, false },
    { "NBTP",   FDCAN_NBTP,   0x06000a03, false },
    { "TSCC",   FDCAN_TSCC,   0x00000000, false },
    { "TSCV",   FDCAN_TSCV,   0x00000000, false },
    { "TOCC",   FDCAN_TOCC,   0xffff0000, false },
    { "TOCV",   FDCAN_TOCV,   0x0000ffff, false },
    { "ECR",    FDCAN_ECR,    0x00000000, false },
    { "PSR",    FDCAN_PSR,    0x00000707, false },
    { "TDCR",   FDCAN_TDCR,   0x00000000, false },
    { "IR",     FDCAN_IR,     0x00000000, false },
    { "IE",     FDCAN_IE,     0x00000000, false },
    { "ILS",    FDCAN_ILS,    0x00000000, false },
    { "ILE",    FDCAN_ILE,    0x00000000, false },
    { "RXGFC",  FDCAN_RXGFC,  0x00000000, false },
    { "XIDAM",  FDCAN_XIDAM,  0x1fffffff, false },
    { "HPMS",   FDCAN_HPMS,   0x00000000, false },
    { "RXF0S",  FDCAN_RXF0S,  0x00000000, false },
    { "RXF0A",  FDCAN_RXF0A,  0x00000000, false },
    { "RXF1S",  FDCAN_RXF1S,  0x00000000, false },
    { "RXF1A",  FDCAN_RXF1A,  0x00000000, false },
    { "TXBC",   FDCAN_TXBC,   0x00000000, false },
    { "TXFQS",  FDCAN_TXFQS,  0x00000003, false },
    { "TXBRP",  FDCAN_TXBRP,  0x00000000, false },
    { "TXBAR",  FDCAN_TXBAR,  0x00000000, false },
    { "TXBCR",  FDCAN_TXBCR,  0x00000000, false },
    { "TXBTO",  FDCAN_TXBTO,  0x00000000, false },
    { "TXBCF",  FDCAN_TXBCF,  0x00000000, false },
    { "TXBTIE", FDCAN_TXBTIE, 0x00000000, false },
    { "TXBCIE", FDCAN_TXBCIE, 0x00000000, false },
    { "TXEFS",  FDCAN_TXEFS,  0x00000000, false },
    { "TXEFA",  FDCAN_TXEFA,  0x00000000, false },
    { "CKDIV",  FDCAN_CKDIV,  0x00000000, true },
};

static const MramMarker mram_markers[] = {
    { 0x000, 0x10203040 },
    { 0x34c, 0x11213141 },
    { 0x350, 0x50607080 },
    { 0x69c, 0x51617181 },
    { 0x6a0, 0x90a0b0c0 },
    { 0x9ec, 0x91a1b1c1 },
};

G_STATIC_ASSERT(ARRAY_SIZE(controllers) == 3);
G_STATIC_ASSERT(2 * FDCAN_MRAM_SLICE_SIZE == 0x6a0);
G_STATIC_ASSERT(3 * FDCAN_MRAM_SLICE_SIZE == FDCAN_MRAM_SIZE);

static QTestState *stm32g474_qtest_start(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null");
}

static QTestState *stm32g474_qtest_start_incoming(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null"
                      " -incoming defer");
}

static uint32_t fdcan_readl(QTestState *qts,
                            const FdcanController *controller,
                            uint32_t offset)
{
    return qtest_readl(qts, controller->base + offset);
}

static void fdcan_writel(QTestState *qts,
                         const FdcanController *controller,
                         uint32_t offset, uint32_t value)
{
    qtest_writel(qts, controller->base + offset, value);
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static void assert_qom_child(QTestState *qts, const char *parent,
                             const char *name, const char *type)
{
    g_autofree char *expected_type = g_strdup_printf("child<%s>", type);
    QDict *response;
    QList *properties;
    QListEntry *entry;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-list',"
                         "  'arguments': { 'path': %s } }", parent);
    g_assert_false(qdict_haskey(response, "error"));
    properties = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (!g_strcmp0(qdict_get_str(property, "name"), name)) {
            g_assert_cmpstr(qdict_get_str(property, "type"), ==,
                            expected_type);
            qobject_unref(response);
            return;
        }
    }

    g_error("QOM child %s/%s is missing", parent, name);
}

static uint64_t clock_period_from_hz(uint64_t hz)
{
    return hz ? CLOCK_PERIOD_1SEC / hz : 0;
}

static uint64_t get_clock_period(QTestState *qts, const char *name)
{
    g_autofree char *path =
        g_strdup_printf("%s/%s", FDCAN_QOM_PATH, name);
    QDict *response;
    uint64_t period;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-get', 'arguments': {"
                         "  'path': %s,"
                         "  'property': 'qtest-clock-period' } }", path);
    g_assert_false(qdict_haskey(response, "error"));
    period = qdict_get_int(response, "return");
    qobject_unref(response);

    return period;
}

static void assert_clock_hz(QTestState *qts, const char *name, uint64_t hz)
{
    g_assert_cmphex(get_clock_period(qts, name), ==,
                    clock_period_from_hz(hz));
}

static void assert_reset_image(QTestState *qts,
                               const FdcanController *controller)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(reset_registers); i++) {
        const FdcanResetRegister *reg = &reset_registers[i];

        if (reg->fdcan1_only && controller != &controllers[0]) {
            continue;
        }
        g_test_message("%s %s reset", controller->name, reg->name);
        g_assert_cmphex(fdcan_readl(qts, controller, reg->offset), ==,
                        reg->reset);
    }
}

static void assert_all_reset_images(QTestState *qts)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(controllers); i++) {
        assert_reset_image(qts, &controllers[i]);
    }
}

static uint64_t nvic_register(uint64_t base, unsigned int irq)
{
    return base + sizeof(uint32_t) * (irq / 32);
}

static uint32_t nvic_irq_bit(unsigned int irq)
{
    return BIT(irq % 32);
}

static void nvic_enable(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, nvic_register(NVIC_ISER, irq), nvic_irq_bit(irq));
}

static void nvic_clear(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, nvic_register(NVIC_ICPR, irq), nvic_irq_bit(irq));
}

static bool nvic_pending(QTestState *qts, unsigned int irq)
{
    return qtest_readl(qts, nvic_register(NVIC_ISPR, irq)) &
           nvic_irq_bit(irq);
}

static void set_fdcan_clock_mux(QTestState *qts, uint32_t selection)
{
    uint32_t ccipr = rcc_readl(qts, RCC_CCIPR);

    ccipr &= ~RCC_CCIPR_FDCAN_MASK;
    rcc_writel(qts, RCC_CCIPR, ccipr | selection);
}

static void set_fdcan_gate(QTestState *qts, bool enabled)
{
    uint32_t enr = rcc_readl(qts, RCC_APB1ENR1);

    if (enabled) {
        enr |= RCC_APB1_FDCAN;
    } else {
        enr &= ~RCC_APB1_FDCAN;
    }
    rcc_writel(qts, RCC_APB1ENR1, enr);
}

static void write_mram_markers(QTestState *qts, uint32_t salt)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(mram_markers); i++) {
        qtest_writel(qts, FDCAN_MRAM_BASE + mram_markers[i].offset,
                     mram_markers[i].value ^ salt);
    }
}

static void assert_mram_markers(QTestState *qts, uint32_t salt)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(mram_markers); i++) {
        g_assert_cmphex(
            qtest_readl(qts, FDCAN_MRAM_BASE + mram_markers[i].offset),
            ==, mram_markers[i].value ^ salt);
    }
}

static void trigger_tcf(QTestState *qts,
                        const FdcanController *controller,
                        uint32_t buffer)
{
    fdcan_writel(qts, controller, FDCAN_TXBAR, buffer);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP) & buffer,
                    ==, buffer);
    fdcan_writel(qts, controller, FDCAN_TXBCR, buffer);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP) & buffer,
                    ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCF) & buffer,
                    ==, buffer);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_IR) &
                    FDCAN_IR_TCF, ==, FDCAN_IR_TCF);
}

static void test_constructs(void)
{
    QTestState *qts = stm32g474_qtest_start();

    assert_qom_child(qts, MCU_QOM_PATH, "fdcan", "stm32g474-fdcan");

    qtest_quit(qts);
}

static void test_reset_apertures_and_access(void)
{
    QTestState *qts = stm32g474_qtest_start();

    assert_all_reset_images(qts);

    for (unsigned int i = 0; i < ARRAY_SIZE(controllers); i++) {
        const FdcanController *controller = &controllers[i];
        uint32_t value = BIT(i);

        qtest_writeb(qts, controller->base + FDCAN_IE, UINT8_MAX);
        qtest_writew(qts, controller->base + FDCAN_IE, UINT16_MAX);
        g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_IE), ==, 0);

        fdcan_writel(qts, controller, FDCAN_IE, value);
        g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_IE), ==,
                        value);
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(controllers); i++) {
        g_assert_cmphex(fdcan_readl(qts, &controllers[i], FDCAN_IE), ==,
                        BIT(i));
    }

    fdcan_writel(qts, &controllers[0], FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    fdcan_writel(qts, &controllers[0], FDCAN_CKDIV, 0xf);
    g_assert_cmphex(fdcan_readl(qts, &controllers[0], FDCAN_CKDIV), ==,
                    0xf);
    for (unsigned int i = 1; i < ARRAY_SIZE(controllers); i++) {
        fdcan_writel(qts, &controllers[i], FDCAN_CKDIV, 0xa + i);
        g_assert_cmphex(fdcan_readl(qts, &controllers[i], FDCAN_CKDIV),
                        ==, 0);
        g_assert_cmphex(fdcan_readl(qts, &controllers[0], FDCAN_CKDIV),
                        ==, 0xf);
    }

    qtest_system_reset(qts);
    assert_all_reset_images(qts);
    qtest_quit(qts);
}

static void test_protection_and_triggers(void)
{
    const FdcanController *controller = &controllers[0];
    QTestState *qts = stm32g474_qtest_start();
    const uint32_t request = BIT(0) | BIT(2);
    const uint32_t unavailable_request = BIT(31);

    fdcan_writel(qts, controller, FDCAN_DBTP, UINT32_MAX);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_DBTP), ==,
                    0x00000a33);
    fdcan_writel(qts, controller, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    fdcan_writel(qts, controller, FDCAN_DBTP, UINT32_MAX);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_DBTP), ==,
                    FDCAN_DBTP_MASK);

    fdcan_writel(qts, controller, FDCAN_TEST, UINT32_MAX);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TEST), ==, 0);
    fdcan_writel(qts, controller, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE | FDCAN_CCCR_TEST);
    fdcan_writel(qts, controller, FDCAN_TEST, UINT32_MAX);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TEST), ==,
                    FDCAN_TEST_WRITABLE_MASK);
    fdcan_writel(qts, controller, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TEST), ==, 0);

    fdcan_writel(qts, controller, FDCAN_CCCR, 0);
    fdcan_writel(qts, controller, FDCAN_TXBTIE, UINT32_MAX);
    fdcan_writel(qts, controller, FDCAN_TXBCIE, UINT32_MAX);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTIE), ==,
                    UINT32_MAX);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCIE), ==,
                    UINT32_MAX);
    fdcan_writel(qts, controller, FDCAN_TXBAR, unavailable_request);
    fdcan_writel(qts, controller, FDCAN_TXBCR, unavailable_request);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBAR), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCR), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCF), ==, 0);

    fdcan_writel(qts, controller, FDCAN_TXBCIE, BIT(2));
    fdcan_writel(qts, controller, FDCAN_TXBAR, request);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBAR), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    request);
    fdcan_writel(qts, controller, FDCAN_TXBCR, BIT(2));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    BIT(0));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCR), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCF), ==,
                    BIT(2));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_IR), ==,
                    FDCAN_IR_TCF);

    fdcan_writel(qts, controller, FDCAN_IR, BIT(7));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_IR), ==,
                    FDCAN_IR_TCF);
    fdcan_writel(qts, controller, FDCAN_IR, FDCAN_IR_TCF);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_IR), ==, 0);

    qtest_quit(qts);
}

static void test_message_ram_slices(void)
{
    QTestState *qts = stm32g474_qtest_start();

    for (unsigned int i = 0; i < ARRAY_SIZE(mram_markers); i++) {
        g_assert_cmphex(
            qtest_readl(qts, FDCAN_MRAM_BASE + mram_markers[i].offset),
            ==, 0);
    }
    write_mram_markers(qts, 0);
    assert_mram_markers(qts, 0);

    for (unsigned int i = 0; i < ARRAY_SIZE(controllers); i++) {
        uint64_t slice = FDCAN_MRAM_BASE + controllers[i].mram_offset;

        g_assert_cmphex(qtest_readl(qts, slice), ==,
                        mram_markers[2 * i].value);
        g_assert_cmphex(
            qtest_readl(qts, slice + FDCAN_MRAM_SLICE_SIZE - 4), ==,
            mram_markers[2 * i + 1].value);
    }

    qtest_quit(qts);
}

static void test_rcc_clock_and_reset(void)
{
    QTestState *qts = stm32g474_qtest_start();

    assert_clock_hz(qts, "kernel-clk", 0);
    assert_clock_hz(qts, "pclk", 16000000);

    set_fdcan_gate(qts, true);
    assert_clock_hz(qts, "kernel-clk", 0);
    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_PCLK1);
    assert_clock_hz(qts, "kernel-clk", 16000000);
    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_NONE);
    assert_clock_hz(qts, "kernel-clk", 0);
    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_PCLK1);
    assert_clock_hz(qts, "kernel-clk", 16000000);
    set_fdcan_gate(qts, false);
    assert_clock_hz(qts, "kernel-clk", 0);
    assert_clock_hz(qts, "pclk", 16000000);

    fdcan_writel(qts, &controllers[1], FDCAN_IE, 0x12345);
    g_assert_cmphex(fdcan_readl(qts, &controllers[1], FDCAN_IE), ==,
                    0x12345);
    write_mram_markers(qts, 0xa5a5a5a5);

    rcc_writel(qts, RCC_APB1RSTR1, RCC_APB1_FDCAN);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1RSTR1) & RCC_APB1_FDCAN, ==,
                    RCC_APB1_FDCAN);
    assert_all_reset_images(qts);
    assert_mram_markers(qts, 0xa5a5a5a5);

    fdcan_writel(qts, &controllers[1], FDCAN_IE, 0x54321);
    fdcan_writel(qts, &controllers[1], FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    g_assert_cmphex(fdcan_readl(qts, &controllers[1], FDCAN_IE), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, &controllers[1], FDCAN_CCCR), ==,
                    FDCAN_CCCR_INIT);

    rcc_writel(qts, RCC_APB1RSTR1, 0);
    fdcan_writel(qts, &controllers[1], FDCAN_IE, 0x54321);
    g_assert_cmphex(fdcan_readl(qts, &controllers[1], FDCAN_IE), ==,
                    0x54321);

    qtest_quit(qts);
}

static void test_system_reset_retains_message_ram(void)
{
    QTestState *qts = stm32g474_qtest_start();

    set_fdcan_gate(qts, true);
    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_PCLK1);
    write_mram_markers(qts, 0x5a5a5a5a);
    for (unsigned int i = 0; i < ARRAY_SIZE(controllers); i++) {
        fdcan_writel(qts, &controllers[i], FDCAN_IE, 0x100 + i);
        fdcan_writel(qts, &controllers[i], FDCAN_CCCR,
                     FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
        fdcan_writel(qts, &controllers[i], FDCAN_DBTP,
                     0x00010000 | i);
    }

    qtest_system_reset(qts);
    assert_all_reset_images(qts);
    assert_mram_markers(qts, 0x5a5a5a5a);
    g_assert_cmphex(rcc_readl(qts, RCC_APB1ENR1) & RCC_APB1_FDCAN, ==, 0);
    g_assert_cmphex(rcc_readl(qts, RCC_CCIPR) & RCC_CCIPR_FDCAN_MASK,
                    ==, 0);
    assert_clock_hz(qts, "kernel-clk", 0);
    assert_clock_hz(qts, "pclk", 16000000);

    qtest_quit(qts);
}

static void test_nvic_routing(const void *opaque)
{
    const FdcanController *controller = opaque;
    QTestState *qts = stm32g474_qtest_start();

    set_fdcan_gate(qts, true);
    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_PCLK1);
    for (unsigned int line = 0; line < 2; line++) {
        unsigned int target_irq = controller->irq[line];
        unsigned int other_irq = controller->irq[!line];

        fdcan_writel(qts, controller, FDCAN_IR, FDCAN_IR_TCF);
        fdcan_writel(qts, controller, FDCAN_IE, FDCAN_IR_TCF);
        fdcan_writel(qts, controller, FDCAN_ILS,
                     line ? FDCAN_ILS_GROUP2 : 0);
        fdcan_writel(qts, controller, FDCAN_ILE,
                     line ? FDCAN_ILE_EINT1 : FDCAN_ILE_EINT0);
        fdcan_writel(qts, controller, FDCAN_TXBCIE, BIT(0));
        nvic_enable(qts, target_irq);
        nvic_enable(qts, other_irq);
        nvic_clear(qts, target_irq);
        nvic_clear(qts, other_irq);

        trigger_tcf(qts, controller, BIT(0));
        g_assert_true(nvic_pending(qts, target_irq));
        g_assert_false(nvic_pending(qts, other_irq));

        fdcan_writel(qts, controller, FDCAN_IR, FDCAN_IR_TCF);
        nvic_clear(qts, target_irq);
        nvic_clear(qts, other_irq);
        g_assert_false(nvic_pending(qts, target_irq));
        g_assert_false(nvic_pending(qts, other_irq));
    }

    qtest_quit(qts);
}

static void configure_migration_source(QTestState *qts)
{
    const FdcanController *fdcan1 = &controllers[0];
    const FdcanController *fdcan2 = &controllers[1];
    const FdcanController *fdcan3 = &controllers[2];

    set_fdcan_gate(qts, true);
    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_PCLK1);
    write_mram_markers(qts, 0x0f0f0f0f);

    fdcan_writel(qts, fdcan1, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    fdcan_writel(qts, fdcan1, FDCAN_DBTP, FDCAN_DBTP_MASK);
    fdcan_writel(qts, fdcan1, FDCAN_CKDIV, 3);

    fdcan_writel(qts, fdcan2, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    fdcan_writel(qts, fdcan2, FDCAN_NBTP, 0x1234567f);

    fdcan_writel(qts, fdcan3, FDCAN_IE, FDCAN_IR_TCF);
    fdcan_writel(qts, fdcan3, FDCAN_ILS, FDCAN_ILS_GROUP2);
    fdcan_writel(qts, fdcan3, FDCAN_ILE, FDCAN_ILE_EINT1);
    fdcan_writel(qts, fdcan3, FDCAN_TXBCIE, BIT(0));
    nvic_enable(qts, fdcan3->irq[1]);
    nvic_clear(qts, fdcan3->irq[1]);
    trigger_tcf(qts, fdcan3, BIT(0));
    g_assert_true(nvic_pending(qts, fdcan3->irq[1]));
}

static void assert_migrated_state(QTestState *qts)
{
    const FdcanController *fdcan1 = &controllers[0];
    const FdcanController *fdcan2 = &controllers[1];
    const FdcanController *fdcan3 = &controllers[2];

    g_assert_cmphex(fdcan_readl(qts, fdcan1, FDCAN_CCCR), ==,
                    FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    g_assert_cmphex(fdcan_readl(qts, fdcan1, FDCAN_DBTP), ==,
                    FDCAN_DBTP_MASK);
    g_assert_cmphex(fdcan_readl(qts, fdcan1, FDCAN_CKDIV), ==, 3);
    g_assert_cmphex(fdcan_readl(qts, fdcan2, FDCAN_CCCR), ==,
                    FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    g_assert_cmphex(fdcan_readl(qts, fdcan2, FDCAN_NBTP), ==,
                    0x1234567f);
    g_assert_cmphex(fdcan_readl(qts, fdcan3, FDCAN_IE), ==,
                    FDCAN_IR_TCF);
    g_assert_cmphex(fdcan_readl(qts, fdcan3, FDCAN_ILS), ==,
                    FDCAN_ILS_GROUP2);
    g_assert_cmphex(fdcan_readl(qts, fdcan3, FDCAN_ILE), ==,
                    FDCAN_ILE_EINT1);
    g_assert_cmphex(fdcan_readl(qts, fdcan3, FDCAN_TXBCF), ==, BIT(0));
    g_assert_cmphex(fdcan_readl(qts, fdcan3, FDCAN_IR), ==,
                    FDCAN_IR_TCF);
    g_assert_true(nvic_pending(qts, fdcan3->irq[1]));
    assert_mram_markers(qts, 0x0f0f0f0f);
    assert_clock_hz(qts, "kernel-clk", 16000000);
    assert_clock_hz(qts, "pclk", 16000000);
}

static void test_active_migration(void)
{
    const FdcanController *controller = &controllers[2];
    const unsigned int output = 2 * 2 + 1;
    QTestState *src;
    QTestState *dst;
    uint64_t raises;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-fdcan-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = stm32g474_qtest_start_incoming();
    qtest_irq_intercept_out_named(dst, FDCAN_QOM_PATH, "sysbus-irq");
    raises = qtest_get_irq_raise_count(dst, output);

    configure_migration_source(src);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    assert_migrated_state(dst);
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, output), ==,
                     raises + 1);
    g_assert_true(qtest_get_irq(dst, output));

    fdcan_writel(dst, controller, FDCAN_IR, FDCAN_IR_TCF);
    g_assert_false(qtest_get_irq(dst, output));
    g_assert_cmpuint(qtest_get_irq_raise_count(dst, output), ==,
                     raises + 1);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/fdcan/1-constructs", test_constructs);
    qtest_add_func("/stm32g474/fdcan/2-reset-apertures-access",
                   test_reset_apertures_and_access);
    qtest_add_func("/stm32g474/fdcan/3-protection-triggers",
                   test_protection_and_triggers);
    qtest_add_func("/stm32g474/fdcan/4-message-ram-slices",
                   test_message_ram_slices);
    qtest_add_func("/stm32g474/fdcan/5-rcc-clock-reset",
                   test_rcc_clock_and_reset);
    qtest_add_func("/stm32g474/fdcan/6-system-reset-retention",
                   test_system_reset_retains_message_ram);
    for (unsigned int i = 0; i < ARRAY_SIZE(controllers); i++) {
        g_autofree char *path =
            g_strdup_printf("/stm32g474/fdcan/7-nvic-routing/%u", i + 1);

        qtest_add_data_func(path, &controllers[i], test_nvic_routing);
    }
    qtest_add_func("/stm32g474/fdcan/8-active-migration",
                   test_active_migration);

    return g_test_run();
}

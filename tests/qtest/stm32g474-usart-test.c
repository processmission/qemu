/*
 * QTest for the STM32G474 USART and UART
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "qobject/qdict.h"

#define STM32G474_MACHINE "stm32g474"

#define USART1_BASE 0x40013800ULL
#define USART2_BASE 0x40004400ULL
#define UART4_BASE 0x40004c00ULL
#define RCC_BASE 0x40021000ULL

#define USART_CR1 0x00
#define USART_CR2 0x04
#define USART_CR3 0x08
#define USART_BRR 0x0c
#define USART_GTPR 0x10
#define USART_RTOR 0x14
#define USART_RQR 0x18
#define USART_ISR 0x1c
#define USART_ICR 0x20
#define USART_RDR 0x24
#define USART_TDR 0x28
#define USART_PRESC 0x2c

#define CR1_FIFO_POLICY_MASK 0xe0000000U
#define CR1_FIFOEN (1U << 29)
#define CR1_M1 (1U << 28)
#define CR1_DEAT_DEDT_MASK (0x3ffU << 16)
#define CR1_OVER8 (1U << 15)
#define CR1_M0 (1U << 12)
#define CR1_WAKE (1U << 11)
#define CR1_PCE (1U << 10)
#define CR1_PS (1U << 9)
#define CR1_TXEIE (1U << 7)
#define CR1_TCIE (1U << 6)
#define CR1_RXNEIE (1U << 5)
#define CR1_TE (1U << 3)
#define CR1_RE (1U << 2)
#define CR1_UE (1U << 0)
#define CR1_UE_LOCK_MASK \
    (CR1_FIFOEN | CR1_M1 | CR1_DEAT_DEDT_MASK | CR1_OVER8 | \
     CR1_M0 | CR1_WAKE | CR1_PCE | CR1_PS)
#define CR1_DYNAMIC_IRQ_MASK (CR1_TXEIE | CR1_TCIE | CR1_RXNEIE)

#define CR2_ADD_MASK (0xffU << 24)
#define CR2_ABRMOD_MASK (3U << 21)
#define CR2_ABREN (1U << 20)
#define CR2_STOP_MASK (3U << 12)
#define CR2_STOP_2 (2U << 12)
#define CR2_TE_LOCK_MASK (7U << 8)
#define CR2_LBDIE (1U << 6)
#define CR2_UE_LOCK_MASK ((0xfffU << 8) | (3U << 4))

#define CR3_FIFO_POLICY_MASK 0xfe800000U
#define CR3_SCARCNT_MASK (7U << 17)
#define CR3_EIE (1U << 0)
#define CR3_UE_LOCK_MASK \
    ((3U << 20) | (0x1fU << 11) | (3U << 8) | (0x1fU << 1))

#define RQR_RXFRQ (1U << 3)
#define ISR_REACK (1U << 22)
#define ISR_TEACK (1U << 21)
#define ISR_RXNE (1U << 5)
#define ISR_ORE (1U << 3)
#define ISR_TXE (1U << 7)
#define ISR_TC (1U << 6)
#define ISR_RESET (ISR_TXE | ISR_TC)
#define ICR_TCCF (1U << 6)

#define RCC_APB1RSTR1 0x38
#define RCC_APB2RSTR 0x40
#define RCC_APB1ENR1 0x58
#define RCC_APB2ENR 0x60
#define RCC_CCIPR 0x88

#define RCC_APB2_USART1 (1U << 14)
#define RCC_APB1_USART2 (1U << 17)
#define RCC_APB1_UART4 (1U << 19)

#define RCC_CCIPR_PCLK 0U
#define RCC_CCIPR_SYSCLK 1U
#define RCC_CCIPR_HSI16 2U
#define RCC_CCIPR_LSE 3U

#define NVIC_ISER1 0xe000e104ULL
#define NVIC_ISPR1 0xe000e204ULL
#define NVIC_ICPR1 0xe000e284ULL

#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)
#define WAIT_TIMEOUT_US (5 * G_TIME_SPAN_SECOND)
#define NO_EVENT_TIMEOUT_US (100 * G_TIME_SPAN_MILLISECOND)

typedef enum Stm32g474SerialId {
    SERIAL_UART4,
    SERIAL_USART2,
    SERIAL_USART1,
    SERIAL_COUNT,
} Stm32g474SerialId;

typedef struct SerialInstance {
    const char *name;
    uint64_t base;
    const char *clock_path;
    uint32_t gate_reg;
    uint32_t gate_bit;
    uint32_t reset_reg;
    uint32_t reset_bit;
    unsigned int ccipr_shift;
    unsigned int irq;
    unsigned int serial_index;
    uint32_t cr1_direct_mask;
    uint32_t cr2_mask;
    uint32_t cr3_direct_mask;
    uint32_t gtpr_mask;
    bool is_usart;
} SerialInstance;

typedef struct SerialLockRegister {
    uint32_t offset;
    uint32_t lock_mask;
    uint32_t dynamic_irq_mask;
} SerialLockRegister;

static const SerialLockRegister ue_lock_registers[] = {
    {
        .offset = USART_CR1,
        .lock_mask = CR1_UE_LOCK_MASK,
        .dynamic_irq_mask = CR1_DYNAMIC_IRQ_MASK,
    }, {
        .offset = USART_CR2,
        .lock_mask = CR2_UE_LOCK_MASK,
        .dynamic_irq_mask = CR2_LBDIE,
    }, {
        .offset = USART_CR3,
        .lock_mask = CR3_UE_LOCK_MASK,
        .dynamic_irq_mask = CR3_EIE,
    },
};

static const SerialInstance serial_instances[SERIAL_COUNT] = {
    [SERIAL_UART4] = {
        .name = "UART4",
        .base = UART4_BASE,
        .clock_path = "/machine/mcu/uart4/clk",
        .gate_reg = RCC_APB1ENR1,
        .gate_bit = RCC_APB1_UART4,
        .reset_reg = RCC_APB1RSTR1,
        .reset_bit = RCC_APB1_UART4,
        .ccipr_shift = 6,
        .irq = 52,
        .serial_index = 0,
        .cr1_direct_mask = 0x17ffffff,
        .cr2_mask = 0xfffff070,
        .cr3_direct_mask = 0x0070ffcf,
        .gtpr_mask = 0x000000ff,
        .is_usart = false,
    },
    [SERIAL_USART2] = {
        .name = "USART2",
        .base = USART2_BASE,
        .clock_path = "/machine/mcu/usart2/clk",
        .gate_reg = RCC_APB1ENR1,
        .gate_bit = RCC_APB1_USART2,
        .reset_reg = RCC_APB1RSTR1,
        .reset_bit = RCC_APB1_USART2,
        .ccipr_shift = 2,
        .irq = 38,
        .serial_index = 1,
        .cr1_direct_mask = 0x1fffffff,
        .cr2_mask = 0xffffff79,
        .cr3_direct_mask = 0x017effff,
        .gtpr_mask = 0x0000ffff,
        .is_usart = true,
    },
    [SERIAL_USART1] = {
        .name = "USART1",
        .base = USART1_BASE,
        .clock_path = "/machine/mcu/usart1/clk",
        .gate_reg = RCC_APB2ENR,
        .gate_bit = RCC_APB2_USART1,
        .reset_reg = RCC_APB2RSTR,
        .reset_bit = RCC_APB2_USART1,
        .ccipr_shift = 0,
        .irq = 37,
        .serial_index = 2,
        .cr1_direct_mask = 0x1fffffff,
        .cr2_mask = 0xffffff79,
        .cr3_direct_mask = 0x017effff,
        .gtpr_mask = 0x0000ffff,
        .is_usart = true,
    },
};

static QTestState *stm32g474_qtest_start(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null");
}

static uint32_t serial_readl(QTestState *qts, const SerialInstance *serial,
                             uint32_t offset)
{
    return qtest_readl(qts, serial->base + offset);
}

static void serial_writel(QTestState *qts, const SerialInstance *serial,
                          uint32_t offset, uint32_t value)
{
    qtest_writel(qts, serial->base + offset, value);
}

static uint32_t serial_register_mask(const SerialInstance *serial,
                                     uint32_t offset)
{
    switch (offset) {
    case USART_CR1:
        return serial->cr1_direct_mask;
    case USART_CR2:
        return serial->cr2_mask;
    case USART_CR3:
        return serial->cr3_direct_mask;
    default:
        g_assert_not_reached();
    }
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static void serial_set_gate(QTestState *qts,
                            const SerialInstance *serial, bool enabled)
{
    uint32_t value = rcc_readl(qts, serial->gate_reg);

    value = enabled ? value | serial->gate_bit : value & ~serial->gate_bit;
    rcc_writel(qts, serial->gate_reg, value);
}

static void serial_select_clock(QTestState *qts,
                                const SerialInstance *serial,
                                uint32_t selector)
{
    uint32_t value = rcc_readl(qts, RCC_CCIPR);
    uint32_t mask = 3U << serial->ccipr_shift;

    value = (value & ~mask) | (selector << serial->ccipr_shift);
    rcc_writel(qts, RCC_CCIPR, value);
}

static uint64_t clock_period_from_hz(uint64_t hz)
{
    return hz ? CLOCK_PERIOD_1SEC / hz : 0;
}

static uint64_t serial_clock_period(QTestState *qts,
                                    const SerialInstance *serial)
{
    QDict *response;
    uint64_t period;

    response = qtest_qmp(qts,
        "{ 'execute': 'qom-get', 'arguments': { 'path': %s, "
        "'property': 'qtest-clock-period' } }", serial->clock_path);
    g_assert_false(qdict_haskey(response, "error"));
    period = qdict_get_int(response, "return");
    qobject_unref(response);

    return period;
}

static void assert_serial_clock_hz(QTestState *qts,
                                   const SerialInstance *serial,
                                   uint64_t hz)
{
    g_test_message("%s clock expected %" PRIu64 " Hz",
                   serial->name, hz);
    g_assert_cmphex(serial_clock_period(qts, serial), ==,
                    clock_period_from_hz(hz));
}

static uint32_t wait_serial_status(QTestState *qts,
                                   const SerialInstance *serial,
                                   uint32_t mask, uint32_t expected,
                                   const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_TIMEOUT_US;
    uint32_t isr;

    do {
        isr = serial_readl(qts, serial, USART_ISR);
        if ((isr & mask) == expected) {
            return isr;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_error("%s timed out for %s: CR1=0x%08x ISR=0x%08x",
            serial->name, operation,
            serial_readl(qts, serial, USART_CR1), isr);
}

static void assert_serial_status_stays(QTestState *qts,
                                       const SerialInstance *serial,
                                       uint32_t mask, uint32_t expected,
                                       const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + NO_EVENT_TIMEOUT_US;
    uint32_t isr;

    do {
        isr = serial_readl(qts, serial, USART_ISR);
        if ((isr & mask) != expected) {
            g_error("%s changed status during %s: "
                    "CR1=0x%08x ISR=0x%08x",
                    serial->name, operation,
                    serial_readl(qts, serial, USART_CR1), isr);
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static uint32_t serial_irq_bit(const SerialInstance *serial)
{
    g_assert_cmpuint(serial->irq, >=, 32);
    g_assert_cmpuint(serial->irq, <, 64);
    return 1U << (serial->irq - 32);
}

static bool serial_irq_pending(QTestState *qts,
                               const SerialInstance *serial)
{
    return qtest_readl(qts, NVIC_ISPR1) & serial_irq_bit(serial);
}

static void serial_irq_enable(QTestState *qts,
                              const SerialInstance *serial)
{
    qtest_writel(qts, NVIC_ISER1, serial_irq_bit(serial));
}

static void serial_irq_clear(QTestState *qts,
                             const SerialInstance *serial)
{
    qtest_writel(qts, NVIC_ICPR1, serial_irq_bit(serial));
}

static void wait_serial_irq(QTestState *qts,
                            const SerialInstance *serial, bool pending,
                            const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_TIMEOUT_US;
    bool observed;

    do {
        observed = serial_irq_pending(qts, serial);
        if (observed == pending) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_error("%s timed out for %s IRQ %u=%u: CR1=0x%08x ISR=0x%08x",
            serial->name, operation, serial->irq, observed,
            serial_readl(qts, serial, USART_CR1),
            serial_readl(qts, serial, USART_ISR));
}

static void assert_serial_irq_stays(QTestState *qts,
                                    const SerialInstance *serial,
                                    bool pending, const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + NO_EVENT_TIMEOUT_US;

    do {
        if (serial_irq_pending(qts, serial) != pending) {
            g_error("%s changed IRQ %u during %s: "
                    "CR1=0x%08x ISR=0x%08x",
                    serial->name, serial->irq, operation,
                    serial_readl(qts, serial, USART_CR1),
                    serial_readl(qts, serial, USART_ISR));
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void socket_send_bytes(QTestState *qts,
                              const SerialInstance *serial, int sock_fd,
                              const uint8_t *bytes, size_t length,
                              const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_TIMEOUT_US;
    size_t sent = 0;

    while (sent < length) {
        ssize_t ret = send(sock_fd, bytes + sent, length - sent, 0);

        if (ret > 0) {
            sent += ret;
            continue;
        }
        if (ret == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                         errno != EINTR)) {
            g_error("%s socket send failed for %s: %s; "
                    "CR1=0x%08x ISR=0x%08x",
                    serial->name, operation,
                    ret == 0 ? "closed" : g_strerror(errno),
                    serial_readl(qts, serial, USART_CR1),
                    serial_readl(qts, serial, USART_ISR));
        }
        if (g_get_monotonic_time() >= deadline) {
            g_error("%s socket send timed out for %s: "
                    "CR1=0x%08x ISR=0x%08x",
                    serial->name, operation,
                    serial_readl(qts, serial, USART_CR1),
                    serial_readl(qts, serial, USART_ISR));
        }
        g_usleep(1000);
    }
}

static uint8_t socket_receive_byte(QTestState *qts,
                                   const SerialInstance *serial, int sock_fd,
                                   const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_TIMEOUT_US;
    uint8_t byte;

    do {
        ssize_t ret = recv(sock_fd, &byte, sizeof(byte), 0);

        if (ret == 1) {
            return byte;
        }
        if (ret == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                         errno != EINTR)) {
            g_error("%s socket receive failed for %s: %s; "
                    "CR1=0x%08x ISR=0x%08x",
                    serial->name, operation,
                    ret == 0 ? "closed" : g_strerror(errno),
                    serial_readl(qts, serial, USART_CR1),
                    serial_readl(qts, serial, USART_ISR));
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_error("%s socket receive timed out for %s: "
            "CR1=0x%08x ISR=0x%08x",
            serial->name, operation,
            serial_readl(qts, serial, USART_CR1),
            serial_readl(qts, serial, USART_ISR));
}

static void socket_expect_no_byte(QTestState *qts,
                                  const SerialInstance *serial, int sock_fd,
                                  const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + NO_EVENT_TIMEOUT_US;
    uint8_t byte;

    do {
        ssize_t ret = recv(sock_fd, &byte, sizeof(byte), 0);

        if (ret == 1) {
            g_error("%s received unexpected byte 0x%02x for %s: "
                    "CR1=0x%08x ISR=0x%08x",
                    serial->name, byte, operation,
                    serial_readl(qts, serial, USART_CR1),
                    serial_readl(qts, serial, USART_ISR));
        }
        if (ret == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                         errno != EINTR)) {
            g_error("%s socket check failed for %s: %s; "
                    "CR1=0x%08x ISR=0x%08x",
                    serial->name, operation,
                    ret == 0 ? "closed" : g_strerror(errno),
                    serial_readl(qts, serial, USART_CR1),
                    serial_readl(qts, serial, USART_ISR));
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void assert_serial_reset(QTestState *qts,
                                const SerialInstance *serial)
{
    static const uint32_t zero_registers[] = {
        USART_CR1, USART_CR2, USART_CR3, USART_BRR, USART_GTPR,
        USART_RTOR, USART_RQR, USART_ICR, USART_RDR, USART_TDR,
        USART_PRESC,
    };

    for (size_t i = 0; i < ARRAY_SIZE(zero_registers); i++) {
        g_assert_cmphex(serial_readl(qts, serial, zero_registers[i]),
                        ==, 0);
    }
    g_assert_cmphex(serial_readl(qts, serial, USART_ISR), ==, ISR_RESET);
}

static void assert_serial_masks(QTestState *qts,
                                const SerialInstance *serial)
{
    uint32_t cr1_expected = serial->cr1_direct_mask & ~CR1_UE;

    qtest_writeb(qts, serial->base + USART_BRR, UINT8_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 0);
    qtest_writew(qts, serial->base + USART_BRR, UINT16_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 0);
    qtest_writel(qts, serial->base + USART_BRR + 1, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 0);

    serial_writel(qts, serial, USART_CR1, UINT32_MAX & ~CR1_UE);
    g_assert_cmphex(serial_readl(qts, serial, USART_CR1), ==,
                    cr1_expected);
    g_assert_cmphex(serial_readl(qts, serial, USART_CR1) &
                    CR1_FIFO_POLICY_MASK, ==, 0);

    serial_writel(qts, serial, USART_CR1,
                  serial_readl(qts, serial, USART_CR1) & ~CR1_TE);
    serial_writel(qts, serial, USART_CR2, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_CR2), ==,
                    serial->cr2_mask);

    serial_writel(qts, serial, USART_CR3, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_CR3), ==,
                    serial->cr3_direct_mask);
    g_assert_cmphex(serial_readl(qts, serial, USART_CR3) &
                    CR3_FIFO_POLICY_MASK, ==, 0);

    serial_writel(qts, serial, USART_BRR, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 0x0000ffff);
    serial_writel(qts, serial, USART_GTPR, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_GTPR), ==,
                    serial->gtpr_mask);
    serial_writel(qts, serial, USART_RTOR, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_RTOR), ==, UINT32_MAX);

    serial_writel(qts, serial, USART_RQR, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_RQR), ==, 0);
    serial_writel(qts, serial, USART_ICR, 1);
    g_assert_cmphex(serial_readl(qts, serial, USART_ICR), ==, 0);

    serial_writel(qts, serial, USART_ISR, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_ISR), ==, ISR_RESET);
    serial_writel(qts, serial, USART_RDR, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_RDR), ==, 0);
    serial_writel(qts, serial, USART_TDR, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_TDR), ==, 0);

    serial_writel(qts, serial, USART_PRESC, UINT32_MAX);
    g_assert_cmphex(serial_readl(qts, serial, USART_PRESC), ==, 0xb);
}

static void dirty_serial_for_system_reset(QTestState *qts,
                                          const SerialInstance *serial)
{
    serial_writel(qts, serial, USART_CR2, CR2_STOP_2);
    serial_writel(qts, serial, USART_CR3, CR3_EIE);
    serial_writel(qts, serial, USART_BRR, 0x1234);
    serial_writel(qts, serial, USART_GTPR, 0x55aa);
    serial_writel(qts, serial, USART_RTOR, 0x12345678);
    serial_writel(qts, serial, USART_PRESC, 3);
    serial_writel(qts, serial, USART_CR1, CR1_TE | CR1_RE);
}

static void test_reset_variants_masks(void)
{
    QTestState *qts = stm32g474_qtest_start();
    const SerialInstance *uart4 = &serial_instances[SERIAL_UART4];
    uint32_t isr;

    isr = serial_readl(qts, uart4, USART_ISR);
    g_test_message("UART4_ISR expected 0x%08x, observed 0x%08x",
                   ISR_RESET, isr);
    g_assert_cmphex(isr, ==, ISR_RESET);

    for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
        assert_serial_reset(qts, &serial_instances[i]);
        assert_serial_masks(qts, &serial_instances[i]);
    }

    for (unsigned int reset = 0; reset < 2; reset++) {
        for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
            dirty_serial_for_system_reset(qts, &serial_instances[i]);
        }
        qtest_system_reset(qts);
        for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
            assert_serial_reset(qts, &serial_instances[i]);
        }
    }

    qtest_quit(qts);
}

static void test_enable_ack_locking(void)
{
    QTestState *qts = stm32g474_qtest_start();

    for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
        const SerialInstance *serial = &serial_instances[i];
        uint32_t expected_locked[ARRAY_SIZE(ue_lock_registers)];
        uint32_t preserved[ARRAY_SIZE(ue_lock_registers)];
        uint32_t preserved_brr;
        uint32_t preserved_gtpr;
        uint32_t preserved_rtor;
        uint32_t preserved_presc;
        uint32_t value;

        serial_set_gate(qts, serial, true);
        for (size_t reg = 0; reg < ARRAY_SIZE(ue_lock_registers); reg++) {
            const SerialLockRegister *lock = &ue_lock_registers[reg];
            uint32_t physical_mask =
                serial_register_mask(serial, lock->offset);

            expected_locked[reg] = lock->lock_mask & physical_mask;
            value = expected_locked[reg];
            if (lock->offset == USART_CR3 && serial->is_usart) {
                value |= 3U << 17;
            }
            serial_writel(qts, serial, lock->offset, value);
            g_assert_cmphex(serial_readl(qts, serial, lock->offset) &
                            lock->lock_mask, ==, expected_locked[reg]);
        }

        value = serial_readl(qts, serial, USART_CR1) | CR1_TE | CR1_RE;
        serial_writel(qts, serial, USART_CR1, value);
        g_assert_cmphex(serial_readl(qts, serial, USART_ISR) &
                        (ISR_TEACK | ISR_REACK), ==, 0);

        serial_writel(qts, serial, USART_BRR, 0x1234);
        serial_writel(qts, serial, USART_GTPR, 0x55aa);
        serial_writel(qts, serial, USART_RTOR, 0x12345678);
        serial_writel(qts, serial, USART_PRESC, 3);
        serial_writel(qts, serial, USART_CR1,
                      serial_readl(qts, serial, USART_CR1) | CR1_UE);
        g_assert_cmphex(serial_readl(qts, serial, USART_ISR) &
                        (ISR_TEACK | ISR_REACK), ==,
                        ISR_TEACK | ISR_REACK);

        for (size_t reg = 0; reg < ARRAY_SIZE(ue_lock_registers); reg++) {
            const SerialLockRegister *lock = &ue_lock_registers[reg];

            value = lock->dynamic_irq_mask;
            if (lock->offset == USART_CR1) {
                value |= CR1_UE | CR1_TE | CR1_RE;
            }
            serial_writel(qts, serial, lock->offset, value);
            value = serial_readl(qts, serial, lock->offset);
            g_assert_cmphex(value & lock->lock_mask, ==,
                            expected_locked[reg]);
            g_assert_cmphex(value & lock->dynamic_irq_mask, ==,
                            lock->dynamic_irq_mask);
        }

        serial_writel(qts, serial, USART_BRR, 0x4321);
        g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 0x1234);
        serial_writel(qts, serial, USART_PRESC, 7);
        g_assert_cmphex(serial_readl(qts, serial, USART_PRESC), ==, 3);
        serial_writel(qts, serial, USART_GTPR, 0);
        g_assert_cmphex(serial_readl(qts, serial, USART_GTPR), ==,
                        0x55aa & serial->gtpr_mask);
        serial_writel(qts, serial, USART_RTOR, 0x87654321);
        g_assert_cmphex(serial_readl(qts, serial, USART_RTOR), ==,
                        0x87654321);

        value = serial_readl(qts, serial, USART_CR1) & ~CR1_RE;
        serial_writel(qts, serial, USART_CR1, value);
        value = serial_readl(qts, serial, USART_CR2);
        serial_writel(qts, serial, USART_CR2,
                      (value & ~CR2_ADD_MASK) | (0x5aU << 24));
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                        CR2_ADD_MASK, ==, 0x5aU << 24);
        serial_writel(qts, serial, USART_CR1,
                      serial_readl(qts, serial, USART_CR1) | CR1_RE);
        value = serial_readl(qts, serial, USART_CR2);
        serial_writel(qts, serial, USART_CR2,
                      (value & ~CR2_ADD_MASK) | (0xa5U << 24));
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                        CR2_ADD_MASK, ==, 0x5aU << 24);

        value = serial_readl(qts, serial, USART_CR2);
        serial_writel(qts, serial, USART_CR2,
                      (value & ~(CR2_ABRMOD_MASK | CR2_ABREN)) |
                      (1U << 21));
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                        CR2_ABRMOD_MASK, ==, 1U << 21);
        serial_writel(qts, serial, USART_CR2,
                      serial_readl(qts, serial, USART_CR2) | CR2_ABREN);
        value = serial_readl(qts, serial, USART_CR2);
        serial_writel(qts, serial, USART_CR2,
                      (value & ~CR2_ABRMOD_MASK) | (2U << 21));
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                        CR2_ABRMOD_MASK, ==, 1U << 21);

        if (serial->is_usart) {
            g_assert_cmphex(serial_readl(qts, serial, USART_CR3) &
                            CR3_SCARCNT_MASK, ==, 0);
            serial_writel(qts, serial, USART_CR3,
                          serial_readl(qts, serial, USART_CR3) |
                          (5U << 17));
            g_assert_cmphex(serial_readl(qts, serial, USART_CR3) &
                            CR3_SCARCNT_MASK, ==, 0);
        }

        for (size_t reg = 0; reg < ARRAY_SIZE(ue_lock_registers); reg++) {
            preserved[reg] = serial_readl(
                qts, serial, ue_lock_registers[reg].offset);
        }
        preserved_brr = serial_readl(qts, serial, USART_BRR);
        preserved_gtpr = serial_readl(qts, serial, USART_GTPR);
        preserved_rtor = serial_readl(qts, serial, USART_RTOR);
        preserved_presc = serial_readl(qts, serial, USART_PRESC);

        serial_writel(qts, serial, USART_CR1,
                      preserved[0] & ~CR1_UE);
        g_assert_cmphex(serial_readl(qts, serial, USART_CR1), ==,
                        preserved[0] & ~CR1_UE);
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2), ==,
                        preserved[1]);
        g_assert_cmphex(serial_readl(qts, serial, USART_CR3), ==,
                        preserved[2]);
        g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==,
                        preserved_brr);
        g_assert_cmphex(serial_readl(qts, serial, USART_GTPR), ==,
                        preserved_gtpr);
        g_assert_cmphex(serial_readl(qts, serial, USART_RTOR), ==,
                        preserved_rtor);
        g_assert_cmphex(serial_readl(qts, serial, USART_PRESC), ==,
                        preserved_presc);
        g_assert_cmphex(serial_readl(qts, serial, USART_ISR), ==, ISR_RESET);

        value = CR2_TE_LOCK_MASK & serial->cr2_mask;
        serial_writel(qts, serial, USART_CR2, CR2_LBDIE);
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                        CR2_TE_LOCK_MASK, ==, value);
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                        ((CR2_UE_LOCK_MASK & serial->cr2_mask) &
                         ~CR2_TE_LOCK_MASK), ==, 0);
        serial_writel(qts, serial, USART_CR1,
                      serial_readl(qts, serial, USART_CR1) & ~CR1_TE);
        serial_writel(qts, serial, USART_CR2, 0);
        g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                        CR2_TE_LOCK_MASK, ==, 0);

        serial_writel(qts, serial, USART_CR1, 0);
        for (uint32_t presc = 0xc; presc <= 0xf; presc++) {
            serial_writel(qts, serial, USART_PRESC, presc);
            g_assert_cmphex(serial_readl(qts, serial, USART_PRESC), ==, 0xb);
        }

        serial_writel(qts, serial, USART_BRR, 0);
        g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 0);
        serial_writel(qts, serial, USART_CR1, CR1_UE | CR1_OVER8);
        serial_writel(qts, serial, USART_CR1, 0);
        serial_writel(qts, serial, USART_BRR, 8);
        serial_writel(qts, serial, USART_CR1, CR1_UE | CR1_OVER8);
        g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 8);
        g_assert_cmphex(serial_readl(qts, serial, USART_CR1) & CR1_OVER8,
                        ==, CR1_OVER8);
        serial_writel(qts, serial, USART_CR1, 0);
        g_assert_cmphex(serial_readl(qts, serial, USART_ISR), ==, ISR_RESET);
    }

    qtest_quit(qts);
}

static void dirty_serial_for_rcc_reset(QTestState *qts,
                                       const SerialInstance *serial)
{
    serial_set_gate(qts, serial, true);
    serial_select_clock(qts, serial, RCC_CCIPR_LSE);
    serial_irq_enable(qts, serial);
    serial_writel(qts, serial, USART_CR1, 0);
    serial_writel(qts, serial, USART_CR2, CR2_STOP_2);
    serial_writel(qts, serial, USART_CR3, CR3_EIE);
    serial_writel(qts, serial, USART_BRR, 0x2345);
    serial_writel(qts, serial, USART_GTPR, 0x5a);
    serial_writel(qts, serial, USART_RTOR, 0x87654321);
    serial_writel(qts, serial, USART_PRESC, 4);
    serial_writel(qts, serial, USART_CR1,
                  CR1_UE | CR1_TE | CR1_TXEIE);
    serial_writel(qts, serial, USART_TDR, 0x100 | serial->serial_index);
    g_assert_cmphex(serial_readl(qts, serial, USART_ISR) &
                    (ISR_TXE | ISR_TC), ==, 0);
    serial_irq_clear(qts, serial);
    wait_serial_irq(qts, serial, false, "zero-clock TDR IRQ deassertion");
}

static void assert_serial_dirty_after_peer_reset(
    QTestState *qts, const SerialInstance *serial)
{
    g_assert_cmphex(serial_readl(qts, serial, USART_CR1) &
                    (CR1_UE | CR1_TE | CR1_TXEIE), ==,
                    CR1_UE | CR1_TE | CR1_TXEIE);
    g_assert_cmphex(serial_readl(qts, serial, USART_CR2) &
                    CR2_STOP_MASK, ==, CR2_STOP_2);
    g_assert_cmphex(serial_readl(qts, serial, USART_CR3) & CR3_EIE,
                    ==, CR3_EIE);
    g_assert_cmphex(serial_readl(qts, serial, USART_BRR), ==, 0x2345);
    g_assert_cmphex(serial_readl(qts, serial, USART_GTPR), ==, 0x5a);
    g_assert_cmphex(serial_readl(qts, serial, USART_RTOR), ==, 0x87654321);
    g_assert_cmphex(serial_readl(qts, serial, USART_PRESC), ==, 4);
    g_assert_cmphex(serial_readl(qts, serial, USART_ISR) &
                    (ISR_TXE | ISR_TC), ==, 0);
}

static void test_rcc_clock_reset_coupling(void)
{
    const SerialInstance *uart4 = &serial_instances[SERIAL_UART4];
    QTestState *qts;
    int sock_fd;

    qts = qtest_init_with_serial("-M " STM32G474_MACHINE, &sock_fd);
    qemu_set_blocking(sock_fd, false, &error_abort);

    for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
        assert_serial_clock_hz(qts, &serial_instances[i], 0);
    }

    for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
        const SerialInstance *serial = &serial_instances[i];

        serial_set_gate(qts, serial, true);
        assert_serial_clock_hz(qts, serial, 16000000);
        serial_select_clock(qts, serial, RCC_CCIPR_HSI16);
        assert_serial_clock_hz(qts, serial, 16000000);
        serial_select_clock(qts, serial, RCC_CCIPR_LSE);
        assert_serial_clock_hz(qts, serial, 0);

        serial_writel(qts, serial, USART_BRR, 0x8b);
        serial_writel(qts, serial, USART_PRESC, 0);
        serial_writel(qts, serial, USART_TDR, UINT32_MAX);
        g_assert_cmphex(serial_readl(qts, serial, USART_TDR), ==, 0);
        g_assert_cmphex(serial_readl(qts, serial, USART_ISR), ==, ISR_RESET);
        serial_writel(qts, serial, USART_CR1, CR1_UE | CR1_TE);
        serial_writel(qts, serial, USART_TDR, UINT32_MAX);
        g_assert_cmphex(serial_readl(qts, serial, USART_TDR), ==, 0x1ff);
        g_assert_cmphex(serial_readl(qts, serial, USART_ISR) &
                        (ISR_TXE | ISR_TC), ==, 0);
        serial_select_clock(qts, serial, RCC_CCIPR_HSI16);
        wait_serial_status(qts, serial, ISR_TXE | ISR_TC,
                           ISR_TXE | ISR_TC, "HSI16 TDR drain");
        if (serial == uart4) {
            g_assert_cmphex(socket_receive_byte(qts, serial, sock_fd,
                                                "HSI16 TDR drain"),
                            ==, 0xff);
        }
    }

    serial_select_clock(qts, uart4, RCC_CCIPR_LSE);
    serial_writel(qts, uart4, USART_CR1, 0);
    serial_writel(qts, uart4, USART_BRR, 0x8b);
    serial_writel(qts, uart4, USART_CR1, CR1_UE | CR1_TE);
    serial_writel(qts, uart4, USART_TDR, 0x6d);
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) &
                    (ISR_TXE | ISR_TC), ==, 0);
    serial_writel(qts, uart4, USART_CR1, CR1_UE);
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) &
                    (ISR_TXE | ISR_TC | ISR_TEACK), ==,
                    ISR_TXE | ISR_TC);
    serial_select_clock(qts, uart4, RCC_CCIPR_HSI16);
    socket_expect_no_byte(qts, uart4, sock_fd,
                          "TE-clear cancellation after clock restore");

    for (size_t target = 0; target < ARRAY_SIZE(serial_instances); target++) {
        const SerialInstance *reset_serial = &serial_instances[target];
        uint32_t reset_value;
        uint8_t rx_byte = 0xa0 + target;

        qtest_system_reset(qts);
        for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
            dirty_serial_for_rcc_reset(qts, &serial_instances[i]);
        }

        serial_select_clock(qts, uart4, RCC_CCIPR_HSI16);
        wait_serial_status(qts, uart4, ISR_TXE | ISR_TC,
                           ISR_TXE | ISR_TC,
                           "UART4 pre-reset pending TDR drain");
        g_assert_cmphex(socket_receive_byte(
                            qts, uart4, sock_fd,
                            "UART4 pre-reset pending TDR drain"),
                        ==, uart4->serial_index);
        serial_writel(qts, uart4, USART_CR1,
                      serial_readl(qts, uart4, USART_CR1) |
                      CR1_RE | CR1_RXNEIE);
        socket_send_bytes(qts, uart4, sock_fd, &rx_byte, 1,
                          "RCC reset RX dirt");
        wait_serial_status(qts, uart4, ISR_RXNE, ISR_RXNE,
                           "RCC reset RX dirt");
        serial_select_clock(qts, uart4, RCC_CCIPR_LSE);
        serial_writel(qts, uart4, USART_TDR, 0xb0 + target);
        g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) &
                        (ISR_TXE | ISR_TC), ==, 0);

        reset_value = rcc_readl(qts, reset_serial->reset_reg);
        rcc_writel(qts, reset_serial->reset_reg,
                   reset_value | reset_serial->reset_bit);
        assert_serial_reset(qts, reset_serial);
        serial_irq_clear(qts, reset_serial);
        wait_serial_irq(qts, reset_serial, false,
                        "peripheral reset IRQ deassertion");

        dirty_serial_for_system_reset(qts, reset_serial);
        serial_writel(qts, reset_serial, USART_CR1,
                      CR1_UE | CR1_TE | CR1_RE);
        assert_serial_reset(qts, reset_serial);

        for (size_t peer = 0; peer < ARRAY_SIZE(serial_instances); peer++) {
            if (peer != target) {
                assert_serial_dirty_after_peer_reset(
                    qts, &serial_instances[peer]);
            }
        }
        if (reset_serial != uart4) {
            g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) &
                            ISR_RXNE, ==, ISR_RXNE);
            g_assert_cmphex(serial_readl(qts, uart4, USART_RDR), ==,
                            rx_byte);
        }

        rcc_writel(qts, reset_serial->reset_reg,
                   rcc_readl(qts, reset_serial->reset_reg) &
                   ~reset_serial->reset_bit);
        serial_select_clock(qts, reset_serial, RCC_CCIPR_HSI16);
        g_assert_cmphex(serial_readl(qts, reset_serial, USART_TDR), ==, 0);
        g_assert_cmphex(serial_readl(qts, reset_serial, USART_ISR), ==,
                        ISR_RESET);
        if (reset_serial == uart4) {
            socket_expect_no_byte(qts, uart4, sock_fd,
                                  "peripheral-reset pending TDR discard");
        }
        serial_writel(qts, reset_serial, USART_CR2, CR2_STOP_2);
        g_assert_cmphex(serial_readl(qts, reset_serial, USART_CR2) &
                        CR2_STOP_MASK, ==, CR2_STOP_2);
    }

    close(sock_fd);
    qtest_quit(qts);
}

static void test_uart4_serial0_transmit(void)
{
    const SerialInstance *uart4 = &serial_instances[SERIAL_UART4];
    QTestState *qts;
    int sock_fd;

    qts = qtest_init_with_serial("-M " STM32G474_MACHINE, &sock_fd);
    qemu_set_blocking(sock_fd, false, &error_abort);
    serial_set_gate(qts, uart4, true);
    serial_writel(qts, uart4, USART_CR2, 0);
    serial_writel(qts, uart4, USART_BRR, 0x8b);
    serial_writel(qts, uart4, USART_PRESC, 0);
    serial_writel(qts, uart4, USART_CR1, CR1_UE | CR1_TE);
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) & ISR_TEACK,
                    ==, ISR_TEACK);

    serial_writel(qts, uart4, USART_TDR, 0x51);
    g_assert_cmphex(socket_receive_byte(qts, uart4, sock_fd,
                                        "serial0 first transmit"),
                    ==, 0x51);
    wait_serial_status(qts, uart4, ISR_TXE | ISR_TC,
                       ISR_TXE | ISR_TC, "serial0 first transmit");

    serial_writel(qts, uart4, USART_ICR, ICR_TCCF);
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) &
                    (ISR_TXE | ISR_TC), ==, ISR_TXE);
    serial_writel(qts, uart4, USART_TDR, 0xa6);
    g_assert_cmphex(socket_receive_byte(qts, uart4, sock_fd,
                                        "serial0 second transmit"),
                    ==, 0xa6);
    wait_serial_status(qts, uart4, ISR_TXE | ISR_TC,
                       ISR_TXE | ISR_TC, "serial0 second transmit");

    serial_writel(qts, uart4, USART_CR1, CR1_TE);
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR), ==, ISR_RESET);
    serial_writel(qts, uart4, USART_TDR, 0x37);
    socket_expect_no_byte(qts, uart4, sock_fd,
                          "TDR write with UE clear");

    close(sock_fd);
    qtest_quit(qts);
}

static void test_uart4_serial0_receive_irq(void)
{
    const SerialInstance *uart4 = &serial_instances[SERIAL_UART4];
    static const uint8_t queued[] = { 0x31, 0x32 };
    QTestState *qts;
    uint8_t byte;
    int sock_fd;

    qts = qtest_init_with_serial("-M " STM32G474_MACHINE, &sock_fd);
    qemu_set_blocking(sock_fd, false, &error_abort);
    serial_set_gate(qts, uart4, true);
    serial_irq_enable(qts, uart4);
    serial_writel(qts, uart4, USART_BRR, 0x8b);
    serial_writel(qts, uart4, USART_CR1,
                  CR1_UE | CR1_RE | CR1_RXNEIE);

    socket_send_bytes(qts, uart4, sock_fd, queued, sizeof(queued),
                      "two-byte receive queue");
    wait_serial_status(qts, uart4, ISR_RXNE, ISR_RXNE,
                       "first queued receive");
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) & ISR_ORE, ==, 0);
    wait_serial_irq(qts, uart4, true, "IRQ 52 first queued receive");
    g_assert_cmphex(serial_readl(qts, uart4, USART_RDR), ==, queued[0]);
    wait_serial_status(qts, uart4, ISR_RXNE, ISR_RXNE,
                       "second queued receive");
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) & ISR_ORE, ==, 0);
    g_assert_cmphex(serial_readl(qts, uart4, USART_RDR), ==, queued[1]);
    wait_serial_status(qts, uart4, ISR_RXNE, 0,
                       "receive queue drained");
    serial_irq_clear(qts, uart4);
    wait_serial_irq(qts, uart4, false, "IRQ 52 receive queue drained");

    byte = 0x43;
    socket_send_bytes(qts, uart4, sock_fd, &byte, 1,
                      "RXFRQ held receive");
    wait_serial_status(qts, uart4, ISR_RXNE, ISR_RXNE,
                       "RXFRQ held receive");
    serial_writel(qts, uart4, USART_RQR, RQR_RXFRQ);
    wait_serial_status(qts, uart4, ISR_RXNE, 0, "RXFRQ discard");
    serial_irq_clear(qts, uart4);
    wait_serial_irq(qts, uart4, false, "RXFRQ IRQ deassertion");

    byte = 0x44;
    socket_send_bytes(qts, uart4, sock_fd, &byte, 1,
                      "post-RXFRQ receive");
    wait_serial_status(qts, uart4, ISR_RXNE, ISR_RXNE,
                       "post-RXFRQ receive");
    g_assert_cmphex(serial_readl(qts, uart4, USART_RDR), ==, byte);
    serial_irq_clear(qts, uart4);

    serial_writel(qts, uart4, USART_CR1, CR1_UE | CR1_RXNEIE);
    byte = 0x45;
    socket_send_bytes(qts, uart4, sock_fd, &byte, 1,
                      "receive while RE clear");
    assert_serial_status_stays(qts, uart4, ISR_RXNE, 0,
                               "receive while RE clear");
    serial_writel(qts, uart4, USART_CR1,
                  CR1_UE | CR1_RE | CR1_RXNEIE);
    wait_serial_status(qts, uart4, ISR_RXNE, ISR_RXNE,
                       "receive after RE set");
    g_assert_cmphex(serial_readl(qts, uart4, USART_RDR), ==, byte);
    serial_irq_clear(qts, uart4);
    wait_serial_irq(qts, uart4, false, "final RX IRQ deassertion");

    close(sock_fd);
    qtest_quit(qts);
}

static void test_serial_tx_irqs(void)
{
    QTestState *qts = stm32g474_qtest_start();
    uint32_t all_serial_irqs = 0;

    for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
        const SerialInstance *serial = &serial_instances[i];

        g_assert_cmphex(all_serial_irqs & serial_irq_bit(serial), ==, 0);
        all_serial_irqs |= serial_irq_bit(serial);
        serial_set_gate(qts, serial, true);
        serial_irq_enable(qts, serial);
        serial_writel(qts, serial, USART_BRR, 0x8b);
        serial_writel(qts, serial, USART_CR1, CR1_UE | CR1_TE);
    }

    for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
        const SerialInstance *serial = &serial_instances[i];
        uint32_t pending;

        qtest_writel(qts, NVIC_ICPR1, all_serial_irqs);
        serial_writel(qts, serial, USART_CR1,
                      CR1_UE | CR1_TE | CR1_TXEIE);
        wait_serial_irq(qts, serial, true, "TXE interrupt");
        pending = qtest_readl(qts, NVIC_ISPR1) & all_serial_irqs;
        g_assert_cmphex(pending, ==, serial_irq_bit(serial));

        serial_writel(qts, serial, USART_CR1, CR1_UE | CR1_TE);
        serial_irq_clear(qts, serial);
        assert_serial_irq_stays(qts, serial, false,
                                "TXEIE disabled");

        serial_writel(qts, serial, USART_CR1,
                      CR1_UE | CR1_TE | CR1_TCIE);
        wait_serial_irq(qts, serial, true, "TC interrupt");
        pending = qtest_readl(qts, NVIC_ISPR1) & all_serial_irqs;
        g_assert_cmphex(pending, ==, serial_irq_bit(serial));

        serial_writel(qts, serial, USART_ICR, ICR_TCCF);
        g_assert_cmphex(serial_readl(qts, serial, USART_ISR) &
                        (ISR_TXE | ISR_TC), ==, ISR_TXE);
        serial_irq_clear(qts, serial);
        assert_serial_irq_stays(qts, serial, false,
                                "TCCF interrupt clear");
        serial_writel(qts, serial, USART_CR1, 0);
    }

    qtest_quit(qts);
}

static void dirty_serial_for_repeated_reset(
    QTestState *qts, const SerialInstance *serial, unsigned int index)
{
    serial_set_gate(qts, serial, true);
    serial_select_clock(qts, serial, index % 3);
    serial_irq_enable(qts, serial);
    serial_writel(qts, serial, USART_CR2,
                  CR2_STOP_2 | ((0x20U + index) << 24));
    serial_writel(qts, serial, USART_CR3, CR3_EIE);
    serial_writel(qts, serial, USART_BRR, 0x1100 + index);
    serial_writel(qts, serial, USART_GTPR, 0x30 + index);
    serial_writel(qts, serial, USART_RTOR, 0x10203040 + index);
    serial_writel(qts, serial, USART_PRESC, index);
    serial_writel(qts, serial, USART_CR1,
                  CR1_UE | CR1_TE | CR1_TXEIE);
    wait_serial_irq(qts, serial, true, "pre-system-reset TXE IRQ");
}

static void test_repeated_reset_and_cleanup(void)
{
    const SerialInstance *uart4 = &serial_instances[SERIAL_UART4];
    QTestState *qts;
    int sock_fd;

    qts = qtest_init_with_serial("-M " STM32G474_MACHINE, &sock_fd);
    qemu_set_blocking(sock_fd, false, &error_abort);

    for (unsigned int reset = 0; reset < 2; reset++) {
        for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
            dirty_serial_for_repeated_reset(
                qts, &serial_instances[i], i + reset);
        }
        qtest_system_reset(qts);
        for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
            const SerialInstance *serial = &serial_instances[i];

            assert_serial_reset(qts, serial);
            assert_serial_clock_hz(qts, serial, 0);
            g_assert_false(serial_irq_pending(qts, serial));
        }
    }

    serial_set_gate(qts, uart4, true);
    serial_select_clock(qts, uart4, RCC_CCIPR_LSE);
    serial_writel(qts, uart4, USART_BRR, 0x8b);
    serial_writel(qts, uart4, USART_CR1, CR1_UE | CR1_TE);
    serial_writel(qts, uart4, USART_TDR, 0x71);
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) &
                    (ISR_TXE | ISR_TC), ==, 0);
    serial_writel(qts, uart4, USART_CR1, 0);
    serial_select_clock(qts, uart4, RCC_CCIPR_HSI16);
    socket_expect_no_byte(qts, uart4, sock_fd,
                          "reset-cleanup pending TDR cancellation");

    serial_writel(qts, uart4, USART_CR1, CR1_UE | CR1_TE);
    serial_writel(qts, uart4, USART_TDR, 0x72);
    g_assert_cmphex(socket_receive_byte(qts, uart4, sock_fd,
                                        "reset-cleanup ordinary TX"),
                    ==, 0x72);
    qtest_system_reset(qts);
    assert_serial_reset(qts, uart4);
    socket_expect_no_byte(qts, uart4, sock_fd,
                          "post-system-reset stale callback");

    close(sock_fd);
    qtest_quit(qts);
}

static void configure_migration_source(QTestState *qts)
{
    const SerialInstance *uart4 = &serial_instances[SERIAL_UART4];
    const SerialInstance *usart2 = &serial_instances[SERIAL_USART2];
    const SerialInstance *usart1 = &serial_instances[SERIAL_USART1];

    for (size_t i = 0; i < ARRAY_SIZE(serial_instances); i++) {
        serial_set_gate(qts, &serial_instances[i], true);
        serial_irq_enable(qts, &serial_instances[i]);
    }

    serial_select_clock(qts, uart4, RCC_CCIPR_HSI16);
    serial_writel(qts, uart4, USART_CR2,
                  CR2_STOP_2 | (0x44U << 24));
    serial_writel(qts, uart4, USART_CR3, CR3_EIE);
    serial_writel(qts, uart4, USART_BRR, 0x4444);
    serial_writel(qts, uart4, USART_GTPR, 0x44);
    serial_writel(qts, uart4, USART_RTOR, 0x44444444);
    serial_writel(qts, uart4, USART_PRESC, 4);
    serial_writel(qts, uart4, USART_CR1, CR1_UE | CR1_TE);

    serial_select_clock(qts, usart2, RCC_CCIPR_SYSCLK);
    serial_writel(qts, usart2, USART_CR2,
                  CR2_STOP_2 | (0x22U << 24));
    serial_writel(qts, usart2, USART_BRR, 0x2222);
    serial_writel(qts, usart2, USART_GTPR, 0x2222);
    serial_writel(qts, usart2, USART_RTOR, 0x22222222);
    serial_writel(qts, usart2, USART_PRESC, 2);
    rcc_writel(qts, usart2->reset_reg,
               rcc_readl(qts, usart2->reset_reg) | usart2->reset_bit);
    assert_serial_reset(qts, usart2);

    serial_select_clock(qts, usart1, RCC_CCIPR_PCLK);
    serial_writel(qts, usart1, USART_CR2,
                  CR2_STOP_2 | (0x11U << 24));
    serial_writel(qts, usart1, USART_CR3, CR3_EIE);
    serial_writel(qts, usart1, USART_BRR, 0x1111);
    serial_writel(qts, usart1, USART_GTPR, 0x1111);
    serial_writel(qts, usart1, USART_RTOR, 0x11111111);
    serial_writel(qts, usart1, USART_PRESC, 1);
    serial_writel(qts, usart1, USART_CR1,
                  CR1_UE | CR1_TE | CR1_TXEIE);
    wait_serial_irq(qts, usart1, true, "migration source level IRQ");
}

static void assert_migrated_serial_state(QTestState *qts)
{
    const SerialInstance *uart4 = &serial_instances[SERIAL_UART4];
    const SerialInstance *usart2 = &serial_instances[SERIAL_USART2];
    const SerialInstance *usart1 = &serial_instances[SERIAL_USART1];

    g_assert_cmphex(rcc_readl(qts, RCC_CCIPR) &
                    ((3U << uart4->ccipr_shift) |
                     (3U << usart2->ccipr_shift) |
                     (3U << usart1->ccipr_shift)), ==,
                    (RCC_CCIPR_HSI16 << uart4->ccipr_shift) |
                    (RCC_CCIPR_SYSCLK << usart2->ccipr_shift) |
                    (RCC_CCIPR_PCLK << usart1->ccipr_shift));
    assert_serial_clock_hz(qts, uart4, 16000000);
    assert_serial_clock_hz(qts, usart2, 16000000);
    assert_serial_clock_hz(qts, usart1, 16000000);

    g_assert_cmphex(serial_readl(qts, uart4, USART_CR2), ==,
                    CR2_STOP_2 | (0x44U << 24));
    g_assert_cmphex(serial_readl(qts, uart4, USART_CR3), ==, CR3_EIE);
    g_assert_cmphex(serial_readl(qts, uart4, USART_BRR), ==, 0x4444);
    g_assert_cmphex(serial_readl(qts, uart4, USART_GTPR), ==, 0x44);
    g_assert_cmphex(serial_readl(qts, uart4, USART_RTOR), ==, 0x44444444);
    g_assert_cmphex(serial_readl(qts, uart4, USART_PRESC), ==, 4);
    g_assert_cmphex(serial_readl(qts, uart4, USART_ISR) & ISR_TEACK,
                    ==, ISR_TEACK);

    g_assert_cmphex(rcc_readl(qts, usart2->reset_reg) &
                    usart2->reset_bit, ==, usart2->reset_bit);
    assert_serial_reset(qts, usart2);
    serial_writel(qts, usart2, USART_BRR, 0x7777);
    assert_serial_reset(qts, usart2);

    g_assert_cmphex(serial_readl(qts, usart1, USART_CR2), ==,
                    CR2_STOP_2 | (0x11U << 24));
    g_assert_cmphex(serial_readl(qts, usart1, USART_CR3), ==, CR3_EIE);
    g_assert_cmphex(serial_readl(qts, usart1, USART_BRR), ==, 0x1111);
    g_assert_cmphex(serial_readl(qts, usart1, USART_GTPR), ==, 0x1111);
    g_assert_cmphex(serial_readl(qts, usart1, USART_RTOR), ==, 0x11111111);
    g_assert_cmphex(serial_readl(qts, usart1, USART_PRESC), ==, 1);
    g_assert_cmphex(serial_readl(qts, usart1, USART_ISR) & ISR_TEACK,
                    ==, ISR_TEACK);

    serial_irq_clear(qts, usart1);
    wait_serial_irq(qts, usart1, true,
                    "migration destination level IRQ reconstruction");
    serial_writel(qts, usart1, USART_CR1, CR1_UE | CR1_TE);
    serial_irq_clear(qts, usart1);
    assert_serial_irq_stays(qts, usart1, false,
                            "migration destination IRQ clear");

    rcc_writel(qts, usart2->reset_reg,
               rcc_readl(qts, usart2->reset_reg) & ~usart2->reset_bit);
    serial_writel(qts, usart2, USART_BRR, 0x7777);
    g_assert_cmphex(serial_readl(qts, usart2, USART_BRR), ==, 0x7777);
}

static void test_idle_migration(void)
{
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-usart-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = qtest_init("-M " STM32G474_MACHINE
                     " -serial null -serial null -serial null"
                     " -incoming defer");

    configure_migration_source(src);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    assert_migrated_serial_state(dst);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/usart/1-reset-variants-masks",
                   test_reset_variants_masks);
    qtest_add_func("/stm32g474/usart/2-enable-ack-locking",
                   test_enable_ack_locking);
    qtest_add_func("/stm32g474/usart/3-rcc-clock-reset-coupling",
                   test_rcc_clock_reset_coupling);
    qtest_add_func("/stm32g474/usart/4-uart4-serial0-transmit",
                   test_uart4_serial0_transmit);
    qtest_add_func("/stm32g474/usart/5-uart4-serial0-receive-irq",
                   test_uart4_serial0_receive_irq);
    qtest_add_func("/stm32g474/usart/6-tx-irqs",
                   test_serial_tx_irqs);
    qtest_add_func("/stm32g474/usart/7-repeated-reset-cleanup",
                   test_repeated_reset_and_cleanup);
    qtest_add_func("/stm32g474/usart/8-idle-migration",
                   test_idle_migration);

    return g_test_run();
}

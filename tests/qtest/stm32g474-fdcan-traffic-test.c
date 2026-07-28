/*
 * QTest for STM32G474 FDCAN traffic
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

#define STM32G474_MACHINE "stm32g474"

#define FDCAN1_BASE 0x40006400ULL
#define FDCAN2_BASE 0x40006800ULL
#define FDCAN3_BASE 0x40006c00ULL
#define FDCAN_MRAM_BASE 0x4000a400ULL
#define FDCAN_MRAM_SLICE_SIZE 0x350

#define FDCAN_TEST 0x010
#define FDCAN_CCCR 0x018
#define FDCAN_IR 0x050
#define FDCAN_IE 0x054
#define FDCAN_ILS 0x058
#define FDCAN_ILE 0x05c
#define FDCAN_RXGFC 0x080
#define FDCAN_RXF0S 0x090
#define FDCAN_TXBC 0x0c0
#define FDCAN_TXFQS 0x0c4
#define FDCAN_TXBRP 0x0c8
#define FDCAN_TXBAR 0x0cc
#define FDCAN_TXBCR 0x0d0
#define FDCAN_TXBTO 0x0d4
#define FDCAN_TXBCF 0x0d8
#define FDCAN_TXBTIE 0x0dc
#define FDCAN_TXEFS 0x0e4
#define FDCAN_TXEFA 0x0e8
#define FDCAN_CKDIV 0x100

#define FDCAN_CCCR_INIT BIT(0)
#define FDCAN_CCCR_CCE BIT(1)
#define FDCAN_CCCR_MON BIT(5)
#define FDCAN_CCCR_TEST BIT(7)
#define FDCAN_CCCR_FDOE BIT(8)
#define FDCAN_CCCR_BRSE BIT(9)
#define FDCAN_TEST_LBCK BIT(4)

#define FDCAN_IR_RF0N BIT(0)
#define FDCAN_IR_TC BIT(7)
#define FDCAN_IR_TEFN BIT(10)
#define FDCAN_IR_TEFF BIT(11)
#define FDCAN_ILE_EINT0 BIT(0)

#define FDCAN_RXGFC_REJECT_NONMATCH \
    ((2U << 4) | (2U << 2))
#define FDCAN_RXGFC_ONE_STD_FILTER \
    (FDCAN_RXGFC_REJECT_NONMATCH | (1U << 16))

#define FDCAN_TXBC_QUEUE BIT(24)

#define RCC_BASE 0x40021000ULL
#define RCC_CR 0x00
#define RCC_PLLCFGR 0x0c
#define RCC_APB1ENR1 0x58
#define RCC_CCIPR 0x88
#define RCC_CR_PLLON BIT(24)
#define RCC_PLLCFGR_HSI16_170MHZ 0x01105532U
#define RCC_APB1_FDCAN BIT(25)
#define RCC_CCIPR_FDCAN_SHIFT 24
#define RCC_CCIPR_FDCAN_MASK (3U << RCC_CCIPR_FDCAN_SHIFT)
#define RCC_CCIPR_FDCAN_PLLQ (1U << RCC_CCIPR_FDCAN_SHIFT)
#define RCC_CCIPR_FDCAN_PCLK1 (2U << RCC_CCIPR_FDCAN_SHIFT)
#define RCC_CCIPR_FDCAN_NONE (3U << RCC_CCIPR_FDCAN_SHIFT)

#define NVIC_ISER 0xe000e100ULL
#define NVIC_ISPR 0xe000e200ULL
#define NVIC_ICPR 0xe000e280ULL

#define MRAM_STD_FILTER 0x000
#define MRAM_RX_FIFO0 0x0b0
#define MRAM_TX_EVENT 0x260
#define MRAM_TX_BUFFER 0x278
#define MRAM_ELEMENT_SIZE 72
#define MRAM_TX_EVENT_SIZE 8

#define STD_FILTER_CLASSIC (2U << 30)
#define STD_FILTER_FIFO0 (1U << 27)
#define STD_FILTER_ID1_SHIFT 16
#define STD_FILTER_ID_MASK 0x7ffU

#define MSG_ID_XTD BIT(30)
#define MSG_ID_RTR BIT(29)
#define MSG_DLC_SHIFT 16
#define MSG_BRS BIT(20)
#define MSG_FDF BIT(21)
#define MSG_EFC BIT(23)
#define MSG_MARKER_SHIFT 24

typedef struct FdcanController {
    uint64_t base;
    uint32_t mram_offset;
    unsigned int irq[2];
} FdcanController;

typedef struct TxFrame {
    uint32_t id;
    bool extended;
    bool remote;
    uint8_t dlc;
    bool fd;
    bool brs;
    bool event;
    uint8_t marker;
    const uint8_t *payload;
} TxFrame;

static const FdcanController controllers[] = {
    {
        .base = FDCAN1_BASE,
        .mram_offset = 0,
        .irq = { 21, 22 },
    }, {
        .base = FDCAN2_BASE,
        .mram_offset = FDCAN_MRAM_SLICE_SIZE,
        .irq = { 86, 87 },
    }, {
        .base = FDCAN3_BASE,
        .mram_offset = 2 * FDCAN_MRAM_SLICE_SIZE,
        .irq = { 88, 89 },
    },
};

static const char *shared_bus_args =
    "-M " STM32G474_MACHINE
    " -object can-bus,id=qcan"
    " -machine canbus0=qcan,canbus1=qcan,canbus2=qcan"
    " -serial null -serial null -serial null";

static QTestState *traffic_qtest_start(void)
{
    return qtest_init(shared_bus_args);
}

static QTestState *traffic_qtest_start_incoming(void)
{
    return qtest_initf("%s -incoming defer", shared_bus_args);
}

static QTestState *hostless_qtest_start(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null");
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

static void set_fdcan_clock_mux(QTestState *qts, uint32_t selection)
{
    uint32_t ccipr = rcc_readl(qts, RCC_CCIPR);

    ccipr &= ~RCC_CCIPR_FDCAN_MASK;
    rcc_writel(qts, RCC_CCIPR, ccipr | selection);
}

static void enable_fdcan_clock(QTestState *qts, uint32_t selection)
{
    rcc_writel(qts, RCC_APB1ENR1,
               rcc_readl(qts, RCC_APB1ENR1) | RCC_APB1_FDCAN);
    set_fdcan_clock_mux(qts, selection);
}

static void enable_pll_170mhz(QTestState *qts)
{
    rcc_writel(qts, RCC_PLLCFGR, RCC_PLLCFGR_HSI16_170MHZ);
    rcc_writel(qts, RCC_CR,
               rcc_readl(qts, RCC_CR) | RCC_CR_PLLON);
}

static uint64_t controller_mram(const FdcanController *controller)
{
    return FDCAN_MRAM_BASE + controller->mram_offset;
}

static size_t dlc_length(uint8_t dlc)
{
    static const uint8_t lengths[16] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 12, 16, 20, 24, 32, 48, 64,
    };

    return lengths[dlc & 0xf];
}

static void write_standard_filter(QTestState *qts,
                                  const FdcanController *controller,
                                  uint16_t id)
{
    uint32_t filter = STD_FILTER_CLASSIC | STD_FILTER_FIFO0 |
                      ((uint32_t)id << STD_FILTER_ID1_SHIFT) |
                      STD_FILTER_ID_MASK;

    qtest_writel(qts, controller_mram(controller) + MRAM_STD_FILTER,
                 filter);
}

static uint32_t tx_header(const TxFrame *frame)
{
    uint32_t value;

    if (frame->extended) {
        value = MSG_ID_XTD | (frame->id & 0x1fffffffU);
    } else {
        value = (frame->id & STD_FILTER_ID_MASK) << 18;
    }
    return frame->remote ? value | MSG_ID_RTR : value;
}

static uint32_t tx_control(const TxFrame *frame)
{
    uint32_t value = (uint32_t)frame->dlc << MSG_DLC_SHIFT;

    if (frame->fd) {
        value |= MSG_FDF;
    }
    if (frame->brs) {
        value |= MSG_BRS;
    }
    if (frame->event) {
        value |= MSG_EFC;
    }
    return value | ((uint32_t)frame->marker << MSG_MARKER_SHIFT);
}

static void write_tx_frame(QTestState *qts,
                           const FdcanController *controller,
                           unsigned int buffer, const TxFrame *frame)
{
    uint64_t address = controller_mram(controller) + MRAM_TX_BUFFER +
                       buffer * MRAM_ELEMENT_SIZE;
    size_t length = frame->remote ? 0 :
                    frame->fd ? dlc_length(frame->dlc) :
                                MIN(frame->dlc, 8);

    qtest_memset(qts, address, 0, MRAM_ELEMENT_SIZE);
    qtest_writel(qts, address, tx_header(frame));
    qtest_writel(qts, address + sizeof(uint32_t), tx_control(frame));
    if (length) {
        qtest_memwrite(qts, address + 2 * sizeof(uint32_t),
                       frame->payload, length);
    }
}

static void enter_configuration(QTestState *qts,
                                const FdcanController *controller,
                                uint32_t modes)
{
    fdcan_writel(qts, controller, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    fdcan_writel(qts, controller, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE | modes);
}

static void configure_receiver(QTestState *qts,
                               const FdcanController *controller,
                               uint32_t modes, uint16_t id)
{
    enter_configuration(qts, controller, modes);
    fdcan_writel(qts, controller, FDCAN_RXGFC,
                 FDCAN_RXGFC_ONE_STD_FILTER);
    write_standard_filter(qts, controller, id);
    fdcan_writel(qts, controller, FDCAN_CCCR, modes);
}

static void configure_sender(QTestState *qts,
                             const FdcanController *controller,
                             uint32_t modes, bool queue, bool stopped)
{
    enter_configuration(qts, controller, modes);
    fdcan_writel(qts, controller, FDCAN_RXGFC,
                 FDCAN_RXGFC_REJECT_NONMATCH);
    fdcan_writel(qts, controller, FDCAN_TXBC,
                 queue ? FDCAN_TXBC_QUEUE : 0);
    fdcan_writel(qts, controller, FDCAN_CCCR,
                 modes | (stopped ? FDCAN_CCCR_INIT : 0));
}

static unsigned int field(uint32_t value, unsigned int shift,
                          uint32_t mask)
{
    return (value >> shift) & mask;
}

static void assert_rx_fifo0(QTestState *qts,
                            const FdcanController *controller,
                            unsigned int fill, unsigned int get,
                            unsigned int put)
{
    uint32_t status = fdcan_readl(qts, controller, FDCAN_RXF0S);

    g_assert_cmpuint(field(status, 0, 0xf), ==, fill);
    g_assert_cmpuint(field(status, 8, 0x3), ==, get);
    g_assert_cmpuint(field(status, 16, 0x3), ==, put);
}

static void assert_rx_element(QTestState *qts,
                              const FdcanController *controller,
                              unsigned int index, const TxFrame *frame)
{
    uint64_t address = controller_mram(controller) + MRAM_RX_FIFO0 +
                       index * MRAM_ELEMENT_SIZE;
    uint8_t actual[64] = { 0 };
    size_t length = frame->remote ? 0 :
                    frame->fd ? dlc_length(frame->dlc) :
                                MIN(frame->dlc, 8);
    uint32_t expected_control =
        (uint32_t)frame->dlc << MSG_DLC_SHIFT;

    if (frame->fd) {
        expected_control |= MSG_FDF;
    }
    if (frame->brs) {
        expected_control |= MSG_BRS;
    }
    g_assert_cmphex(qtest_readl(qts, address), ==, tx_header(frame));
    g_assert_cmphex(qtest_readl(qts, address + sizeof(uint32_t)), ==,
                    expected_control);
    qtest_memread(qts, address + 2 * sizeof(uint32_t), actual, length);
    g_assert_cmpmem(actual, length, frame->payload, length);
}

static void assert_tx_event(QTestState *qts,
                            const FdcanController *controller,
                            unsigned int index, const TxFrame *frame)
{
    uint64_t address = controller_mram(controller) + MRAM_TX_EVENT +
                       index * MRAM_TX_EVENT_SIZE;
    uint32_t control = qtest_readl(qts, address + sizeof(uint32_t));

    g_assert_cmphex(qtest_readl(qts, address), ==, tx_header(frame));
    g_assert_cmpuint(field(control, MSG_MARKER_SHIFT, 0xff), ==,
                     frame->marker);
    g_assert_cmpuint(field(control, MSG_DLC_SHIFT, 0xf), ==,
                     frame->dlc);
    g_assert_cmphex(control & (MSG_FDF | MSG_BRS), ==,
                    tx_control(frame) & (MSG_FDF | MSG_BRS));
}

static void assert_tx_fifo(QTestState *qts,
                           const FdcanController *controller,
                           unsigned int free, unsigned int get,
                           unsigned int put, bool full)
{
    uint32_t status = fdcan_readl(qts, controller, FDCAN_TXFQS);

    g_assert_cmpuint(field(status, 0, 0x7), ==, free);
    g_assert_cmpuint(field(status, 8, 0x3), ==, get);
    g_assert_cmpuint(field(status, 16, 0x3), ==, put);
    g_assert_cmpuint(field(status, 21, 0x1), ==, full);
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
    qtest_writel(qts, nvic_register(NVIC_ISER, irq),
                 nvic_irq_bit(irq));
}

static void nvic_clear(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, nvic_register(NVIC_ICPR, irq),
                 nvic_irq_bit(irq));
}

static bool nvic_pending(QTestState *qts, unsigned int irq)
{
    return qtest_readl(qts, nvic_register(NVIC_ISPR, irq)) &
           nvic_irq_bit(irq);
}

static void test_classic_peer_filter_irq(void)
{
    const FdcanController *sender = &controllers[0];
    const FdcanController *receiver = &controllers[1];
    const uint8_t rejected_payload[2] = { 0xde, 0xad };
    const uint8_t payload[8] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    const TxFrame rejected = {
        .id = 0x102,
        .dlc = sizeof(rejected_payload),
        .payload = rejected_payload,
    };
    const TxFrame accepted = {
        .id = 0x101,
        .dlc = sizeof(payload),
        .payload = payload,
    };
    QTestState *qts = traffic_qtest_start();

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_receiver(qts, receiver, 0, accepted.id);
    configure_sender(qts, sender, 0, false, false);

    fdcan_writel(qts, sender, FDCAN_TXBTIE, BIT(1));
    fdcan_writel(qts, sender, FDCAN_IE, FDCAN_IR_TC);
    fdcan_writel(qts, sender, FDCAN_ILS, 0);
    fdcan_writel(qts, sender, FDCAN_ILE, FDCAN_ILE_EINT0);
    nvic_enable(qts, sender->irq[0]);
    nvic_clear(qts, sender->irq[0]);

    write_tx_frame(qts, sender, 0, &rejected);
    fdcan_writel(qts, sender, FDCAN_TXBAR, BIT(0));
    assert_rx_fifo0(qts, receiver, 0, 0, 0);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBTO), ==,
                    BIT(0));
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_IR), ==, 0);
    g_assert_false(nvic_pending(qts, sender->irq[0]));

    write_tx_frame(qts, sender, 1, &accepted);
    fdcan_writel(qts, sender, FDCAN_TXBAR, BIT(1));
    assert_rx_fifo0(qts, receiver, 1, 0, 1);
    assert_rx_element(qts, receiver, 0, &accepted);
    g_assert_cmphex(fdcan_readl(qts, receiver, FDCAN_IR), ==,
                    FDCAN_IR_RF0N);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBTO), ==,
                    BIT(0) | BIT(1));
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_IR), ==,
                    FDCAN_IR_TC);
    g_assert_true(nvic_pending(qts, sender->irq[0]));

    fdcan_writel(qts, sender, FDCAN_IR, FDCAN_IR_TC);
    nvic_clear(qts, sender->irq[0]);
    g_assert_false(nvic_pending(qts, sender->irq[0]));
    qtest_quit(qts);
}

static void test_fd_brs_64_bytes(void)
{
    const FdcanController *sender = &controllers[0];
    const FdcanController *receiver = &controllers[1];
    uint8_t payload[64];
    TxFrame frame = {
        .id = 0x321,
        .dlc = 15,
        .fd = true,
        .brs = true,
        .event = true,
        .marker = 0xa5,
        .payload = payload,
    };
    QTestState *qts = traffic_qtest_start();

    for (unsigned int i = 0; i < sizeof(payload); i++) {
        payload[i] = 0xff - i;
    }
    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_receiver(qts, receiver,
                       FDCAN_CCCR_FDOE | FDCAN_CCCR_BRSE, frame.id);
    configure_sender(qts, sender,
                     FDCAN_CCCR_FDOE | FDCAN_CCCR_BRSE, false, false);

    write_tx_frame(qts, sender, 2, &frame);
    fdcan_writel(qts, sender, FDCAN_TXBAR, BIT(2));

    assert_rx_fifo0(qts, receiver, 1, 0, 1);
    assert_rx_element(qts, receiver, 0, &frame);
    g_assert_cmphex(fdcan_readl(qts, receiver, FDCAN_IR), ==,
                    FDCAN_IR_RF0N);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBTO), ==,
                    BIT(2));
    g_assert_cmpuint(field(fdcan_readl(qts, sender, FDCAN_TXEFS),
                           0, 0x7), ==, 1);
    assert_tx_event(qts, sender, 0, &frame);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_IR), ==,
                    FDCAN_IR_TEFN);
    qtest_quit(qts);
}

static void run_queue_batch(QTestState *qts,
                            const FdcanController *controller,
                            const TxFrame frames[3],
                            const unsigned int expected[3])
{
    configure_sender(qts, controller, 0, true, true);
    for (unsigned int i = 0; i < 3; i++) {
        write_tx_frame(qts, controller, i, &frames[i]);
    }

    fdcan_writel(qts, controller, FDCAN_TXBAR, BIT(0) | BIT(1) | BIT(2));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    BIT(0) | BIT(1) | BIT(2));
    fdcan_writel(qts, controller, FDCAN_CCCR, 0);

    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==,
                    BIT(0) | BIT(1) | BIT(2));
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           0, 0x7), ==, 3);
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           24, 0x1), ==, 1);
    for (unsigned int i = 0; i < 3; i++) {
        assert_tx_event(qts, controller, i, &frames[expected[i]]);
    }
    fdcan_writel(qts, controller, FDCAN_TXEFA, 2);
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           0, 0x7), ==, 0);
}

static void test_queue_arbitration(void)
{
    const FdcanController *controller = &controllers[0];
    const uint8_t payload[1] = { 0x5a };
    const uint32_t base = 0x155;
    const uint32_t tied_id = (base << 18) | 0x100;
    const TxFrame base_and_format[3] = {
        {
            .id = (0x120U << 18) | 0x155,
            .extended = true,
            .dlc = 1,
            .event = true,
            .marker = 0xa0,
            .payload = payload,
        }, {
            .id = 0x120,
            .dlc = 1,
            .event = true,
            .marker = 0xb1,
            .payload = payload,
        }, {
            .id = 0x100,
            .dlc = 1,
            .event = true,
            .marker = 0xc2,
            .payload = payload,
        },
    };
    const unsigned int base_and_format_order[3] = { 2, 1, 0 };
    const TxFrame full_id_and_buffer[3] = {
        {
            .id = tied_id,
            .extended = true,
            .dlc = 1,
            .event = true,
            .marker = 0xd0,
            .payload = payload,
        }, {
            .id = tied_id,
            .extended = true,
            .dlc = 1,
            .event = true,
            .marker = 0xd1,
            .payload = payload,
        }, {
            .id = (base << 18) | 0x80,
            .extended = true,
            .dlc = 1,
            .event = true,
            .marker = 0xd2,
            .payload = payload,
        },
    };
    const unsigned int full_id_and_buffer_order[3] = { 2, 0, 1 };
    const TxFrame rtr_and_buffer[3] = {
        {
            .id = 0x200,
            .remote = true,
            .dlc = 1,
            .event = true,
            .marker = 0xe0,
            .payload = payload,
        }, {
            .id = 0x200,
            .dlc = 1,
            .event = true,
            .marker = 0xe1,
            .payload = payload,
        }, {
            .id = 0x200,
            .remote = true,
            .dlc = 1,
            .event = true,
            .marker = 0xe2,
            .payload = payload,
        },
    };
    const unsigned int rtr_and_buffer_order[3] = { 0, 1, 2 };
    QTestState *qts = traffic_qtest_start();

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    run_queue_batch(qts, controller, base_and_format,
                    base_and_format_order);
    run_queue_batch(qts, controller, full_id_and_buffer,
                    full_id_and_buffer_order);
    run_queue_batch(qts, controller, rtr_and_buffer,
                    rtr_and_buffer_order);
    qtest_quit(qts);
}

static void test_fifo_wrap_cancel(void)
{
    const FdcanController *controller = &controllers[0];
    const uint8_t payload[3][1] = {
        { 0x00 }, { 0x11 }, { 0x22 },
    };
    const TxFrame frames[3] = {
        {
            .id = 0x300,
            .dlc = 1,
            .event = true,
            .marker = 0x30,
            .payload = payload[0],
        }, {
            .id = 0x200,
            .dlc = 1,
            .event = true,
            .marker = 0x20,
            .payload = payload[1],
        }, {
            .id = 0x100,
            .dlc = 1,
            .event = true,
            .marker = 0x10,
            .payload = payload[2],
        },
    };
    const unsigned int event_order[3] = { 2, 1, 0 };
    QTestState *qts = traffic_qtest_start();

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_sender(qts, controller, 0, false, true);
    assert_tx_fifo(qts, controller, 3, 0, 0, false);

    fdcan_writel(qts, controller, FDCAN_TXBAR, BIT(0));
    assert_tx_fifo(qts, controller, 2, 0, 1, false);
    fdcan_writel(qts, controller, FDCAN_TXBCR, BIT(0));
    assert_tx_fifo(qts, controller, 3, 0, 1, false);

    fdcan_writel(qts, controller, FDCAN_TXBAR, BIT(1));
    assert_tx_fifo(qts, controller, 2, 1, 2, false);
    fdcan_writel(qts, controller, FDCAN_TXBCR, BIT(1));
    assert_tx_fifo(qts, controller, 3, 0, 2, false);

    for (unsigned int i = 0; i < 3; i++) {
        write_tx_frame(qts, controller, i, &frames[i]);
    }
    fdcan_writel(qts, controller, FDCAN_TXBAR,
                 BIT(0) | BIT(1) | BIT(2));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    BIT(0) | BIT(1) | BIT(2));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCF), ==, 0);
    assert_tx_fifo(qts, controller, 0, 2, 2, true);

    fdcan_writel(qts, controller, FDCAN_TXBCR, BIT(0));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    BIT(1) | BIT(2));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCF), ==,
                    BIT(0));
    assert_tx_fifo(qts, controller, 1, 2, 0, false);

    fdcan_writel(qts, controller, FDCAN_TXBAR, BIT(0));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    BIT(0) | BIT(1) | BIT(2));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBCF), ==, 0);
    assert_tx_fifo(qts, controller, 0, 2, 1, true);

    fdcan_writel(qts, controller, FDCAN_CCCR, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==,
                    BIT(0) | BIT(1) | BIT(2));
    assert_tx_fifo(qts, controller, 3, 0, 2, false);
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           0, 0x7), ==, 3);
    for (unsigned int i = 0; i < 3; i++) {
        assert_tx_event(qts, controller, i, &frames[event_order[i]]);
    }
    qtest_quit(qts);
}

static void test_hostless_completion(void)
{
    const FdcanController *controller = &controllers[0];
    const uint8_t payload[3] = { 0x12, 0x34, 0x56 };
    const TxFrame frame = {
        .id = 0x456,
        .dlc = sizeof(payload),
        .payload = payload,
    };
    QTestState *qts = hostless_qtest_start();

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_sender(qts, controller, 0, false, false);
    fdcan_writel(qts, controller, FDCAN_TXBTIE, BIT(0));
    write_tx_frame(qts, controller, 0, &frame);
    fdcan_writel(qts, controller, FDCAN_TXBAR, BIT(0));

    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==,
                    BIT(0));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_IR), ==,
                    FDCAN_IR_TC);
    qtest_quit(qts);
}

static void configure_loopback(QTestState *qts,
                               const FdcanController *controller,
                               bool internal, uint16_t id)
{
    uint32_t modes = FDCAN_CCCR_TEST |
                     (internal ? FDCAN_CCCR_MON : 0);

    enter_configuration(qts, controller, modes);
    fdcan_writel(qts, controller, FDCAN_RXGFC,
                 FDCAN_RXGFC_ONE_STD_FILTER);
    write_standard_filter(qts, controller, id);
    fdcan_writel(qts, controller, FDCAN_TEST, FDCAN_TEST_LBCK);
    fdcan_writel(qts, controller, FDCAN_CCCR, modes);
}

static void test_monitor_receive_only(void)
{
    const FdcanController *monitor = &controllers[0];
    const FdcanController *peer = &controllers[1];
    const uint8_t payload[2] = { 0xca, 0xfe };
    const TxFrame frame = {
        .id = 0x512,
        .dlc = sizeof(payload),
        .payload = payload,
    };
    QTestState *qts = traffic_qtest_start();

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_receiver(qts, monitor, FDCAN_CCCR_MON, frame.id);
    configure_sender(qts, peer, 0, false, false);

    write_tx_frame(qts, monitor, 0, &frame);
    fdcan_writel(qts, monitor, FDCAN_TXBAR, BIT(0));
    g_assert_cmphex(fdcan_readl(qts, monitor, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, monitor, FDCAN_TXBTO), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, monitor, FDCAN_IR), ==, 0);

    write_tx_frame(qts, peer, 0, &frame);
    fdcan_writel(qts, peer, FDCAN_TXBAR, BIT(0));
    assert_rx_fifo0(qts, monitor, 1, 0, 1);
    assert_rx_element(qts, monitor, 0, &frame);
    g_assert_cmphex(fdcan_readl(qts, monitor, FDCAN_IR), ==,
                    FDCAN_IR_RF0N);
    qtest_quit(qts);
}

static void test_external_loopback(void)
{
    const FdcanController *loopback = &controllers[0];
    const FdcanController *peer = &controllers[1];
    const uint8_t payload[4] = { 0x11, 0x22, 0x33, 0x44 };
    const TxFrame frame = {
        .id = 0x601,
        .dlc = sizeof(payload),
        .payload = payload,
    };
    QTestState *qts = traffic_qtest_start();

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_loopback(qts, loopback, false, frame.id);
    configure_receiver(qts, peer, 0, frame.id);

    write_tx_frame(qts, loopback, 0, &frame);
    fdcan_writel(qts, loopback, FDCAN_TXBAR, BIT(0));
    assert_rx_fifo0(qts, loopback, 1, 0, 1);
    assert_rx_element(qts, loopback, 0, &frame);
    assert_rx_fifo0(qts, peer, 1, 0, 1);
    assert_rx_element(qts, peer, 0, &frame);

    configure_sender(qts, peer, 0, false, false);
    write_tx_frame(qts, peer, 0, &frame);
    fdcan_writel(qts, peer, FDCAN_TXBAR, BIT(0));
    assert_rx_fifo0(qts, loopback, 1, 0, 1);
    qtest_quit(qts);
}

static void test_internal_loopback(void)
{
    const FdcanController *loopback = &controllers[0];
    const FdcanController *peer = &controllers[1];
    const uint8_t payload[4] = { 0x88, 0x77, 0x66, 0x55 };
    const TxFrame frame = {
        .id = 0x602,
        .dlc = sizeof(payload),
        .payload = payload,
    };
    QTestState *qts = traffic_qtest_start();

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_loopback(qts, loopback, true, frame.id);
    configure_receiver(qts, peer, 0, frame.id);

    write_tx_frame(qts, loopback, 0, &frame);
    fdcan_writel(qts, loopback, FDCAN_TXBAR, BIT(0));
    assert_rx_fifo0(qts, loopback, 1, 0, 1);
    assert_rx_element(qts, loopback, 0, &frame);
    assert_rx_fifo0(qts, peer, 0, 0, 0);

    configure_sender(qts, peer, 0, false, false);
    write_tx_frame(qts, peer, 0, &frame);
    fdcan_writel(qts, peer, FDCAN_TXBAR, BIT(0));
    assert_rx_fifo0(qts, loopback, 1, 0, 1);
    qtest_quit(qts);
}

static void test_clock_and_cccr_recovery(void)
{
    const FdcanController *controller = &controllers[0];
    const uint8_t payload[2] = { 0x90, 0x09 };
    const TxFrame clock_frame = {
        .id = 0x701,
        .dlc = sizeof(payload),
        .event = true,
        .marker = 0x91,
        .payload = payload,
    };
    const TxFrame cccr_frame = {
        .id = 0x702,
        .dlc = sizeof(payload),
        .event = true,
        .marker = 0x92,
        .payload = payload,
    };
    QTestState *qts = traffic_qtest_start();

    enable_pll_170mhz(qts);
    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PLLQ);
    configure_sender(qts, controller, 0, false, false);

    write_tx_frame(qts, controller, 0, &clock_frame);
    fdcan_writel(qts, controller, FDCAN_TXBAR, BIT(0));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    BIT(0));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==, 0);
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           0, 0x7), ==, 0);

    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_PCLK1);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==,
                    BIT(0));
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           0, 0x7), ==, 1);
    assert_tx_event(qts, controller, 0, &clock_frame);

    configure_sender(qts, controller, 0, false, true);
    write_tx_frame(qts, controller, 1, &cccr_frame);
    fdcan_writel(qts, controller, FDCAN_TXBAR, BIT(1));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==,
                    BIT(1));
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==,
                    BIT(0));
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           0, 0x7), ==, 1);
    assert_tx_event(qts, controller, 0, &clock_frame);

    fdcan_writel(qts, controller, FDCAN_CCCR, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, controller, FDCAN_TXBTO), ==,
                    BIT(0) | BIT(1));
    g_assert_cmpuint(field(fdcan_readl(qts, controller, FDCAN_TXEFS),
                           0, 0x7), ==, 2);
    assert_tx_event(qts, controller, 0, &clock_frame);
    assert_tx_event(qts, controller, 1, &cccr_frame);
    qtest_quit(qts);
}

static void test_shared_ckdiv_and_monitor_clear(void)
{
    const FdcanController *divider = &controllers[0];
    const FdcanController *pending = &controllers[1];
    const uint8_t payload[1] = { 0xc6 };
    const TxFrame pending_frame = {
        .id = 0x711,
        .dlc = sizeof(payload),
        .event = true,
        .marker = 0xc6,
        .payload = payload,
    };
    const TxFrame cleared_frame = {
        .id = 0x712,
        .dlc = sizeof(payload),
        .event = true,
        .marker = 0x12,
        .payload = payload,
    };
    QTestState *qts = traffic_qtest_start();

    enable_pll_170mhz(qts);
    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PLLQ);
    configure_sender(qts, pending, 0, false, false);
    write_tx_frame(qts, pending, 0, &pending_frame);
    fdcan_writel(qts, pending, FDCAN_TXBAR, BIT(0));
    g_assert_cmphex(fdcan_readl(qts, pending, FDCAN_TXBRP), ==,
                    BIT(0));

    enter_configuration(qts, divider, 0);
    fdcan_writel(qts, divider, FDCAN_CKDIV, 6);
    g_assert_cmphex(fdcan_readl(qts, pending, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(qts, pending, FDCAN_TXBTO), ==,
                    BIT(0));
    g_assert_cmpuint(field(fdcan_readl(qts, pending, FDCAN_TXEFS),
                           0, 0x7), ==, 1);
    assert_tx_event(qts, pending, 0, &pending_frame);

    fdcan_writel(qts, divider, FDCAN_CKDIV, 0);
    configure_loopback(qts, divider, true, cleared_frame.id);
    write_tx_frame(qts, divider, 1, &cleared_frame);
    fdcan_writel(qts, divider, FDCAN_TXBAR, BIT(1));
    g_assert_cmphex(fdcan_readl(qts, divider, FDCAN_TXBRP), ==,
                    BIT(1));
    g_assert_cmphex(fdcan_readl(qts, divider, FDCAN_TXBTO), ==, 0);

    fdcan_writel(qts, divider, FDCAN_TEST, 0);
    g_assert_cmphex(fdcan_readl(qts, divider, FDCAN_TXBRP), ==, 0);
    assert_tx_fifo(qts, divider, 3, 0, 0, false);
    set_fdcan_clock_mux(qts, RCC_CCIPR_FDCAN_PCLK1);
    g_assert_cmphex(fdcan_readl(qts, divider, FDCAN_TXBTO), ==, 0);
    g_assert_cmpuint(field(fdcan_readl(qts, divider, FDCAN_TXEFS),
                           0, 0x7), ==, 0);
    assert_rx_fifo0(qts, divider, 0, 0, 0);
    qtest_quit(qts);
}

static void configure_migration_source(QTestState *qts,
                                       const TxFrame frames[3])
{
    const FdcanController *sender = &controllers[0];
    const FdcanController *receiver = &controllers[1];

    enable_fdcan_clock(qts, RCC_CCIPR_FDCAN_PCLK1);
    configure_receiver(qts, receiver, 0, frames[0].id);
    configure_sender(qts, sender, 0, false, true);

    fdcan_writel(qts, sender, FDCAN_TXBAR, BIT(0));
    fdcan_writel(qts, sender, FDCAN_TXBCR, BIT(0));
    fdcan_writel(qts, sender, FDCAN_TXBAR, BIT(1));
    fdcan_writel(qts, sender, FDCAN_TXBCR, BIT(1));
    for (unsigned int i = 0; i < 3; i++) {
        write_tx_frame(qts, sender, i, &frames[i]);
    }
    fdcan_writel(qts, sender, FDCAN_TXBAR, BIT(0) | BIT(2));

    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBRP), ==,
                    BIT(0) | BIT(2));
    assert_tx_fifo(qts, sender, 1, 2, 1, false);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBTO), ==, 0);
    g_assert_cmpuint(field(fdcan_readl(qts, sender, FDCAN_TXEFS),
                           0, 0x7), ==, 0);
    assert_rx_fifo0(qts, receiver, 0, 0, 0);

    fdcan_writel(qts, sender, FDCAN_CCCR,
                 FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBRP), ==,
                    BIT(0) | BIT(2));
    assert_tx_fifo(qts, sender, 1, 2, 1, false);
}

static void assert_migrated_pending(QTestState *qts)
{
    const FdcanController *sender = &controllers[0];
    const FdcanController *receiver = &controllers[1];

    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_CCCR), ==,
                    FDCAN_CCCR_INIT | FDCAN_CCCR_CCE);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBRP), ==,
                    BIT(0) | BIT(2));
    assert_tx_fifo(qts, sender, 1, 2, 1, false);
    g_assert_cmphex(fdcan_readl(qts, sender, FDCAN_TXBTO), ==, 0);
    g_assert_cmpuint(field(fdcan_readl(qts, sender, FDCAN_TXEFS),
                           0, 0x7), ==, 0);
    assert_rx_fifo0(qts, receiver, 0, 0, 0);
}

static void test_pending_fifo_migration(void)
{
    const FdcanController *sender = &controllers[0];
    const FdcanController *receiver = &controllers[1];
    const uint8_t payload[3][1] = {
        { 0xa0 }, { 0xb1 }, { 0xc2 },
    };
    const TxFrame frames[3] = {
        {
            .id = 0x721,
            .dlc = 1,
            .event = true,
            .marker = 0xa0,
            .payload = payload[0],
        }, {
            .id = 0x721,
            .dlc = 1,
            .event = true,
            .marker = 0xb1,
            .payload = payload[1],
        }, {
            .id = 0x721,
            .dlc = 1,
            .event = true,
            .marker = 0xc2,
            .payload = payload[2],
        },
    };
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-fdcan-traffic-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = traffic_qtest_start();
    dst = traffic_qtest_start_incoming();
    configure_migration_source(src, frames);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    assert_migrated_pending(dst);
    fdcan_writel(dst, sender, FDCAN_CCCR, 0);
    g_assert_cmphex(fdcan_readl(dst, sender, FDCAN_TXBRP), ==, 0);
    g_assert_cmphex(fdcan_readl(dst, sender, FDCAN_TXBTO), ==,
                    BIT(0) | BIT(2));
    assert_tx_fifo(dst, sender, 3, 0, 1, false);
    g_assert_cmpuint(field(fdcan_readl(dst, sender, FDCAN_TXEFS),
                           0, 0x7), ==, 2);
    assert_tx_event(dst, sender, 0, &frames[2]);
    assert_tx_event(dst, sender, 1, &frames[0]);
    assert_rx_fifo0(dst, receiver, 2, 0, 2);
    assert_rx_element(dst, receiver, 0, &frames[2]);
    assert_rx_element(dst, receiver, 1, &frames[0]);

    fdcan_writel(dst, sender, FDCAN_CCCR, 0);
    set_fdcan_clock_mux(dst, RCC_CCIPR_FDCAN_NONE);
    set_fdcan_clock_mux(dst, RCC_CCIPR_FDCAN_PCLK1);
    g_assert_cmpuint(field(fdcan_readl(dst, sender, FDCAN_TXEFS),
                           0, 0x7), ==, 2);
    assert_rx_fifo0(dst, receiver, 2, 0, 2);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stm32g474/fdcan-traffic/1-classic-peer-filter-irq",
                   test_classic_peer_filter_irq);
    qtest_add_func("/stm32g474/fdcan-traffic/2-fd-brs-64-bytes",
                   test_fd_brs_64_bytes);
    qtest_add_func("/stm32g474/fdcan-traffic/3-queue-arbitration",
                   test_queue_arbitration);
    qtest_add_func("/stm32g474/fdcan-traffic/4-fifo-wrap-cancel",
                   test_fifo_wrap_cancel);
    qtest_add_func("/stm32g474/fdcan-traffic/5-hostless-completion",
                   test_hostless_completion);
    qtest_add_func("/stm32g474/fdcan-traffic/6-monitor-receive-only",
                   test_monitor_receive_only);
    qtest_add_func("/stm32g474/fdcan-traffic/7-external-loopback",
                   test_external_loopback);
    qtest_add_func("/stm32g474/fdcan-traffic/8-internal-loopback",
                   test_internal_loopback);
    qtest_add_func("/stm32g474/fdcan-traffic/9-clock-cccr-recovery",
                   test_clock_and_cccr_recovery);
    qtest_add_func(
        "/stm32g474/fdcan-traffic/10-shared-ckdiv-monitor-clear",
        test_shared_ckdiv_and_monitor_clear);
    qtest_add_func("/stm32g474/fdcan-traffic/11-pending-fifo-migration",
                   test_pending_fifo_migration);

    return g_test_run();
}

/*
 * Bosch M_CAN message engine tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/net/m_can.h"
#include "qemu/bswap.h"

#define TEST_MRAM_SIZE 0x350

#define TX_ID_RTR (1U << 29)
#define TX_ID_XTD (1U << 30)
#define TX_ID_ESI (1U << 31)
#define TX_DLC_SHIFT 16
#define TX_BRS (1U << 20)
#define TX_FDF (1U << 21)
#define TX_EFC (1U << 23)
#define TX_MM_SHIFT 24

#define RX_ANMF (1U << 31)
#define RX_FIDX_SHIFT 24

#define STD_FILTER_ID2_MASK 0x7ffU
#define STD_FILTER_ID1_SHIFT 16
#define STD_FILTER_SFEC_SHIFT 27
#define STD_FILTER_SFT_SHIFT 30

#define EXT_FILTER_ID_MASK 0x1fffffffU
#define EXT_FILTER_EFEC_SHIFT 29
#define EXT_FILTER_EFT_SHIFT 30

typedef struct TestMCan {
    MCanEngine engine;
    MCanEngineConfig config;
    uint8_t mram[TEST_MRAM_SIZE];
    uint32_t events;
} TestMCan;

static const MCanMsgRamLayout test_layout = {
    .std_filter = 0x000,
    .ext_filter = 0x070,
    .rx_fifo = { 0x0b0, 0x188 },
    .tx_event = 0x260,
    .tx_buffer = 0x278,
    .std_filters = 28,
    .ext_filters = 8,
    .rx_elements = { 3, 3 },
    .tx_events = 3,
    .tx_buffers = 3,
};

static void test_get_config(void *opaque, MCanEngineConfig *config)
{
    TestMCan *test = opaque;

    *config = test->config;
}

static void test_event(void *opaque, uint32_t events)
{
    TestMCan *test = opaque;

    test->events |= events;
}

static const MCanEngineOps test_ops = {
    .get_config = test_get_config,
    .event = test_event,
};

static void test_m_can_init(TestMCan *test)
{
    *test = (TestMCan) {
        .config = {
            .enabled = true,
            .brs_enabled = true,
            .std_filters = test_layout.std_filters,
            .ext_filters = test_layout.ext_filters,
            .nonmatching_standard = M_CAN_NONMATCHING_REJECT,
            .nonmatching_extended = M_CAN_NONMATCHING_REJECT,
            .extended_id_mask = QEMU_CAN_EFF_MASK,
        },
    };

    g_assert_true(m_can_engine_init(&test->engine, test->mram,
                                    sizeof(test->mram), &test_layout,
                                    &test_ops, test));
}

static void set_std_filter(TestMCan *test, unsigned int index,
                           unsigned int type, unsigned int action,
                           uint16_t id1, uint16_t id2)
{
    uint32_t value = (type << STD_FILTER_SFT_SHIFT) |
                     (action << STD_FILTER_SFEC_SHIFT) |
                     (id1 << STD_FILTER_ID1_SHIFT) |
                     (id2 & STD_FILTER_ID2_MASK);

    stl_le_p(test->mram + test_layout.std_filter + index * sizeof(value),
             value);
}

static void set_ext_filter(TestMCan *test, unsigned int index,
                           unsigned int type, unsigned int action,
                           uint32_t id1, uint32_t id2)
{
    uint8_t *element = test->mram + test_layout.ext_filter +
                       index * 2 * sizeof(uint32_t);

    stl_le_p(element, (action << EXT_FILTER_EFEC_SHIFT) |
                      (id1 & EXT_FILTER_ID_MASK));
    stl_le_p(element + sizeof(uint32_t),
             (type << EXT_FILTER_EFT_SHIFT) |
             (id2 & EXT_FILTER_ID_MASK));
}

static qemu_can_frame make_frame(uint32_t id, uint8_t length)
{
    qemu_can_frame frame = {
        .can_id = id,
        .can_dlc = length,
    };

    for (unsigned int i = 0; i < MIN(length, sizeof(frame.data)); i++) {
        frame.data[i] = i + 1;
    }

    return frame;
}

static uint32_t rx_element_id(TestMCan *test, unsigned int fifo,
                              unsigned int index)
{
    size_t offset = test_layout.rx_fifo[fifo] +
                    index * M_CAN_ELEMENT_SIZE;

    return ldl_le_p(test->mram + offset);
}

static void test_layout_validation(void)
{
    TestMCan test;
    MCanMsgRamLayout bad_layout = test_layout;
    MCanEngineOps bad_ops = test_ops;

    test_m_can_init(&test);

    bad_layout.tx_buffer = TEST_MRAM_SIZE - M_CAN_ELEMENT_SIZE + 1;
    g_assert_false(m_can_engine_init(&test.engine, test.mram,
                                     sizeof(test.mram), &bad_layout,
                                     &test_ops, &test));

    bad_ops.get_config = NULL;
    g_assert_false(m_can_engine_init(&test.engine, test.mram,
                                     sizeof(test.mram), &test_layout,
                                     &bad_ops, &test));
}

static void test_tx_classic_element(void)
{
    TestMCan test;
    MCanTxTransfer transfer;
    uint8_t *element;

    test_m_can_init(&test);
    element = test.mram + test_layout.tx_buffer;
    stl_le_p(element, 0x321U << 18);
    stl_le_p(element + 4, (8U << 16) | (1U << 23) | (0x5aU << 24));
    for (unsigned int i = 0; i < 8; i++) {
        element[8 + i] = i + 1;
    }

    g_assert_true(m_can_tx_element_decode(&test.engine, 0, &transfer));
    g_assert_cmphex(transfer.frame.can_id, ==, 0x321);
    g_assert_cmpuint(transfer.frame.can_dlc, ==, 8);
    g_assert_cmpmem(transfer.frame.data, 8, element + 8, 8);
    g_assert_true(transfer.event_fifo_control);
    g_assert_cmpuint(transfer.message_marker, ==, 0x5a);
}

static void test_tx_classic_normalizes_fd_bits(void)
{
    TestMCan test;
    MCanTxTransfer transfer;
    uint8_t *element;

    test_m_can_init(&test);
    test.config.fd_enabled = true;
    element = test.mram + test_layout.tx_buffer + M_CAN_ELEMENT_SIZE;
    stl_le_p(element, TX_ID_XTD | TX_ID_RTR | TX_ID_ESI | 0x1234567);
    stl_le_p(element + 4, (15U << TX_DLC_SHIFT) | TX_BRS | TX_FDF);

    g_assert_true(m_can_tx_element_decode(&test.engine, 1, &transfer));
    g_assert_cmphex(transfer.frame.can_id, ==,
                    QEMU_CAN_EFF_FLAG | QEMU_CAN_RTR_FLAG | 0x1234567);
    g_assert_cmpuint(transfer.frame.can_dlc, ==, 8);
    g_assert_cmphex(transfer.frame.flags &
                    (QEMU_CAN_FRMF_TYPE_FD | QEMU_CAN_FRMF_BRS |
                     QEMU_CAN_FRMF_ESI), ==, 0);
    g_assert_cmpuint(transfer.dlc, ==, 15);
}

static void test_tx_fd_element(void)
{
    TestMCan test;
    MCanTxTransfer transfer;
    uint8_t *element;

    test_m_can_init(&test);
    test.config.fd_enabled = true;
    element = test.mram + test_layout.tx_buffer + 2 * M_CAN_ELEMENT_SIZE;
    stl_le_p(element, TX_ID_XTD | TX_ID_ESI | 0x1abcde);
    stl_le_p(element + 4, (15U << TX_DLC_SHIFT) | TX_BRS | TX_FDF |
                          TX_EFC | (0xa5U << TX_MM_SHIFT));
    for (unsigned int i = 0; i < 64; i++) {
        element[8 + i] = 0xff - i;
    }

    g_assert_true(m_can_tx_element_decode(&test.engine, 2, &transfer));
    g_assert_cmphex(transfer.frame.can_id, ==,
                    QEMU_CAN_EFF_FLAG | 0x1abcde);
    g_assert_cmpuint(transfer.frame.can_dlc, ==, 64);
    g_assert_cmphex(transfer.frame.flags, ==,
                    QEMU_CAN_FRMF_TYPE_FD |
                    QEMU_CAN_FRMF_BRS | QEMU_CAN_FRMF_ESI);
    g_assert_cmpmem(transfer.frame.data, 64, element + 8, 64);
    g_assert_cmpuint(transfer.dlc, ==, 15);
    g_assert_true(transfer.event_fifo_control);
    g_assert_cmpuint(transfer.message_marker, ==, 0xa5);

    test.config.brs_enabled = false;
    g_assert_true(m_can_tx_element_decode(&test.engine, 2, &transfer));
    g_assert_cmphex(transfer.frame.flags & QEMU_CAN_FRMF_BRS, ==, 0);
    g_assert_false(m_can_tx_element_decode(&test.engine, 3, &transfer));
}

static void test_standard_filter_types(void)
{
    TestMCan test;
    MCanFilterResult result;
    qemu_can_frame frame;

    test_m_can_init(&test);
    test.config.std_filters = 2;

    set_std_filter(&test, 0, 0, 1, 0x100, 0x110);
    frame = make_frame(0x108, 1);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.accepted);
    g_assert_true(result.matched);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_0);

    memset(test.mram + test_layout.std_filter, 0,
           test_layout.std_filters * sizeof(uint32_t));
    set_std_filter(&test, 0, 1, 2, 0x123, 0x456);
    frame.can_id = 0x456;
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_1);

    memset(test.mram + test_layout.std_filter, 0,
           test_layout.std_filters * sizeof(uint32_t));
    set_std_filter(&test, 0, 2, 4, 0x650, 0x7f0);
    frame.can_id = 0x657;
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.priority);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_NONE);

    memset(test.mram + test_layout.std_filter, 0,
           test_layout.std_filters * sizeof(uint32_t));
    set_std_filter(&test, 0, 3, 1, 0x321, 0x321);
    set_std_filter(&test, 1, 1, 3, 0x321, 0x123);
    frame.can_id = 0x321;
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_false(result.accepted);
    g_assert_cmpuint(result.filter_index, ==, 1);
}

static void test_filter_actions_and_order(void)
{
    TestMCan test;
    MCanFilterResult result;
    qemu_can_frame frame = make_frame(0x123, 1);

    test_m_can_init(&test);
    test.config.std_filters = 2;
    set_std_filter(&test, 0, 1, 1, 0x123, 0x456);
    set_std_filter(&test, 1, 1, 2, 0x123, 0x456);

    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_cmpuint(result.filter_index, ==, 0);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_0);

    set_std_filter(&test, 0, 1, 5, 0x123, 0x456);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.priority);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_0);

    set_std_filter(&test, 0, 1, 6, 0x123, 0x456);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.priority);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_1);

    set_std_filter(&test, 0, 3, 7, 0x123, 0);
    set_std_filter(&test, 1, 1, 2, 0x123, 0x456);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_false(result.accepted);
    g_assert_true(result.matched);
    g_assert_cmpuint(result.filter_index, ==, 0);

    set_std_filter(&test, 0, 3, 7, 0x124, 0);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.accepted);
    g_assert_cmpuint(result.filter_index, ==, 1);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_1);
}

static void test_extended_filter_types(void)
{
    TestMCan test;
    MCanFilterResult result;
    qemu_can_frame frame = make_frame(QEMU_CAN_EFF_FLAG | 0x1ff, 1);

    test_m_can_init(&test);
    test.config.ext_filters = 2;
    test.config.extended_id_mask = 0xff;

    set_ext_filter(&test, 0, 0, 1, 0, 0x100);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.accepted);
    g_assert_true(result.extended);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_0);

    frame.can_id = QEMU_CAN_EFF_FLAG | 0x1234aa;
    test.config.extended_id_mask = 0x1fffff00;
    set_ext_filter(&test, 0, 1, 2, 0x111111, 0x1234aa);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_1);

    test.config.extended_id_mask = 0;
    set_ext_filter(&test, 0, 2, 1, 0x123400, 0x1fffff00);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_0);

    test.config.extended_id_mask = 0;
    set_ext_filter(&test, 0, 3, 5, 0x123400, 0x1234ff);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.priority);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_0);

    test.config.extended_id_mask = 0x1fffff00;
    set_ext_filter(&test, 0, 3, 7, 0x123400, 0);
    set_ext_filter(&test, 1, 1, 2, 0x1234aa, 0x42);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_false(result.accepted);
    g_assert_true(result.matched);
    g_assert_cmpuint(result.filter_index, ==, 0);

    set_ext_filter(&test, 0, 3, 7, 0x123401, 0);
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.accepted);
    g_assert_cmpuint(result.filter_index, ==, 1);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_1);
}

static void test_global_filter(void)
{
    TestMCan test;
    MCanFilterResult result;
    qemu_can_frame frame = make_frame(0x123, 1);

    test_m_can_init(&test);
    test.config.std_filters = 0;
    test.config.ext_filters = 0;
    test.config.nonmatching_standard = M_CAN_NONMATCHING_FIFO1;

    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.accepted);
    g_assert_false(result.matched);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_1);

    test.config.nonmatching_extended = M_CAN_NONMATCHING_FIFO0;
    frame.can_id = QEMU_CAN_EFF_FLAG | 0x1234567;
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_true(result.accepted);
    g_assert_false(result.matched);
    g_assert_cmpuint(result.fifo, ==, M_CAN_FIFO_0);

    test.config.reject_remote_extended = true;
    frame.can_id |= QEMU_CAN_RTR_FLAG;
    g_assert_true(m_can_filter_frame(&test.engine, &frame, &result));
    g_assert_false(result.accepted);

    frame.can_id = QEMU_CAN_ERR_FLAG;
    g_assert_false(m_can_filter_frame(&test.engine, &frame, &result));
}

static void test_receive_classic(void)
{
    TestMCan test;
    qemu_can_frame frame = make_frame(0x65, 3);
    MCanFifoStatus status;
    uint8_t *element;

    test_m_can_init(&test);
    test.config.std_filters = 1;
    set_std_filter(&test, 0, 2, 1, 0x65, 0x7ff);
    g_assert_cmphex((uint32_t)ldl_le_p(test.mram +
                                      test_layout.std_filter), ==,
                    0x886507ff);

    g_assert_true(m_can_can_receive(&test.engine));
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    status = m_can_rx_fifo_status(&test.engine, 0);
    g_assert_cmpuint(status.fill, ==, 1);
    g_assert_cmpuint(status.get_index, ==, 0);
    g_assert_cmpuint(status.put_index, ==, 1);
    g_assert_false(status.full);
    g_assert_false(status.lost);
    g_assert_cmphex(test.events, ==, M_CAN_EVENT_RX_FIFO0_NEW);

    element = test.mram + test_layout.rx_fifo[0];
    g_assert_cmphex((uint32_t)ldl_le_p(element), ==, 0x65U << 18);
    g_assert_cmphex((uint32_t)ldl_le_p(element + 4), ==, 3U << 16);
    g_assert_cmpmem(element + 8, 3, frame.data, 3);

    test.events = 0;
    frame.can_id = 0x66;
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    g_assert_cmpuint(m_can_rx_fifo_status(&test.engine, 0).fill, ==, 1);
    g_assert_cmphex(test.events, ==, 0);

    test.config.enabled = false;
    g_assert_false(m_can_can_receive(&test.engine));
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 0);
}

static void test_receive_fd_and_nonmatching(void)
{
    TestMCan test;
    qemu_can_frame frame = make_frame(QEMU_CAN_EFF_FLAG | 0x1234567, 64);
    uint8_t *element;
    uint32_t word1;

    test_m_can_init(&test);
    test.config.fd_enabled = true;
    test.config.ext_filters = 1;
    frame.flags = QEMU_CAN_FRMF_TYPE_FD |
                  QEMU_CAN_FRMF_BRS | QEMU_CAN_FRMF_ESI;
    set_ext_filter(&test, 0, 1, 2, 0x1234567, 0x42);

    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    element = test.mram + test_layout.rx_fifo[1];
    g_assert_cmphex((uint32_t)ldl_le_p(element), ==,
                    TX_ID_XTD | TX_ID_ESI | 0x1234567);
    word1 = ldl_le_p(element + 4);
    g_assert_cmphex(word1, ==,
                    (15U << TX_DLC_SHIFT) | TX_BRS | TX_FDF);
    g_assert_cmpmem(element + 8, 64, frame.data, 64);
    g_assert_cmphex(test.events, ==, M_CAN_EVENT_RX_FIFO1_NEW);

    test_m_can_init(&test);
    test.config.std_filters = 0;
    test.config.nonmatching_standard = M_CAN_NONMATCHING_FIFO0;
    frame = make_frame(0x321, 1);
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    element = test.mram + test_layout.rx_fifo[0];
    word1 = ldl_le_p(element + 4);
    g_assert_cmphex(word1, ==, RX_ANMF |
                    (0x7fU << RX_FIDX_SHIFT) |
                    (1U << TX_DLC_SHIFT));

    test_m_can_init(&test);
    test.config.fd_enabled = false;
    frame.flags = QEMU_CAN_FRMF_TYPE_FD;
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    g_assert_cmpuint(m_can_rx_fifo_status(&test.engine, 0).fill, ==, 0);
}

static void test_high_priority(void)
{
    TestMCan test;
    qemu_can_frame frame = make_frame(0x123, 1);
    MCanHighPriorityState status;

    test_m_can_init(&test);
    test.config.std_filters = 1;
    set_std_filter(&test, 0, 1, 5, 0x123, 0x456);

    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    status = m_can_high_priority_status(&test.engine);
    g_assert_true(status.valid);
    g_assert_false(status.extended);
    g_assert_cmpuint(status.filter_index, ==, 0);
    g_assert_cmpuint(status.storage, ==, M_CAN_HIGH_PRIORITY_FIFO0);
    g_assert_cmpuint(status.buffer_index, ==, 0);
    g_assert_cmphex(test.events, ==,
                    M_CAN_EVENT_HIGH_PRIORITY |
                    M_CAN_EVENT_RX_FIFO0_NEW);

    test.events = 0;
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    status = m_can_high_priority_status(&test.engine);
    g_assert_cmpuint(status.storage, ==, M_CAN_HIGH_PRIORITY_FIFO0);
    g_assert_cmpuint(status.buffer_index, ==, 1);
    g_assert_cmphex(test.events, ==,
                    M_CAN_EVENT_HIGH_PRIORITY |
                    M_CAN_EVENT_RX_FIFO0_NEW);

    test_m_can_init(&test);
    test.config.std_filters = 1;
    set_std_filter(&test, 0, 1, 4, 0x123, 0x456);
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    g_assert_cmpuint(m_can_rx_fifo_status(&test.engine, 0).fill, ==, 0);
    status = m_can_high_priority_status(&test.engine);
    g_assert_true(status.valid);
    g_assert_cmpuint(status.storage, ==, M_CAN_HIGH_PRIORITY_NO_FIFO);
    g_assert_cmpuint(status.buffer_index, ==, 0);
    g_assert_cmphex(test.events, ==, M_CAN_EVENT_HIGH_PRIORITY);
}

static void test_rx_fifo_ack(void)
{
    TestMCan test;
    qemu_can_frame frames[3];
    MCanFifoStatus status;

    test_m_can_init(&test);
    test.config.std_filters = 0;
    test.config.nonmatching_standard = M_CAN_NONMATCHING_FIFO0;
    for (unsigned int i = 0; i < ARRAY_SIZE(frames); i++) {
        frames[i] = make_frame(0x100 + i, 1);
    }

    g_assert_cmpint(m_can_receive(&test.engine, frames,
                                  ARRAY_SIZE(frames)), ==,
                    ARRAY_SIZE(frames));
    status = m_can_rx_fifo_status(&test.engine, 0);
    g_assert_cmpuint(status.fill, ==, 3);
    g_assert_cmpuint(status.put_index, ==, 0);
    g_assert_cmpuint(status.get_index, ==, 0);
    g_assert_true(status.full);
    g_assert_cmphex(test.events & M_CAN_EVENT_RX_FIFO0_FULL, !=, 0);

    m_can_rx_fifo_ack(&test.engine, 0, 1);
    status = m_can_rx_fifo_status(&test.engine, 0);
    g_assert_cmpuint(status.fill, ==, 1);
    g_assert_cmpuint(status.get_index, ==, 2);

    m_can_rx_fifo_ack(&test.engine, 0, 2);
    status = m_can_rx_fifo_status(&test.engine, 0);
    g_assert_cmpuint(status.fill, ==, 0);
    g_assert_cmpuint(status.get_index, ==, 0);
}

static void test_rx_fifo_blocking(void)
{
    TestMCan test;
    qemu_can_frame frames[4];
    MCanFifoStatus status;
    uint32_t first_id;

    test_m_can_init(&test);
    test.config.std_filters = 1;
    set_std_filter(&test, 0, 2, 5, 0x100, 0x7fc);
    for (unsigned int i = 0; i < ARRAY_SIZE(frames); i++) {
        frames[i] = make_frame(0x100 + i, 1);
    }

    g_assert_cmpint(m_can_receive(&test.engine, frames, 3), ==, 3);
    first_id = rx_element_id(&test, 0, 0);
    test.events = 0;
    g_assert_cmpint(m_can_receive(&test.engine, &frames[3], 1), ==, 1);
    status = m_can_rx_fifo_status(&test.engine, 0);
    g_assert_cmpuint(status.fill, ==, 3);
    g_assert_cmpuint(status.get_index, ==, 0);
    g_assert_cmpuint(status.put_index, ==, 0);
    g_assert_true(status.lost);
    g_assert_cmphex(test.events, ==,
                    M_CAN_EVENT_RX_FIFO0_LOST |
                    M_CAN_EVENT_HIGH_PRIORITY);
    g_assert_cmpuint(m_can_high_priority_status(&test.engine).storage, ==,
                     M_CAN_HIGH_PRIORITY_FIFO_LOST);
    g_assert_cmpuint(m_can_high_priority_status(&test.engine).buffer_index,
                     ==, 0);
    g_assert_cmphex(rx_element_id(&test, 0, 0), ==, first_id);

    m_can_rx_fifo_clear_lost(&test.engine, 0);
    g_assert_false(m_can_rx_fifo_status(&test.engine, 0).lost);
}

static void test_rx_fifo_overwrite(void)
{
    TestMCan test;
    qemu_can_frame frames[4];
    MCanFifoStatus status;

    test_m_can_init(&test);
    test.config.std_filters = 1;
    set_std_filter(&test, 0, 2, 5, 0x100, 0x7fc);
    test.config.rx_fifo_overwrite[0] = true;
    for (unsigned int i = 0; i < ARRAY_SIZE(frames); i++) {
        frames[i] = make_frame(0x100 + i, 1);
    }

    g_assert_cmpint(m_can_receive(&test.engine, frames, 3), ==, 3);
    test.events = 0;
    g_assert_cmpint(m_can_receive(&test.engine, &frames[3], 1), ==, 1);
    status = m_can_rx_fifo_status(&test.engine, 0);
    g_assert_cmpuint(status.fill, ==, 3);
    g_assert_cmpuint(status.get_index, ==, 1);
    g_assert_cmpuint(status.put_index, ==, 1);
    g_assert_false(status.lost);
    g_assert_cmphex(test.events, ==,
                    M_CAN_EVENT_RX_FIFO0_NEW |
                    M_CAN_EVENT_HIGH_PRIORITY);
    g_assert_cmpuint(m_can_high_priority_status(&test.engine).storage, ==,
                     M_CAN_HIGH_PRIORITY_FIFO0);
    g_assert_cmpuint(m_can_high_priority_status(&test.engine).buffer_index,
                     ==, 0);
    g_assert_cmphex(rx_element_id(&test, 0, 0), ==, 0x103U << 18);
}

static void test_tx_event_fifo(void)
{
    TestMCan test;
    MCanTxTransfer transfer = {
        .frame = {
            .can_id = QEMU_CAN_EFF_FLAG | 0x1234567,
            .can_dlc = 64,
            .flags = QEMU_CAN_FRMF_TYPE_FD | QEMU_CAN_FRMF_BRS,
        },
        .header_word = TX_ID_XTD | 0x1234567,
        .dlc = 15,
        .message_marker = 0x5a,
        .event_fifo_control = true,
    };
    MCanFifoStatus status;
    uint8_t *element;

    test_m_can_init(&test);
    g_assert_true(m_can_tx_event_append(&test.engine, &transfer, 1));
    status = m_can_tx_event_status(&test.engine);
    g_assert_cmpuint(status.fill, ==, 1);
    g_assert_cmpuint(status.get_index, ==, 0);
    g_assert_cmpuint(status.put_index, ==, 1);
    g_assert_false(status.full);
    g_assert_cmphex(test.events, ==, M_CAN_EVENT_TX_EVENT_NEW);

    element = test.mram + test_layout.tx_event;
    g_assert_cmphex((uint32_t)ldl_le_p(element), ==,
                    transfer.header_word);
    g_assert_cmphex((uint32_t)ldl_le_p(element + 4), ==,
                    (15U << TX_DLC_SHIFT) | TX_BRS | TX_FDF |
                    (1U << 22) | (0x5aU << TX_MM_SHIFT));

    g_assert_true(m_can_tx_event_append(&test.engine, &transfer, 1));
    test.events = 0;
    g_assert_true(m_can_tx_event_append(&test.engine, &transfer, 1));
    g_assert_cmphex(test.events, ==,
                    M_CAN_EVENT_TX_EVENT_NEW |
                    M_CAN_EVENT_TX_EVENT_FULL);

    test.events = 0;
    g_assert_false(m_can_tx_event_append(&test.engine, &transfer, 1));
    status = m_can_tx_event_status(&test.engine);
    g_assert_cmpuint(status.fill, ==, 3);
    g_assert_true(status.lost);
    g_assert_cmphex(test.events, ==, M_CAN_EVENT_TX_EVENT_LOST);

    m_can_tx_event_ack(&test.engine, 1);
    status = m_can_tx_event_status(&test.engine);
    g_assert_cmpuint(status.fill, ==, 1);
    g_assert_cmpuint(status.get_index, ==, 2);
    m_can_tx_event_clear_lost(&test.engine);
    g_assert_false(m_can_tx_event_status(&test.engine).lost);
}

static void test_reset_retains_mram(void)
{
    TestMCan test;
    qemu_can_frame frame = make_frame(0x123, 1);
    MCanTxTransfer transfer = {
        .frame = {
            .can_id = 0x123,
            .can_dlc = 1,
        },
        .header_word = 0x123U << 18,
        .dlc = 1,
        .event_fifo_control = true,
    };
    uint32_t rx_element;
    uint32_t tx_event_element;

    test_m_can_init(&test);
    test.config.std_filters = 0;
    test.config.nonmatching_standard = M_CAN_NONMATCHING_FIFO0;
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    rx_element = rx_element_id(&test, 0, 0);

    test.config.std_filters = 1;
    set_std_filter(&test, 0, 1, 4, 0x123, 0x456);
    g_assert_cmpint(m_can_receive(&test.engine, &frame, 1), ==, 1);
    g_assert_true(m_can_high_priority_status(&test.engine).valid);

    g_assert_true(m_can_tx_event_append(&test.engine, &transfer, 1));
    g_assert_cmpuint(m_can_tx_event_status(&test.engine).fill, ==, 1);
    tx_event_element = ldl_le_p(test.mram + test_layout.tx_event);

    m_can_engine_reset(&test.engine);
    g_assert_cmpuint(m_can_rx_fifo_status(&test.engine, 0).fill, ==, 0);
    g_assert_cmpuint(m_can_tx_event_status(&test.engine).fill, ==, 0);
    g_assert_false(m_can_high_priority_status(&test.engine).valid);
    g_assert_cmpuint(m_can_high_priority_status(&test.engine).storage, ==,
                     M_CAN_HIGH_PRIORITY_NO_FIFO);
    g_assert_cmpuint(m_can_high_priority_status(&test.engine).buffer_index,
                     ==, 0);
    g_assert_cmphex(rx_element_id(&test, 0, 0), ==, rx_element);
    g_assert_cmphex((uint32_t)ldl_le_p(test.mram +
                                      test_layout.tx_event), ==,
                    tx_event_element);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/m-can/init/layout-validation",
                    test_layout_validation);
    g_test_add_func("/m-can/tx/classic-element", test_tx_classic_element);
    g_test_add_func("/m-can/tx/classic-normalizes-fd",
                    test_tx_classic_normalizes_fd_bits);
    g_test_add_func("/m-can/tx/fd-element", test_tx_fd_element);
    g_test_add_func("/m-can/filter/standard-types",
                    test_standard_filter_types);
    g_test_add_func("/m-can/filter/actions-and-order",
                    test_filter_actions_and_order);
    g_test_add_func("/m-can/filter/extended-types",
                    test_extended_filter_types);
    g_test_add_func("/m-can/filter/global", test_global_filter);
    g_test_add_func("/m-can/rx/classic", test_receive_classic);
    g_test_add_func("/m-can/rx/fd-and-nonmatching",
                    test_receive_fd_and_nonmatching);
    g_test_add_func("/m-can/rx/high-priority", test_high_priority);
    g_test_add_func("/m-can/rx/fifo-ack", test_rx_fifo_ack);
    g_test_add_func("/m-can/rx/fifo-blocking", test_rx_fifo_blocking);
    g_test_add_func("/m-can/rx/fifo-overwrite", test_rx_fifo_overwrite);
    g_test_add_func("/m-can/tx-event/fifo", test_tx_event_fifo);
    g_test_add_func("/m-can/reset/retains-mram", test_reset_retains_mram);

    return g_test_run();
}

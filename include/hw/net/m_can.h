/*
 * Bosch M_CAN message engine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_M_CAN_H
#define HW_NET_M_CAN_H

#include "net/can_emu.h"

#define M_CAN_RX_FIFO_COUNT 2
#define M_CAN_ELEMENT_SIZE 72
#define M_CAN_TX_EVENT_ELEMENT_SIZE 8

typedef struct MCanMsgRamLayout {
    uint16_t std_filter;
    uint16_t ext_filter;
    uint16_t rx_fifo[M_CAN_RX_FIFO_COUNT];
    uint16_t tx_event;
    uint16_t tx_buffer;
    uint8_t std_filters;
    uint8_t ext_filters;
    uint8_t rx_elements[M_CAN_RX_FIFO_COUNT];
    uint8_t tx_events;
    uint8_t tx_buffers;
} MCanMsgRamLayout;

typedef enum MCanNonmatching {
    M_CAN_NONMATCHING_FIFO0,
    M_CAN_NONMATCHING_FIFO1,
    M_CAN_NONMATCHING_REJECT,
} MCanNonmatching;

typedef struct MCanEngineConfig {
    bool enabled;
    bool fd_enabled;
    bool brs_enabled;
    bool reject_remote_standard;
    bool reject_remote_extended;
    bool rx_fifo_overwrite[M_CAN_RX_FIFO_COUNT];
    uint8_t std_filters;
    uint8_t ext_filters;
    MCanNonmatching nonmatching_standard;
    MCanNonmatching nonmatching_extended;
    uint32_t extended_id_mask;
} MCanEngineConfig;

typedef enum MCanEvent {
    M_CAN_EVENT_RX_FIFO0_NEW = 1U << 0,
    M_CAN_EVENT_RX_FIFO0_FULL = 1U << 1,
    M_CAN_EVENT_RX_FIFO0_LOST = 1U << 2,
    M_CAN_EVENT_RX_FIFO1_NEW = 1U << 3,
    M_CAN_EVENT_RX_FIFO1_FULL = 1U << 4,
    M_CAN_EVENT_RX_FIFO1_LOST = 1U << 5,
    M_CAN_EVENT_HIGH_PRIORITY = 1U << 6,
    M_CAN_EVENT_TX_EVENT_NEW = 1U << 7,
    M_CAN_EVENT_TX_EVENT_FULL = 1U << 8,
    M_CAN_EVENT_TX_EVENT_LOST = 1U << 9,
} MCanEvent;

typedef struct MCanFifoState {
    uint8_t fill;
    uint8_t get_index;
    uint8_t put_index;
    bool lost;
} MCanFifoState;

typedef struct MCanFifoStatus {
    uint8_t fill;
    uint8_t get_index;
    uint8_t put_index;
    bool full;
    bool lost;
} MCanFifoStatus;

typedef enum MCanFifo {
    M_CAN_FIFO_NONE,
    M_CAN_FIFO_0,
    M_CAN_FIFO_1,
} MCanFifo;

typedef struct MCanFilterResult {
    bool accepted;
    bool matched;
    bool priority;
    bool extended;
    uint8_t filter_index;
    MCanFifo fifo;
} MCanFilterResult;

typedef enum MCanHighPriorityStorage {
    M_CAN_HIGH_PRIORITY_NO_FIFO,
    M_CAN_HIGH_PRIORITY_FIFO_LOST,
    M_CAN_HIGH_PRIORITY_FIFO0,
    M_CAN_HIGH_PRIORITY_FIFO1,
} MCanHighPriorityStorage;

typedef struct MCanHighPriorityState {
    bool valid;
    bool extended;
    uint8_t filter_index;
    uint8_t storage;
    uint8_t buffer_index;
} MCanHighPriorityState;

typedef struct MCanTxTransfer {
    qemu_can_frame frame;
    uint32_t header_word;
    uint8_t dlc;
    uint8_t message_marker;
    bool event_fifo_control;
} MCanTxTransfer;

typedef struct MCanEngineOps {
    void (*get_config)(void *opaque, MCanEngineConfig *config);
    void (*event)(void *opaque, uint32_t events);
} MCanEngineOps;

typedef struct MCanEngine {
    uint8_t *message_ram;
    size_t message_ram_size;
    MCanMsgRamLayout layout;
    const MCanEngineOps *ops;
    void *opaque;

    MCanFifoState rx_fifo[M_CAN_RX_FIFO_COUNT];
    MCanFifoState tx_event_fifo;
    MCanHighPriorityState high_priority;
} MCanEngine;

bool m_can_engine_init(MCanEngine *engine, uint8_t *message_ram,
                       size_t message_ram_size,
                       const MCanMsgRamLayout *layout,
                       const MCanEngineOps *ops, void *opaque);
void m_can_engine_reset(MCanEngine *engine);

bool m_can_can_receive(MCanEngine *engine);
ssize_t m_can_receive(MCanEngine *engine, const qemu_can_frame *frames,
                      size_t count);

bool m_can_filter_frame(MCanEngine *engine, const qemu_can_frame *frame,
                        MCanFilterResult *result);
bool m_can_tx_element_decode(MCanEngine *engine, uint8_t index,
                             MCanTxTransfer *transfer);

bool m_can_tx_event_append(MCanEngine *engine,
                           const MCanTxTransfer *transfer,
                           uint8_t event_type);
void m_can_rx_fifo_ack(MCanEngine *engine, unsigned int fifo, uint8_t index);
void m_can_tx_event_ack(MCanEngine *engine, uint8_t index);
void m_can_rx_fifo_clear_lost(MCanEngine *engine, unsigned int fifo);
void m_can_tx_event_clear_lost(MCanEngine *engine);

MCanFifoStatus m_can_rx_fifo_status(const MCanEngine *engine,
                                    unsigned int fifo);
MCanFifoStatus m_can_tx_event_status(const MCanEngine *engine);
MCanHighPriorityState
m_can_high_priority_status(const MCanEngine *engine);

extern const VMStateDescription vmstate_m_can_engine;

#endif

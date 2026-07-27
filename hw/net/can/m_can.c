/*
 * Bosch M_CAN message engine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/net/m_can.h"
#include "migration/vmstate.h"
#include "qemu/bswap.h"

#define M_CAN_ID_RTR (1U << 29)
#define M_CAN_ID_XTD (1U << 30)
#define M_CAN_ID_ESI (1U << 31)

#define M_CAN_DLC_SHIFT 16
#define M_CAN_DLC_MASK 0xfU
#define M_CAN_BRS (1U << 20)
#define M_CAN_FDF (1U << 21)
#define M_CAN_EFC (1U << 23)
#define M_CAN_MM_SHIFT 24

#define M_CAN_RX_FILTER_INDEX_SHIFT 24
#define M_CAN_RX_NONMATCHING (1U << 31)
#define M_CAN_NONMATCHING_FILTER_INDEX 0x7f

#define M_CAN_STD_ID_MASK 0x7ffU
#define M_CAN_STD_ID_SHIFT 18
#define M_CAN_STD_FILTER_ID1_SHIFT 16
#define M_CAN_STD_FILTER_ACTION_SHIFT 27
#define M_CAN_STD_FILTER_TYPE_SHIFT 30

#define M_CAN_EXT_ID_MASK 0x1fffffffU
#define M_CAN_EXT_FILTER_ACTION_SHIFT 29
#define M_CAN_EXT_FILTER_TYPE_SHIFT 30

enum {
    M_CAN_FILTER_DISABLE,
    M_CAN_FILTER_FIFO0,
    M_CAN_FILTER_FIFO1,
    M_CAN_FILTER_REJECT,
    M_CAN_FILTER_PRIORITY,
    M_CAN_FILTER_PRIORITY_FIFO0,
    M_CAN_FILTER_PRIORITY_FIFO1,
    M_CAN_FILTER_RX_BUFFER,
};

enum {
    M_CAN_STD_FILTER_RANGE,
    M_CAN_STD_FILTER_DUAL,
    M_CAN_STD_FILTER_CLASSIC,
    M_CAN_STD_FILTER_DISABLED,
};

enum {
    M_CAN_EXT_FILTER_RANGE_MASKED,
    M_CAN_EXT_FILTER_DUAL,
    M_CAN_EXT_FILTER_CLASSIC,
    M_CAN_EXT_FILTER_RANGE,
};

static bool m_can_region_fits(size_t ram_size, uint16_t offset,
                              uint8_t count, size_t element_size)
{
    return offset <= ram_size &&
           count <= (ram_size - offset) / element_size;
}

static bool m_can_layout_valid(size_t ram_size,
                               const MCanMsgRamLayout *layout)
{
    return m_can_region_fits(ram_size, layout->std_filter,
                             layout->std_filters, sizeof(uint32_t)) &&
           m_can_region_fits(ram_size, layout->ext_filter,
                             layout->ext_filters, 2 * sizeof(uint32_t)) &&
           m_can_region_fits(ram_size, layout->rx_fifo[0],
                             layout->rx_elements[0], M_CAN_ELEMENT_SIZE) &&
           m_can_region_fits(ram_size, layout->rx_fifo[1],
                             layout->rx_elements[1], M_CAN_ELEMENT_SIZE) &&
           m_can_region_fits(ram_size, layout->tx_event,
                             layout->tx_events,
                             M_CAN_TX_EVENT_ELEMENT_SIZE) &&
           m_can_region_fits(ram_size, layout->tx_buffer,
                             layout->tx_buffers, M_CAN_ELEMENT_SIZE);
}

static MCanEngineConfig m_can_get_config(MCanEngine *engine)
{
    MCanEngineConfig config = {
        .extended_id_mask = M_CAN_EXT_ID_MASK,
    };

    engine->ops->get_config(engine->opaque, &config);
    config.std_filters = MIN(config.std_filters,
                             engine->layout.std_filters);
    config.ext_filters = MIN(config.ext_filters,
                             engine->layout.ext_filters);

    return config;
}

static void m_can_event(MCanEngine *engine, uint32_t events)
{
    if (events) {
        engine->ops->event(engine->opaque, events);
    }
}

bool m_can_engine_init(MCanEngine *engine, uint8_t *message_ram,
                       size_t message_ram_size,
                       const MCanMsgRamLayout *layout,
                       const MCanEngineOps *ops, void *opaque)
{
    if (!engine || !message_ram || !layout || !ops ||
        !ops->get_config || !ops->event ||
        !m_can_layout_valid(message_ram_size, layout)) {
        return false;
    }

    *engine = (MCanEngine) {
        .message_ram = message_ram,
        .message_ram_size = message_ram_size,
        .layout = *layout,
        .ops = ops,
        .opaque = opaque,
    };

    return true;
}

void m_can_engine_reset(MCanEngine *engine)
{
    memset(engine->rx_fifo, 0, sizeof(engine->rx_fifo));
    memset(&engine->tx_event_fifo, 0, sizeof(engine->tx_event_fifo));
    memset(&engine->high_priority, 0, sizeof(engine->high_priority));
}

bool m_can_can_receive(MCanEngine *engine)
{
    return m_can_get_config(engine).enabled;
}

static void m_can_filter_action(unsigned int action, bool extended,
                                unsigned int index,
                                MCanFilterResult *result)
{
    *result = (MCanFilterResult) {
        .matched = true,
        .extended = extended,
        .filter_index = index,
    };

    switch (action) {
    case M_CAN_FILTER_FIFO0:
        result->accepted = true;
        result->fifo = M_CAN_FIFO_0;
        break;
    case M_CAN_FILTER_FIFO1:
        result->accepted = true;
        result->fifo = M_CAN_FIFO_1;
        break;
    case M_CAN_FILTER_PRIORITY:
        result->accepted = true;
        result->priority = true;
        break;
    case M_CAN_FILTER_PRIORITY_FIFO0:
        result->accepted = true;
        result->priority = true;
        result->fifo = M_CAN_FIFO_0;
        break;
    case M_CAN_FILTER_PRIORITY_FIFO1:
        result->accepted = true;
        result->priority = true;
        result->fifo = M_CAN_FIFO_1;
        break;
    case M_CAN_FILTER_REJECT:
    case M_CAN_FILTER_RX_BUFFER:
    default:
        break;
    }
}

static bool m_can_standard_filter(MCanEngine *engine,
                                  const MCanEngineConfig *config,
                                  uint16_t id, MCanFilterResult *result)
{
    for (unsigned int i = 0; i < config->std_filters; i++) {
        uint32_t filter = ldl_le_p(engine->message_ram +
                                   engine->layout.std_filter +
                                   i * sizeof(filter));
        unsigned int type = filter >> M_CAN_STD_FILTER_TYPE_SHIFT;
        unsigned int action =
            (filter >> M_CAN_STD_FILTER_ACTION_SHIFT) & 0x7;
        uint16_t id1 =
            (filter >> M_CAN_STD_FILTER_ID1_SHIFT) & M_CAN_STD_ID_MASK;
        uint16_t id2 = filter & M_CAN_STD_ID_MASK;
        bool match;

        if (action == M_CAN_FILTER_DISABLE) {
            continue;
        }
        if (action == M_CAN_FILTER_RX_BUFFER) {
            match = id == id1;
        } else {
            switch (type) {
            case M_CAN_STD_FILTER_RANGE:
                match = id >= id1 && id <= id2;
                break;
            case M_CAN_STD_FILTER_DUAL:
                match = id == id1 || id == id2;
                break;
            case M_CAN_STD_FILTER_CLASSIC:
                match = (id & id2) == (id1 & id2);
                break;
            case M_CAN_STD_FILTER_DISABLED:
            default:
                match = false;
                break;
            }
        }

        if (match) {
            m_can_filter_action(action, false, i, result);
            return true;
        }
    }

    return false;
}

static bool m_can_extended_filter(MCanEngine *engine,
                                  const MCanEngineConfig *config,
                                  uint32_t id, MCanFilterResult *result)
{
    uint32_t masked_id = id & config->extended_id_mask &
                         M_CAN_EXT_ID_MASK;

    for (unsigned int i = 0; i < config->ext_filters; i++) {
        uint8_t *element = engine->message_ram +
                           engine->layout.ext_filter +
                           i * 2 * sizeof(uint32_t);
        uint32_t word0 = ldl_le_p(element);
        uint32_t word1 = ldl_le_p(element + sizeof(uint32_t));
        unsigned int action = word0 >> M_CAN_EXT_FILTER_ACTION_SHIFT;
        unsigned int type = word1 >> M_CAN_EXT_FILTER_TYPE_SHIFT;
        uint32_t id1 = word0 & M_CAN_EXT_ID_MASK;
        uint32_t id2 = word1 & M_CAN_EXT_ID_MASK;
        bool match;

        if (action == M_CAN_FILTER_DISABLE) {
            continue;
        }
        if (action == M_CAN_FILTER_RX_BUFFER) {
            match = masked_id == id1;
        } else {
            switch (type) {
            case M_CAN_EXT_FILTER_RANGE_MASKED:
                match = masked_id >= id1 && masked_id <= id2;
                break;
            case M_CAN_EXT_FILTER_DUAL:
                match = id == id1 || id == id2;
                break;
            case M_CAN_EXT_FILTER_CLASSIC:
                match = (id & id2) == (id1 & id2);
                break;
            case M_CAN_EXT_FILTER_RANGE:
                match = id >= id1 && id <= id2;
                break;
            default:
                match = false;
                break;
            }
        }

        if (match) {
            m_can_filter_action(action, true, i, result);
            return true;
        }
    }

    return false;
}

static void m_can_nonmatching_filter(MCanNonmatching action, bool extended,
                                     MCanFilterResult *result)
{
    *result = (MCanFilterResult) {
        .extended = extended,
        .filter_index = M_CAN_NONMATCHING_FILTER_INDEX,
    };

    switch (action) {
    case M_CAN_NONMATCHING_FIFO0:
        result->accepted = true;
        result->fifo = M_CAN_FIFO_0;
        break;
    case M_CAN_NONMATCHING_FIFO1:
        result->accepted = true;
        result->fifo = M_CAN_FIFO_1;
        break;
    case M_CAN_NONMATCHING_REJECT:
    default:
        break;
    }
}

bool m_can_filter_frame(MCanEngine *engine, const qemu_can_frame *frame,
                        MCanFilterResult *result)
{
    MCanEngineConfig config = m_can_get_config(engine);
    bool extended = frame->can_id & QEMU_CAN_EFF_FLAG;
    bool remote = frame->can_id & QEMU_CAN_RTR_FLAG;
    bool matched;

    if (frame->can_id & QEMU_CAN_ERR_FLAG) {
        return false;
    }

    if (remote && (extended ? config.reject_remote_extended :
                              config.reject_remote_standard)) {
        *result = (MCanFilterResult) {
            .extended = extended,
            .filter_index = M_CAN_NONMATCHING_FILTER_INDEX,
        };
        return true;
    }

    if (extended) {
        matched = m_can_extended_filter(engine, &config,
                                        frame->can_id & M_CAN_EXT_ID_MASK,
                                        result);
        if (!matched) {
            m_can_nonmatching_filter(config.nonmatching_extended, true,
                                     result);
        }
    } else {
        matched = m_can_standard_filter(engine, &config,
                                        frame->can_id & M_CAN_STD_ID_MASK,
                                        result);
        if (!matched) {
            m_can_nonmatching_filter(config.nonmatching_standard, false,
                                     result);
        }
    }

    return true;
}

bool m_can_tx_element_decode(MCanEngine *engine, uint8_t index,
                             MCanTxTransfer *transfer)
{
    MCanEngineConfig config;
    uint8_t *element;
    uint32_t word0;
    uint32_t word1;
    uint8_t dlc;
    size_t length;
    bool fd;

    if (index >= engine->layout.tx_buffers) {
        return false;
    }

    config = m_can_get_config(engine);
    element = engine->message_ram + engine->layout.tx_buffer +
              index * M_CAN_ELEMENT_SIZE;
    word0 = ldl_le_p(element);
    word1 = ldl_le_p(element + sizeof(uint32_t));
    dlc = (word1 >> M_CAN_DLC_SHIFT) & M_CAN_DLC_MASK;
    fd = config.fd_enabled && (word1 & M_CAN_FDF) &&
         !(word0 & M_CAN_ID_RTR);
    length = fd ? can_dlc2len(dlc) : MIN(dlc, 8);

    *transfer = (MCanTxTransfer) {
        .header_word = word0,
        .dlc = dlc,
        .message_marker = word1 >> M_CAN_MM_SHIFT,
        .event_fifo_control = word1 & M_CAN_EFC,
    };

    if (word0 & M_CAN_ID_XTD) {
        transfer->frame.can_id = QEMU_CAN_EFF_FLAG |
                                 (word0 & M_CAN_EXT_ID_MASK);
    } else {
        transfer->frame.can_id =
            (word0 >> M_CAN_STD_ID_SHIFT) & M_CAN_STD_ID_MASK;
    }
    if (word0 & M_CAN_ID_RTR) {
        transfer->frame.can_id |= QEMU_CAN_RTR_FLAG;
    }
    if (fd) {
        transfer->frame.flags |= QEMU_CAN_FRMF_TYPE_FD;
        if (config.brs_enabled && (word1 & M_CAN_BRS)) {
            transfer->frame.flags |= QEMU_CAN_FRMF_BRS;
        }
        if (word0 & M_CAN_ID_ESI) {
            transfer->frame.flags |= QEMU_CAN_FRMF_ESI;
        }
    }
    transfer->frame.can_dlc = length;
    if (!(word0 & M_CAN_ID_RTR)) {
        memcpy(transfer->frame.data, element + 2 * sizeof(uint32_t),
               length);
    }

    return true;
}

static void m_can_write_rx_element(uint8_t *element,
                                   const qemu_can_frame *frame,
                                   const MCanFilterResult *filter)
{
    bool fd = frame->flags & QEMU_CAN_FRMF_TYPE_FD;
    size_t length = fd ? MIN(frame->can_dlc, 64) :
                         MIN(frame->can_dlc, 8);
    uint8_t dlc = fd ? can_len2dlc(length) : length;
    uint32_t word0;
    uint32_t word1 = (uint32_t)dlc << M_CAN_DLC_SHIFT;

    memset(element, 0, M_CAN_ELEMENT_SIZE);
    if (frame->can_id & QEMU_CAN_EFF_FLAG) {
        word0 = M_CAN_ID_XTD | (frame->can_id & M_CAN_EXT_ID_MASK);
    } else {
        word0 = (frame->can_id & M_CAN_STD_ID_MASK) <<
                M_CAN_STD_ID_SHIFT;
    }
    if (frame->can_id & QEMU_CAN_RTR_FLAG) {
        word0 |= M_CAN_ID_RTR;
    }
    if (fd) {
        word1 |= M_CAN_FDF;
        if (frame->flags & QEMU_CAN_FRMF_BRS) {
            word1 |= M_CAN_BRS;
        }
        if (frame->flags & QEMU_CAN_FRMF_ESI) {
            word0 |= M_CAN_ID_ESI;
        }
    }
    word1 |= (uint32_t)filter->filter_index <<
             M_CAN_RX_FILTER_INDEX_SHIFT;
    if (!filter->matched) {
        word1 |= M_CAN_RX_NONMATCHING;
    }

    stl_le_p(element, word0);
    stl_le_p(element + sizeof(uint32_t), word1);
    if (!(frame->can_id & QEMU_CAN_RTR_FLAG)) {
        memcpy(element + 2 * sizeof(uint32_t), frame->data, length);
    }
}

static int m_can_store_rx_frame(MCanEngine *engine, unsigned int fifo,
                                const qemu_can_frame *frame,
                                const MCanFilterResult *filter,
                                const MCanEngineConfig *config)
{
    static const uint32_t new_events[M_CAN_RX_FIFO_COUNT] = {
        M_CAN_EVENT_RX_FIFO0_NEW,
        M_CAN_EVENT_RX_FIFO1_NEW,
    };
    static const uint32_t full_events[M_CAN_RX_FIFO_COUNT] = {
        M_CAN_EVENT_RX_FIFO0_FULL,
        M_CAN_EVENT_RX_FIFO1_FULL,
    };
    static const uint32_t lost_events[M_CAN_RX_FIFO_COUNT] = {
        M_CAN_EVENT_RX_FIFO0_LOST,
        M_CAN_EVENT_RX_FIFO1_LOST,
    };
    MCanFifoState *state = &engine->rx_fifo[fifo];
    unsigned int capacity = engine->layout.rx_elements[fifo];
    uint32_t events = 0;
    uint8_t *element;
    int buffer_index;

    if (!capacity) {
        state->lost = true;
        m_can_event(engine, lost_events[fifo]);
        return -1;
    }

    if (state->fill == capacity && !config->rx_fifo_overwrite[fifo]) {
        state->lost = true;
        m_can_event(engine, lost_events[fifo]);
        return -1;
    }

    buffer_index = state->put_index;
    element = engine->message_ram + engine->layout.rx_fifo[fifo] +
              buffer_index * M_CAN_ELEMENT_SIZE;
    m_can_write_rx_element(element, frame, filter);

    if (state->fill == capacity) {
        state->put_index = (state->put_index + 1) % capacity;
        state->get_index = (state->get_index + 1) % capacity;
    } else {
        state->put_index = (state->put_index + 1) % capacity;
        state->fill++;
        if (state->fill == capacity) {
            events |= full_events[fifo];
        }
    }
    events |= new_events[fifo];
    m_can_event(engine, events);

    return buffer_index;
}

static void m_can_update_high_priority(MCanEngine *engine,
                                       const MCanFilterResult *filter,
                                       int buffer_index)
{
    MCanHighPriorityStorage storage;

    switch (filter->fifo) {
    case M_CAN_FIFO_0:
        storage = buffer_index < 0 ? M_CAN_HIGH_PRIORITY_FIFO_LOST :
                                     M_CAN_HIGH_PRIORITY_FIFO0;
        break;
    case M_CAN_FIFO_1:
        storage = buffer_index < 0 ? M_CAN_HIGH_PRIORITY_FIFO_LOST :
                                     M_CAN_HIGH_PRIORITY_FIFO1;
        break;
    case M_CAN_FIFO_NONE:
    default:
        storage = M_CAN_HIGH_PRIORITY_NO_FIFO;
        break;
    }

    engine->high_priority = (MCanHighPriorityState) {
        .valid = true,
        .extended = filter->extended,
        .filter_index = filter->filter_index,
        .storage = storage,
        .buffer_index = buffer_index < 0 ? 0 : buffer_index,
    };
    m_can_event(engine, M_CAN_EVENT_HIGH_PRIORITY);
}

ssize_t m_can_receive(MCanEngine *engine, const qemu_can_frame *frames,
                      size_t count)
{
    MCanEngineConfig config = m_can_get_config(engine);
    size_t processed = 0;

    if (!config.enabled) {
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        const qemu_can_frame *frame = &frames[i];
        MCanFilterResult filter;
        bool fd = frame->flags & QEMU_CAN_FRMF_TYPE_FD;
        int buffer_index = -1;

        if (frame->can_id & QEMU_CAN_ERR_FLAG) {
            continue;
        }
        if (frame->can_dlc > 64) {
            continue;
        }

        processed++;
        if ((fd && !config.fd_enabled) ||
            ((frame->flags & QEMU_CAN_FRMF_BRS) &&
             (!fd || !config.brs_enabled))) {
            continue;
        }
        if (!m_can_filter_frame(engine, frame, &filter) ||
            !filter.accepted) {
            continue;
        }

        switch (filter.fifo) {
        case M_CAN_FIFO_0:
            buffer_index =
                m_can_store_rx_frame(engine, 0, frame, &filter, &config);
            break;
        case M_CAN_FIFO_1:
            buffer_index =
                m_can_store_rx_frame(engine, 1, frame, &filter, &config);
            break;
        case M_CAN_FIFO_NONE:
        default:
            break;
        }

        if (filter.priority) {
            m_can_update_high_priority(engine, &filter, buffer_index);
        }
    }

    return processed;
}

bool m_can_tx_event_append(MCanEngine *engine,
                           const MCanTxTransfer *transfer,
                           uint8_t event_type)
{
    MCanFifoState *state = &engine->tx_event_fifo;
    unsigned int capacity = engine->layout.tx_events;
    uint8_t *element;
    uint32_t word1;
    uint32_t events = M_CAN_EVENT_TX_EVENT_NEW;

    if (!transfer->event_fifo_control || event_type < 1 || event_type > 2) {
        return false;
    }
    if (!capacity || state->fill == capacity) {
        state->lost = true;
        m_can_event(engine, M_CAN_EVENT_TX_EVENT_LOST);
        return false;
    }

    element = engine->message_ram + engine->layout.tx_event +
              state->put_index * M_CAN_TX_EVENT_ELEMENT_SIZE;
    word1 = ((uint32_t)transfer->dlc << M_CAN_DLC_SHIFT) |
            ((uint32_t)event_type << 22) |
            ((uint32_t)transfer->message_marker << M_CAN_MM_SHIFT);
    if (transfer->frame.flags & QEMU_CAN_FRMF_TYPE_FD) {
        word1 |= M_CAN_FDF;
    }
    if (transfer->frame.flags & QEMU_CAN_FRMF_BRS) {
        word1 |= M_CAN_BRS;
    }

    stl_le_p(element, transfer->header_word);
    stl_le_p(element + sizeof(uint32_t), word1);
    state->put_index = (state->put_index + 1) % capacity;
    state->fill++;
    if (state->fill == capacity) {
        events |= M_CAN_EVENT_TX_EVENT_FULL;
    }
    m_can_event(engine, events);

    return true;
}

static void m_can_fifo_ack(MCanFifoState *state, unsigned int capacity,
                           uint8_t index)
{
    unsigned int acknowledged;

    if (!capacity || !state->fill) {
        return;
    }

    index %= capacity;
    acknowledged = (index + capacity - state->get_index) % capacity + 1;
    state->get_index = (index + 1) % capacity;
    state->fill -= MIN(acknowledged, state->fill);
}

void m_can_rx_fifo_ack(MCanEngine *engine, unsigned int fifo, uint8_t index)
{
    if (fifo < M_CAN_RX_FIFO_COUNT) {
        m_can_fifo_ack(&engine->rx_fifo[fifo],
                       engine->layout.rx_elements[fifo], index);
    }
}

void m_can_tx_event_ack(MCanEngine *engine, uint8_t index)
{
    m_can_fifo_ack(&engine->tx_event_fifo, engine->layout.tx_events, index);
}

void m_can_rx_fifo_clear_lost(MCanEngine *engine, unsigned int fifo)
{
    if (fifo < M_CAN_RX_FIFO_COUNT) {
        engine->rx_fifo[fifo].lost = false;
    }
}

void m_can_tx_event_clear_lost(MCanEngine *engine)
{
    engine->tx_event_fifo.lost = false;
}

static MCanFifoStatus m_can_fifo_status(const MCanFifoState *state,
                                        unsigned int capacity)
{
    return (MCanFifoStatus) {
        .fill = state->fill,
        .get_index = state->get_index,
        .put_index = state->put_index,
        .full = capacity && state->fill == capacity,
        .lost = state->lost,
    };
}

MCanFifoStatus m_can_rx_fifo_status(const MCanEngine *engine,
                                    unsigned int fifo)
{
    if (fifo < M_CAN_RX_FIFO_COUNT) {
        return m_can_fifo_status(&engine->rx_fifo[fifo],
                                 engine->layout.rx_elements[fifo]);
    }

    return (MCanFifoStatus) { 0 };
}

MCanFifoStatus m_can_tx_event_status(const MCanEngine *engine)
{
    return m_can_fifo_status(&engine->tx_event_fifo,
                             engine->layout.tx_events);
}

MCanHighPriorityState
m_can_high_priority_status(const MCanEngine *engine)
{
    return engine->high_priority;
}

static const VMStateDescription vmstate_m_can_fifo = {
    .name = "m-can-fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(fill, MCanFifoState),
        VMSTATE_UINT8(get_index, MCanFifoState),
        VMSTATE_UINT8(put_index, MCanFifoState),
        VMSTATE_BOOL(lost, MCanFifoState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_m_can_high_priority = {
    .name = "m-can-high-priority",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(valid, MCanHighPriorityState),
        VMSTATE_BOOL(extended, MCanHighPriorityState),
        VMSTATE_UINT8(filter_index, MCanHighPriorityState),
        VMSTATE_UINT8(storage, MCanHighPriorityState),
        VMSTATE_UINT8(buffer_index, MCanHighPriorityState),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_m_can_engine = {
    .name = "m-can-engine",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(rx_fifo, MCanEngine, M_CAN_RX_FIFO_COUNT, 0,
                             vmstate_m_can_fifo, MCanFifoState),
        VMSTATE_STRUCT(tx_event_fifo, MCanEngine, 0, vmstate_m_can_fifo,
                       MCanFifoState),
        VMSTATE_STRUCT(high_priority, MCanEngine, 0,
                       vmstate_m_can_high_priority, MCanHighPriorityState),
        VMSTATE_END_OF_LIST()
    }
};

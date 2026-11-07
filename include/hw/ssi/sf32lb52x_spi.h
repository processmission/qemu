/*
 * SiFli SF32LB52X SPI controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_SF32LB52X_SPI_H
#define HW_SSI_SF32LB52X_SPI_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_SPI "sifli.sf32lb52x-spi"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XSPIState, SF32LB52X_SPI)

#define SF32LB52X_SPI_SIZE 0x1000
#define SF32LB52X_SPI_FIFO_DEPTH 16

/*
 * Framed-transfer latency: a DATA write queues one frame, which the internal
 * shift engine moves to the RX side after this delay. Keeps BSY and the FIFO
 * level fields observable while staying fully deterministic under qtest.
 */
#define SF32LB52X_SPI_SHIFT_NS 1000

struct SF32LB52XSPIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer *shift_timer;

    uint32_t top_ctrl;
    uint32_t fifo_ctrl;
    uint32_t inte;
    uint32_t to;
    uint32_t clk_ctrl;
    uint32_t triwire_ctrl;
    uint32_t rwot_ctrl;
    uint32_t rwot_ccm;
    uint32_t rwot_cvwrn;

    /* STATUS error flags (rw1c): TUR, ROR, TINT. */
    uint32_t status_flags;

    uint32_t tx_fifo[SF32LB52X_SPI_FIFO_DEPTH];
    uint32_t rx_fifo[SF32LB52X_SPI_FIFO_DEPTH];
    uint8_t tx_head;
    uint8_t rx_head;
    uint8_t tx_count;
    uint8_t rx_count;
    bool busy;
};

#endif /* HW_SSI_SF32LB52X_SPI_H */

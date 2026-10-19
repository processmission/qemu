/*
 * SiFli SF32LB52X LCDC display controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_SF32LB52X_LCDC_H
#define HW_DISPLAY_SF32LB52X_LCDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_LCDC "sifli.sf32lb52x-lcdc"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XLCDCState, SF32LB52X_LCDC)

#define SF32LB52X_LCDC_SIZE 0x1000

/* Model-side capture FIFO of single transactions (not guest-visible). */
#define SF32LB52X_LCDC_CAPTURE_FIFO 32

struct SF32LB52XLCDCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    /* Plain read/write registers (canvas, layer, DPI/JDI configuration). */
    uint32_t regs[0x400];

    uint32_t irq_raw;      /* event flags for IRQ.*_STAT/RAW_STAT bits */
    uint32_t setting;      /* SETTING register (mask enables/counters) */
    uint32_t last_wr;      /* LCD_WR staging register */
    uint32_t lcd_rd;       /* LCD_RD read-data register */
    uint32_t single_type;  /* LCD_SINGLE.TYPE (0 command, 1 data) */

    /* Capture FIFO: one entry per LCD_SINGLE write/read trigger. */
    uint32_t cap_data[SF32LB52X_LCDC_CAPTURE_FIFO];
    uint8_t cap_type[SF32LB52X_LCDC_CAPTURE_FIFO];
    uint8_t cap_len[SF32LB52X_LCDC_CAPTURE_FIFO];
    uint8_t cap_is_read[SF32LB52X_LCDC_CAPTURE_FIFO];
    uint8_t cap_head;
    uint8_t cap_num;

    /* QOM debug views of the last captured transaction. */
    uint32_t capture_count;
    uint32_t last_type;
    uint32_t last_data;
    uint32_t last_len;
    uint32_t last_is_read;
    uint32_t read_count;
};

#endif /* HW_DISPLAY_SF32LB52X_LCDC_H */

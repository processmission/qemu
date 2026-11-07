/*
 * SiFli SF32LB52X AUDIO codec
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_SF32LB52X_AUDCODEC_H
#define HW_AUDIO_SF32LB52X_AUDCODEC_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_AUDCODEC "sifli.sf32lb52x-audcodec"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XAudCodecState, SF32LB52X_AUDCODEC)

#define SF32LB52X_AUDCODEC_SIZE 0x400

/* Control/status registers: 32-bit slots 0x00..0xd8 (0x37 slots). */
#define SF32LB52X_AUDCODEC_R_MAX ((0xd8 / 4) + 1)

/* APB data path FIFO depth (SVD APB_STAT fields are 4 bits wide). */
#define SF32LB52X_AUDCODEC_FIFO_DEPTH 16

struct SF32LB52XAudCodecState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;      /* 0x400 control window container */
    MemoryRegion entry_mem; /* manual APB FIFO aperture at 0x40..0x57 */

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[SF32LB52X_AUDCODEC_R_MAX];
    uint32_t regs[SF32LB52X_AUDCODEC_R_MAX];

    /*
     * APB data path: DAC TX FIFOs and ADC RX FIFOs for the two channels.
     * The ADC FIFO is also fed by the DAC FIFO (loopback approximation), so
     * the APB read path is testable without an analog source.
     */
    uint32_t dac_fifo[2][SF32LB52X_AUDCODEC_FIFO_DEPTH];
    uint32_t adc_fifo[2][SF32LB52X_AUDCODEC_FIFO_DEPTH];
    uint32_t dac_fifo_len[2];
    uint32_t adc_fifo_len[2];
    uint32_t dac_last[2];
};

#endif /* HW_AUDIO_SF32LB52X_AUDCODEC_H */

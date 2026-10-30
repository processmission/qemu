/*
 * Sony PlayStation 2 CDVD controller S-command (SCMD) interface
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PS2_SCMD_H
#define HW_MISC_PS2_SCMD_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PS2_SCMD "ps2-scmd"
OBJECT_DECLARE_SIMPLE_TYPE(PS2SCMDState, PS2_SCMD)

/* The CDVD registers start at 0x1f402000. */
#define PS2_CDVD_SIZE           0x40
#define PS2_SCMD_COMMAND        0x16
#define PS2_SCMD_STATUS         0x17
#define PS2_SCMD_SEND           0x17
#define PS2_SCMD_RECV           0x18

#define PS2_SCMD_STATUS_EMPTY   0x40
#define PS2_SCMD_STATUS_BUSY    0x80

#define PS2_SCMD_FIFO_SIZE      16

struct PS2SCMDState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    char *machine_name;

    uint8_t param[PS2_SCMD_FIFO_SIZE];
    uint32_t param_len;
    uint8_t reply[PS2_SCMD_FIFO_SIZE];
    uint32_t reply_len;
    uint32_t reply_pos;
    /* Offset of the guest RTC from the host clock, in seconds. */
    int64_t rtc_offset;
};

#endif

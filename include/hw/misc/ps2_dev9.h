/*
 * Sony PlayStation 2 DEV9 expansion bay and SPEED interface
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PS2_DEV9_H
#define HW_MISC_PS2_DEV9_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_PS2_DEV9 "ps2-dev9"
OBJECT_DECLARE_SIMPLE_TYPE(PS2DEV9State, PS2_DEV9)

/* The DEV9 bridge registers of the IOP start at 0x1f801460. */
#define PS2_DEV9_SIZE           0x20
#define PS2_DEV9_1462           0x02
#define PS2_DEV9_POWER          0x0c
#define PS2_DEV9_REV            0x0e

/*
 * The registers of the SPEED chip in the expansion bay, at 0x14000000 for
 * the EE.  Its ATA interface at offset 0x40 is a separate device.
 */
#define PS2_SPEED_SIZE          0x10000
#define PS2_SPEED_REGS_SIZE     0x100
#define PS2_SPEED_REV_1         0x02
#define PS2_SPEED_REV_3         0x04
#define PS2_SPEED_REV_8         0x0e
#define PS2_SPEED_ATA_BASE      0x40
#define PS2_SPEED_ATA_CTL       0x5c

struct PS2DEV9State {
    SysBusDevice parent_obj;

    MemoryRegion dev9_iomem;
    MemoryRegion speed_iomem;

    uint16_t dev9[PS2_DEV9_SIZE / 2];
    uint16_t speed[PS2_SPEED_REGS_SIZE / 2];
};

#endif

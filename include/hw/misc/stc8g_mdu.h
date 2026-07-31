/*
 * STC8G1K08A MDU16
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STC8G_MDU_H
#define HW_MISC_STC8G_MDU_H

#include "qom/object.h"

#define TYPE_STC8G_MDU "stc8g-mdu"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gMDUState, STC8G_MDU)

#define STC8G_MDU_MMIO_REGS 8

#endif

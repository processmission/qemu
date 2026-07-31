/*
 * STC8G1K08A low-voltage detector
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_STC8G_LVD_H
#define HW_MISC_STC8G_LVD_H

#include "qom/object.h"

#define TYPE_STC8G_LVD "stc8g-lvd"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gLvdState, STC8G_LVD)

enum Stc8gLvdMMIO {
    STC8G_LVD_MMIO_RSTCFG,
};

#endif

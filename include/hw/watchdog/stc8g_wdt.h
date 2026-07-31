/*
 * STC8G1K08A watchdog timer
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_WATCHDOG_STC8G_WDT_H
#define HW_WATCHDOG_STC8G_WDT_H

#include "qom/object.h"

#define TYPE_STC8G_WDT "stc8g-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gWdtState, STC8G_WDT)

enum Stc8gWdtMMIO {
    STC8G_WDT_MMIO_CONTR,
    STC8G_WDT_MMIO_REGS,
};

#endif

/*
 * Haoyu HYM8563 RTC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RTC_HYM8563_H
#define HW_RTC_HYM8563_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_HYM8563 "hym8563"
OBJECT_DECLARE_SIMPLE_TYPE(Hym8563State, HYM8563)

#endif /* HW_RTC_HYM8563_H */

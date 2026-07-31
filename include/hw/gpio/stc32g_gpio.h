/*
 * STC32G GPIO P0-P7
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_STC32G_GPIO_H
#define HW_GPIO_STC32G_GPIO_H

#include "qom/object.h"

#define TYPE_STC32G_GPIO "stc32g-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Stc32gGPIOState, STC32G_GPIO)

#define STC32G_GPIO_PORTS 8
#define STC32G_GPIO_PINS (STC32G_GPIO_PORTS * 8)

#endif

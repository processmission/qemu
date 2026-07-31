/*
 * STC8G1K08A GPIO
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_STC8G_GPIO_H
#define HW_GPIO_STC8G_GPIO_H

#include "qom/object.h"

#define TYPE_STC8G_GPIO "stc8g-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gGPIOState, STC8G_GPIO)

#define STC8G_GPIO_PINS 6

enum Stc8gGPIOPin {
    STC8G_GPIO_P30,
    STC8G_GPIO_P31,
    STC8G_GPIO_P32,
    STC8G_GPIO_P33,
    STC8G_GPIO_P54,
    STC8G_GPIO_P55,
};

#endif

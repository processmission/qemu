/*
 * STC8G1K08A SPI
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_STC8G_SPI_H
#define HW_SSI_STC8G_SPI_H

#include "qom/object.h"

#define TYPE_STC8G_SPI "stc8g-spi"
OBJECT_DECLARE_SIMPLE_TYPE(Stc8gSPIState, STC8G_SPI)

enum Stc8gSPIMMIO {
    STC8G_SPI_MMIO_STAT,
    STC8G_SPI_MMIO_CTL,
    STC8G_SPI_MMIO_DATA,
};

#endif

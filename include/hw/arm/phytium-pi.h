/*
 * Local-only Phytium Pi board model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_PHYTIUM_PI_H
#define HW_ARM_PHYTIUM_PI_H

#include "hw/core/boards.h"

#define TYPE_PHYTIUMPI_MACHINE MACHINE_TYPE_NAME("phytium-pi")
OBJECT_DECLARE_SIMPLE_TYPE(PhytiumPiMachineState, PHYTIUMPI_MACHINE)

#define PHYTIUMPI_MAX_CPUS 4

#endif /* HW_ARM_PHYTIUM_PI_H */

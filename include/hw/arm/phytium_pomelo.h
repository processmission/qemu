/*
 * Phytium D2000 Pomelo board model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_PHYTIUM_POMELO_H
#define HW_ARM_PHYTIUM_POMELO_H

#include "hw/core/boards.h"
#include "qom/object.h"

#define TYPE_PHYTIUM_POMELO_MACHINE MACHINE_TYPE_NAME("phytium-pomelo")
OBJECT_DECLARE_SIMPLE_TYPE(PhytiumPomeloMachineState, PHYTIUM_POMELO_MACHINE)

#endif /* HW_ARM_PHYTIUM_POMELO_H */

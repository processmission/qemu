/*
 * AXERA AX650X hardware spinlock
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_AX650X_HWSPINLOCK_H
#define HW_MISC_AX650X_HWSPINLOCK_H

#include "hw/core/sysbus.h"

#define TYPE_AX650X_HWSPINLOCK "ax650x-hwspinlock"
OBJECT_DECLARE_SIMPLE_TYPE(AX650XHWSpinlockState, AX650X_HWSPINLOCK)

#define AX650X_HWSPINLOCK_MMIO_SIZE 0x1000
#define AX650X_HWSPINLOCK_COUNT     32

typedef struct AX650XHWSpinlockState {
    SysBusDevice parent;

    MemoryRegion iomem;
    uint32_t locked;
} AX650XHWSpinlockState;

#endif /* HW_MISC_AX650X_HWSPINLOCK_H */

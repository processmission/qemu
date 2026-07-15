/*
 * AX650X DWMAC board integration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_AX650X_DWMAC_H
#define HW_ARM_AX650X_DWMAC_H

#include "hw/core/qdev.h"

void ax650x_dwmac_create(DeviceState *gic);
void ax650x_dwmac_create_fdt(void *fdt);

#endif /* HW_ARM_AX650X_DWMAC_H */

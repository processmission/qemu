/*
 * Axera AX650X DWMAC clock/reset/interface glue
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_AX650X_DWMAC_GLUE_H
#define HW_MISC_AX650X_DWMAC_GLUE_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"

#define TYPE_AX650X_DWMAC_GLUE "ax650x-dwmac-glue"
OBJECT_DECLARE_SIMPLE_TYPE(AX650XDWMACGlueState, AX650X_DWMAC_GLUE)

#define AX650X_DWMAC_GLB_REG_SIZE     0xa0
#define AX650X_DWMAC_GLB_NR_REGS      \
    (AX650X_DWMAC_GLB_REG_SIZE / sizeof(uint32_t))
#define AX650X_DWMAC_CLK_REG_SIZE     0x40
#define AX650X_DWMAC_CLK_NR_REGS      \
    (AX650X_DWMAC_CLK_REG_SIZE / sizeof(uint32_t))
#define AX650X_DWMAC_GLUE_MMIO_SIZE   0x1000

typedef struct AX650XDWMACGlueState {
    SysBusDevice parent;

    uint32_t glb_regs[AX650X_DWMAC_GLB_NR_REGS];
    RegisterInfo glb_regs_info[AX650X_DWMAC_GLB_NR_REGS];
    RegisterInfoArray *glb_reg_array;
    uint32_t clk_regs[AX650X_DWMAC_CLK_NR_REGS];
    RegisterInfo clk_regs_info[AX650X_DWMAC_CLK_NR_REGS];
    RegisterInfoArray *clk_reg_array;
    uint8_t port;
} AX650XDWMACGlueState;

#endif /* HW_MISC_AX650X_DWMAC_GLUE_H */

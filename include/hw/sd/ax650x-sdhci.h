/*
 * Axera AX650X SDHCI controller
 *
 * Copyright (c) 2026 Zevorn
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_AX650X_SDHCI_H
#define HW_SD_AX650X_SDHCI_H

#include "hw/core/register.h"
#include "hw/sd/sdhci.h"

#define TYPE_AX650X_SDHCI "ax650x-sdhci"
OBJECT_DECLARE_SIMPLE_TYPE(AX650XSDHCIState, AX650X_SDHCI)

#define AX650X_SDHCI_REG_SIZE              0x600
#define AX650X_SDHCI_VENDOR_BASE           0x100
#define AX650X_SDHCI_VENDOR_SIZE           0x500
#define AX650X_SDHCI_VENDOR_PTR            0x0e8
#define AX650X_SDHCI_VENDOR_PTR_VALUE      0x500

#define AX650X_SDHCI_PHY_CNFG              0x300
#define AX650X_SDHCI_PHY_CMDPAD_CNFG       0x304
#define AX650X_SDHCI_PHY_DATAPAD_CNFG      0x306
#define AX650X_SDHCI_PHY_CLKPAD_CNFG       0x308
#define AX650X_SDHCI_PHY_STBPAD_CNFG       0x30a
#define AX650X_SDHCI_PHY_RSTNPAD_CNFG      0x30c
#define AX650X_SDHCI_PHY_COMMDL_CNFG       0x31c
#define AX650X_SDHCI_PHY_SDCLKDL_CNFG      0x31d
#define AX650X_SDHCI_PHY_SDCLKDL_DC        0x31e
#define AX650X_SDHCI_PHY_SMPLDL_CNFG       0x320
#define AX650X_SDHCI_PHY_ATDL_CNFG         0x321
#define AX650X_SDHCI_PHY_DLL_CTRL          0x324
#define AX650X_SDHCI_PHY_DLL_CNFG1         0x325
#define AX650X_SDHCI_PHY_DLL_CNFG2         0x326
#define AX650X_SDHCI_PHY_DLLDL_CNFG        0x328
#define AX650X_SDHCI_PHY_DLL_OFFSET        0x329
#define AX650X_SDHCI_PHY_DLLLBT_CNFG       0x32c
#define AX650X_SDHCI_PHY_DLL_STATUS        0x32e
#define AX650X_SDHCI_PHY_DLLDBG_MLKDC      0x330
#define AX650X_SDHCI_PHY_DLLDBG_SLKDC      0x332
#define AX650X_SDHCI_EMMC_CTRL             0x52c

#define AX650X_SDHCI_PHY_PWRGOOD           BIT(1)
#define AX650X_SDHCI_PHY_RSTN              BIT(0)
#define AX650X_SDHCI_DLL_EN                BIT(0)
#define AX650X_SDHCI_DLL_ERROR             BIT(1)
#define AX650X_SDHCI_DLL_LOCKED            BIT(0)
#define AX650X_SDHCI_CARD_IS_EMMC          BIT(0)
#define AX650X_SDHCI_EMMC_RST_N            BIT(2)
#define AX650X_SDHCI_EMMC_RST_N_OE         BIT(3)
#define AX650X_SDHCI_ENH_STROBE_EN         BIT(8)

struct AX650XSDHCIState {
    SysBusDevice parent_obj;

    MemoryRegion container;
    SDHCIState sdhci;

    RegisterInfo pointer_regs_info[2];
    uint8_t pointer_regs[2];
    RegisterInfoArray *pointer_reg_array;

    RegisterInfo vendor_regs_info[AX650X_SDHCI_VENDOR_SIZE];
    uint8_t vendor_regs[AX650X_SDHCI_VENDOR_SIZE];
    RegisterInfoArray *vendor_reg_array;

    bool emmc_reset_asserted;
};

SDBus *ax650x_sdhci_get_bus(AX650XSDHCIState *s);

#endif

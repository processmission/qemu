/*
 * Rockchip Crypto V2
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_CRYPTO_V2_H
#define HW_MISC_ROCKCHIP_CRYPTO_V2_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_CRYPTO_V2 "rockchip-crypto-v2"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipCryptoV2State, ROCKCHIP_CRYPTO_V2)

#define ROCKCHIP_CRYPTO_V2_MMIO_SIZE 0x4000

#endif

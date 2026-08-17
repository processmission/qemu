/*
 * Rockchip RK806 PMIC model (SPI interface)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Implements the SPI frame protocol of the rk806 driver
 * (drivers/mfd/rk806-spi.c): a write frame is a single 32-bit transfer
 * of [cmd | len-1][addr][RK806_REG_H][data], and a read frame is a
 * 3-byte header followed by the register value.  The register file
 * defaults to all regulators enabled (POWER_EN0-5 = 0xff) with the
 * BUCK8 and PLDO5 voltages at 3.3 V so the SD/eMMC regulator supplies
 * resolve when Linux probes the MMC controllers.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/ssi/rk806.h"
#include "migration/vmstate.h"

#define RK806_CMD_READ  0x00
#define RK806_CMD_WRITE 0x80
#define RK806_REG_H     0x00

/* Register addresses (include/linux/mfd/rk806.h). */
#define RK806_POWER_EN0  0x00
#define RK806_POWER_EN5  0x05
#define RK806_BUCK8_ON_VSEL 0x21
#define RK806_PLDO5_ON_VSEL 0x52
#define RK806_CHIP_NAME  0x5a
#define RK806_CHIP_VER   0x5b
#define RK806_OTP_VER    0x5c

/*
 * VSEL values for 3.3 V: BUCK8 uses 1500 mV + (vsel-160)*25 mV,
 * PLDO uses 500 mV + vsel*12.5 mV (rk806-regulator.c ranges).
 */
#define RK806_BUCK8_VSEL_3V3 232
#define RK806_PLDO5_VSEL_3V3 224

static uint32_t rk806_transfer(SSIPeripheral *ss, uint32_t tx)
{
    RK806State *s = RK806(ss);
    uint8_t byte = tx & 0xff;

    switch (s->frame_state) {
    case RK806_FRAME_CMD:
        s->frame_cmd = byte;
        s->frame_state = RK806_FRAME_ADDR;
        return 0;
    case RK806_FRAME_ADDR:
        s->frame_addr = byte;
        s->frame_state = RK806_FRAME_REG_H;
        return 0;
    case RK806_FRAME_REG_H:
        s->frame_remain = (s->frame_cmd & 0x7) + 1;
        if (s->frame_cmd & RK806_CMD_WRITE) {
            s->frame_state = RK806_FRAME_DATA;
        } else {
            s->frame_state = RK806_FRAME_READ;
        }
        return 0;
    case RK806_FRAME_DATA:
        if (s->frame_remain) {
            s->regs[s->frame_addr] = byte;
            s->frame_remain--;
            if (!s->frame_remain) {
                s->frame_state = RK806_FRAME_CMD;
            }
        }
        return 0;
    case RK806_FRAME_READ:
        if (s->frame_remain) {
            uint8_t val = s->regs[s->frame_addr];

            s->frame_remain--;
            if (!s->frame_remain) {
                s->frame_state = RK806_FRAME_CMD;
            }
            return val;
        }
        return 0;
    default:
        s->frame_state = RK806_FRAME_CMD;
        return 0;
    }
}

static void rk806_reset(DeviceState *dev)
{
    RK806State *s = RK806(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    /* All regulators powered on. */
    for (i = RK806_POWER_EN0; i <= RK806_POWER_EN5; i++) {
        s->regs[i] = 0xff;
    }
    s->regs[RK806_BUCK8_ON_VSEL] = RK806_BUCK8_VSEL_3V3;
    s->regs[RK806_PLDO5_ON_VSEL] = RK806_PLDO5_VSEL_3V3;
    s->regs[RK806_CHIP_NAME] = 0x06;
    s->regs[RK806_CHIP_VER] = 0x00;
    s->regs[RK806_OTP_VER] = 0x00;
    s->frame_state = RK806_FRAME_CMD;
    s->frame_cmd = 0;
    s->frame_addr = 0;
    s->frame_remain = 0;
}

static const VMStateDescription vmstate_rk806 = {
    .name = "rk806",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RK806State, RK806_REG_COUNT),
        VMSTATE_UINT32(frame_state, RK806State),
        VMSTATE_UINT8(frame_cmd, RK806State),
        VMSTATE_UINT8(frame_addr, RK806State),
        VMSTATE_UINT32(frame_remain, RK806State),
        VMSTATE_END_OF_LIST()
    },
};

static void rk806_realize(SSIPeripheral *ss, Error **errp)
{
    /* The register file is initialized by the reset handler. */
}

static void rk806_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->transfer = rk806_transfer;
    k->realize = rk806_realize;
    device_class_set_legacy_reset(dc, rk806_reset);
    dc->vmsd = &vmstate_rk806;
    dc->user_creatable = false;
}

static const TypeInfo rk806_info = {
    .name = TYPE_RK806,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(RK806State),
    .class_init = rk806_class_init,
};

static void rk806_register_types(void)
{
    type_register_static(&rk806_info);
}

type_init(rk806_register_types)

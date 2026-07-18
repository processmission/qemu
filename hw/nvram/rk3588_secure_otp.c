/*
 * Rockchip RK3588 secure OTP
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/nvram/rk3588_secure_otp.h"

#include "hw/core/register.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

REG32(DOUT, 0x20)
REG32(INT_STATUS, 0x84)
    FIELD(INT_STATUS, READ_DONE, 1, 1)

#define RK3588_SECURE_OTP_REG_WORDS (R_INT_STATUS + 1)

struct RK3588SecureOTPState {
    SysBusDevice parent_obj;

    uint32_t regs[RK3588_SECURE_OTP_REG_WORDS];
    RegisterInfo regs_info[RK3588_SECURE_OTP_REG_WORDS];
    RegisterInfoArray *reg_array;
};

static const RegisterAccessInfo rk3588_secure_otp_regs_info[] = {
    { .name = "DOUT", .addr = A_DOUT, .ro = UINT32_MAX },
    { .name = "INT_STATUS", .addr = A_INT_STATUS, .ro = UINT32_MAX,
      .reset = R_INT_STATUS_READ_DONE_MASK },
};

static bool rk3588_secure_otp_is_register(hwaddr addr)
{
    return addr == A_DOUT || addr == A_INT_STATUS;
}

static uint64_t rk3588_secure_otp_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    if (!rk3588_secure_otp_is_register(addr)) {
        return 0;
    }

    return register_read_memory(opaque, addr, size);
}

static void rk3588_secure_otp_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    if (!rk3588_secure_otp_is_register(addr)) {
        return;
    }

    register_write_memory(opaque, addr, value, size);
}

static const MemoryRegionOps rk3588_secure_otp_ops = {
    .read = rk3588_secure_otp_read,
    .write = rk3588_secure_otp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void rk3588_secure_otp_reset_hold(Object *obj, ResetType type)
{
    RK3588SecureOTPState *s = RK3588_SECURE_OTP(obj);

    memset(s->regs, 0, sizeof(s->regs));
    for (unsigned int i = 0;
         i < ARRAY_SIZE(rk3588_secure_otp_regs_info); i++) {
        register_reset(&s->regs_info[
                       rk3588_secure_otp_regs_info[i].addr / 4]);
    }
}

static const VMStateDescription vmstate_rk3588_secure_otp = {
    .name = TYPE_RK3588_SECURE_OTP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3588SecureOTPState,
                             RK3588_SECURE_OTP_REG_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_secure_otp_init(Object *obj)
{
    RK3588SecureOTPState *s = RK3588_SECURE_OTP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(
        DEVICE(obj), rk3588_secure_otp_regs_info,
        ARRAY_SIZE(rk3588_secure_otp_regs_info), s->regs_info, s->regs,
        &rk3588_secure_otp_ops, false, RK3588_SECURE_OTP_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
}

static void rk3588_secure_otp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_rk3588_secure_otp;
    rc->phases.hold = rk3588_secure_otp_reset_hold;
}

static const TypeInfo rk3588_secure_otp_info = {
    .name = TYPE_RK3588_SECURE_OTP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588SecureOTPState),
    .instance_init = rk3588_secure_otp_init,
    .class_init = rk3588_secure_otp_class_init,
};

static void rk3588_secure_otp_register_types(void)
{
    type_register_static(&rk3588_secure_otp_info);
}

type_init(rk3588_secure_otp_register_types)

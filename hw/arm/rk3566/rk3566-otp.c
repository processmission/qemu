/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"

#define TYPE_RK3566_OTP "rk3566-otp"
#define OTP_REG_SIZE 0x4000

OBJECT_DECLARE_SIMPLE_TYPE(RK3566OTPState, RK3566_OTP)

enum {
    OTP_REG_CMD,
    OTP_REG_FUSE_DATA,
    OTP_REG_STATUS,
    OTP_REG_MAX,
};

struct RK3566OTPState {
    SysBusDevice parent_obj;
    uint32_t regs[OTP_REG_SIZE / 4];
    uint8_t fuses[1024];
    RegisterInfo regs_info[OTP_REG_MAX];
    RegisterInfoArray *reg_array;
};

static uint64_t otp_hiword_pre_write(RegisterInfo *reg, uint64_t val)
{
    uint32_t *data = reg->data;
    uint32_t mask = val >> 16;

    return (*data & ~mask) | (val & mask);
}

static void otp_cmd_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566OTPState *s = RK3566_OTP(reg->opaque);

    if (!(val & 1)) {
        return;
    }
    if (s->regs[0x1000 / 4] == 0xfa) {
        s->regs[0x304 / 4] |= 2;
    }
    s->regs[0x20 / 4] &= ~1;
}

static void otp_fuse_data_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566OTPState *s = RK3566_OTP(reg->opaque);
    unsigned address;

    if (!(val & 1)) {
        return;
    }
    address = s->regs[0x104 / 4] * 2;
    if ((s->regs[0x100 / 4] & 1) && address + 2 <= sizeof(s->fuses)) {
        s->regs[0x124 / 4] = lduw_le_p(s->fuses + address);
        s->regs[0x304 / 4] |= 4;
    }
    s->regs[0x108 / 4] = 0;
}

static const RegisterAccessInfo otp_regs_info[] = {
    { .name = "CMD", .addr = 0x20,
      .pre_write = otp_hiword_pre_write,
      .post_write = otp_cmd_post_write },
    { .name = "FUSE_DATA", .addr = 0x108,
      .pre_write = otp_hiword_pre_write,
      .post_write = otp_fuse_data_post_write },
    { .name = "STATUS", .addr = 0x304, .w1c = UINT32_MAX },
};

static RegisterInfo *otp_find_register(RegisterInfoArray *reg_array,
                                       hwaddr offset)
{
    for (unsigned i = 0; i < reg_array->num_elements; i++) {
        RegisterInfo *reg = reg_array->r[i];

        if (reg->access->addr == offset) {
            return reg;
        }
    }
    return NULL;
}

static uint64_t otp_read(void *opaque, hwaddr offset, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566OTPState *s = RK3566_OTP(register_array_get_owner(reg_array));

    if (otp_find_register(reg_array, offset)) {
        return register_read_memory(reg_array, offset, size);
    }
    return s->regs[offset / 4];
}

static void otp_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566OTPState *s = RK3566_OTP(register_array_get_owner(reg_array));
    uint32_t *reg = &s->regs[offset / 4];
    uint32_t mask;

    if (otp_find_register(reg_array, offset)) {
        register_write_memory(reg_array, offset, value, size);
        return;
    }
    if (offset < 0x120) {
        mask = value >> 16;
        *reg = (*reg & ~mask) | (value & mask);
    } else {
        *reg = value;
    }
}

static const MemoryRegionOps otp_ops = {
    .read = otp_read, .write = otp_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void otp_init(Object *obj)
{
    RK3566OTPState *s = RK3566_OTP(obj);

    /* CPU code identifies the modeled silicon; other fuses are unprogrammed. */
    s->fuses[2] = 0x35;
    s->fuses[3] = 0x66;
    s->reg_array = register_init_block32(DEVICE(obj), otp_regs_info,
                                         ARRAY_SIZE(otp_regs_info),
                                         s->regs_info, s->regs,
                                         &otp_ops, false, OTP_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->reg_array->mem);
}

static const TypeInfo otp_info = {
    .name = TYPE_RK3566_OTP, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566OTPState), .instance_init = otp_init,
};

static void otp_register(void) { type_register_static(&otp_info); }
type_init(otp_register)

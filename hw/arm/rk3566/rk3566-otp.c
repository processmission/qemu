/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "qemu/bswap.h"

#define TYPE_RK3566_OTP "rk3566-otp"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566OTPState, RK3566_OTP)
struct RK3566OTPState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    uint32_t regs[0x4000 / 4];
    uint8_t fuses[1024];
};

static uint64_t otp_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566OTPState *s = opaque;
    return s->regs[offset / 4];
}

static void otp_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RK3566OTPState *s = opaque;
    uint32_t *reg = &s->regs[offset / 4];
    if (offset == 0x304) {
        *reg &= ~value;
        return;
    }
    if (offset < 0x120) {
        uint32_t mask = value >> 16;
        *reg = (*reg & ~mask) | (value & mask);
    } else {
        *reg = value;
    }
    if (offset == 0x20 && (*reg & 1)) {
        if (s->regs[0x1000 / 4] == 0xfa) {
            s->regs[0x304 / 4] |= 2;
        }
        *reg &= ~1;
    }
    if (offset == 0x108 && (*reg & 1)) {
        unsigned address = s->regs[0x104 / 4] * 2;
        if ((s->regs[0x100 / 4] & 1) && address + 2 <= sizeof(s->fuses)) {
            s->regs[0x124 / 4] = lduw_le_p(s->fuses + address);
            s->regs[0x304 / 4] |= 4;
        }
        *reg = 0;
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
    memory_region_init_io(&s->io, obj, &otp_ops, s, TYPE_RK3566_OTP, 0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io);
}
static const TypeInfo otp_info = {
    .name = TYPE_RK3566_OTP, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566OTPState), .instance_init = otp_init,
};
static void otp_register(void) { type_register_static(&otp_info); }
type_init(otp_register)

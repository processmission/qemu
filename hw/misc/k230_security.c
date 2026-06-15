/*
 * K230 security register block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_security.h"

#define K230_TRNG_BASE          0x3000
#define K230_TRNG_DATA          0x02a0
#define K230_OTP_BASE           0x3500

static uint64_t k230_security_read_bytes(uint8_t *regs, hwaddr addr,
                                         unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_security_write_bytes(uint8_t *regs, hwaddr addr,
                                      uint64_t val, unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static bool k230_security_range_ok(hwaddr addr, unsigned int size)
{
    return addr <= K230_SECURITY_SIZE && size <= K230_SECURITY_SIZE - addr;
}

static bool k230_otp_range(hwaddr addr, unsigned int size)
{
    hwaddr end = addr + size;

    return addr >= K230_OTP_BASE && end <= K230_OTP_BASE + K230_OTP_SIZE;
}

static uint64_t k230_security_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    K230SecurityState *s = K230_SECURITY(opaque);

    if (!k230_security_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_SECURITY, addr);
        return 0;
    }

    if (addr == K230_TRNG_BASE + K230_TRNG_DATA && size == 4) {
        uint32_t value;

        qemu_guest_getrandom_nofail(&value, sizeof(value));
        return value;
    }

    if (k230_otp_range(addr, size)) {
        return k230_security_read_bytes(s->otp, addr - K230_OTP_BASE, size);
    }

    return k230_security_read_bytes(s->regs, addr, size);
}

static void k230_security_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned int size)
{
    K230SecurityState *s = K230_SECURITY(opaque);

    if (!k230_security_range_ok(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      TYPE_K230_SECURITY, addr);
        return;
    }

    if (k230_otp_range(addr, size)) {
        return;
    }

    k230_security_write_bytes(s->regs, addr, val, size);
}

static const MemoryRegionOps k230_security_ops = {
    .read = k230_security_read,
    .write = k230_security_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_security_reset(DeviceState *dev)
{
    K230SecurityState *s = K230_SECURITY(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->otp, 0, sizeof(s->otp));
}

static const VMStateDescription vmstate_k230_security = {
    .name = TYPE_K230_SECURITY,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230SecurityState, K230_SECURITY_SIZE),
        VMSTATE_UINT8_ARRAY(otp, K230SecurityState, K230_OTP_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_security_realize(DeviceState *dev, Error **errp)
{
    K230SecurityState *s = K230_SECURITY(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_security_ops, s,
                          TYPE_K230_SECURITY, K230_SECURITY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_security_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_security_realize;
    device_class_set_legacy_reset(dc, k230_security_reset);
    dc->vmsd = &vmstate_k230_security;
    dc->desc = "K230 security registers";
}

static const TypeInfo k230_security_type_info = {
    .name = TYPE_K230_SECURITY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SecurityState),
    .class_init = k230_security_class_init,
};

static void k230_security_register_types(void)
{
    type_register_static(&k230_security_type_info);
}

type_init(k230_security_register_types)

/*
 * RK3588 TRNGv1 model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register-level model for the rockchip-trngv1 driver
 * (drivers/char/hw_random/rockchip-rng.c).  The Linux RNG core drains
 * entropy from this device early in boot (systemd's machine-ID
 * generation blocks until the CRNG is seeded), so the model generates
 * real random data through QEMU's crypto layer.
 *
 * Registers (TRNG_V1_*):
 *   CTRL 0x00  STAT 0x04  MODE 0x08  IE 0x10  ISTAT 0x14
 *   RAND0..RAND7 0x20..0x3c  AUTO_RQSTS 0x60  VERSION 0xf0
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "crypto/random.h"
#include "hw/core/irq.h"
#include "hw/misc/rk3588_rng.h"
#include "migration/vmstate.h"

#define TRNG_V1_CTRL      0x00
#define TRNG_V1_CTRL_NOP  0x00
#define TRNG_V1_CTRL_RAND 0x01
#define TRNG_V1_STAT      0x04
#define TRNG_V1_STAT_SEEDED BIT(9)
#define TRNG_V1_MODE      0x08
#define TRNG_V1_ISTAT     0x14
#define TRNG_V1_ISTAT_RAND_RDY BIT(0)
#define TRNG_V1_RAND0     0x20
#define TRNG_V1_AUTO_RQSTS 0x60
#define TRNG_V1_VERSION   0xf0
#define TRNG_v1_VERSION_CODE 0x46bc

static uint64_t rk3588_rng_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3588RNGState *s = opaque;

    switch (offset) {
    case TRNG_V1_VERSION:
        return TRNG_v1_VERSION_CODE;
    case TRNG_V1_STAT:
        return TRNG_V1_STAT_SEEDED;
    case TRNG_V1_ISTAT:
        return s->rand_ready ? TRNG_V1_ISTAT_RAND_RDY : 0;
    default:
        if (offset >= TRNG_V1_RAND0 && offset < TRNG_V1_RAND0 + 32) {
            return s->rand_words[(offset - TRNG_V1_RAND0) / 4];
        }
        return s->regs[offset >> 2];
    }
}

static void rk3588_rng_generate(RK3588RNGState *s)
{
    Error *err = NULL;

    if (qcrypto_random_bytes(s->rand_words, sizeof(s->rand_words), &err)) {
        /* Fall back to deterministic values on RNG failure. */
        error_free(err);
        memset(s->rand_words, 0x5a, sizeof(s->rand_words));
    }
    s->rand_ready = true;
}

static void rk3588_rng_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RK3588RNGState *s = opaque;

    switch (offset) {
    case TRNG_V1_CTRL:
        if ((value & 0xff) == TRNG_V1_CTRL_RAND) {
            rk3588_rng_generate(s);
            qemu_set_irq(s->irq, 1);
        } else {
            s->rand_ready = false;
            qemu_set_irq(s->irq, 0);
        }
        break;
    case TRNG_V1_ISTAT:
        /* Write-1-to-clear. */
        if (value & TRNG_V1_ISTAT_RAND_RDY) {
            s->rand_ready = false;
            qemu_set_irq(s->irq, 0);
        }
        break;
    case TRNG_V1_MODE:
    case TRNG_V1_AUTO_RQSTS:
    case TRNG_V1_STAT:
        break;
    default:
        if (offset < RK3588_RNG_MMIO_SIZE - 4) {
            s->regs[offset >> 2] = value;
        }
        break;
    }
}

static const MemoryRegionOps rk3588_rng_ops = {
    .read = rk3588_rng_read,
    .write = rk3588_rng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rk3588_rng_realize(DeviceState *dev, Error **errp)
{
    RK3588RNGState *s = RK3588_RNG(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_init_irq(sbd, &s->irq);
    memory_region_init_io(&s->iomem, OBJECT(dev), &rk3588_rng_ops, s,
                          "rk3588-rng", RK3588_RNG_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rk3588_rng_reset(DeviceState *dev)
{
    RK3588RNGState *s = RK3588_RNG(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->rand_words, 0, sizeof(s->rand_words));
    s->rand_ready = false;
}

static const VMStateDescription vmstate_rk3588_rng = {
    .name = "rk3588-rng",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3588RNGState, RK3588_RNG_MMIO_SIZE / 4),
        VMSTATE_UINT32_ARRAY(rand_words, RK3588RNGState, 8),
        VMSTATE_BOOL(rand_ready, RK3588RNGState),
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_rng_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rk3588_rng_realize;
    device_class_set_legacy_reset(dc, rk3588_rng_reset);
    dc->vmsd = &vmstate_rk3588_rng;
    dc->user_creatable = false;
}

static const TypeInfo rk3588_rng_info = {
    .name = TYPE_RK3588_RNG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588RNGState),
    .class_init = rk3588_rng_class_init,
};

static void rk3588_rng_register_types(void)
{
    type_register_static(&rk3588_rng_info);
}

type_init(rk3588_rng_register_types)

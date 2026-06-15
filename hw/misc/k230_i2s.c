/*
 * K230 I2S / WS2812 registers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/misc/k230_i2s.h"

REG32(IER, 0x000)
    FIELD(IER, ENABLE, 0, 1)
REG32(IRER, 0x004)
    FIELD(IRER, ENABLE, 0, 1)
REG32(ITER, 0x008)
    FIELD(ITER, ENABLE, 0, 1)
REG32(CER, 0x00c)
    FIELD(CER, ENABLE, 0, 1)
REG32(CCR, 0x010)
    FIELD(CCR, SCLK_GATING, 0, 3)
    FIELD(CCR, WORD_SELECT, 3, 2)
    FIELD(CCR, MODE, 5, 3)
    FIELD(CCR, TX_DMA_ENABLE, 8, 1)
REG32(RXFFR, 0x014)
REG32(TXFFR, 0x018)

#define K230_I2S_CHANNEL_COUNT 4
#define K230_I2S_CHANNEL_STRIDE 0x40
#define K230_I2S_CH_BASE(n) (0x020 + (n) * K230_I2S_CHANNEL_STRIDE)
#define K230_I2S_CH_LRXT(n) (K230_I2S_CH_BASE(n) + 0x00)
#define K230_I2S_CH_RRXT(n) (K230_I2S_CH_BASE(n) + 0x04)
#define K230_I2S_CH_RER(n)  (K230_I2S_CH_BASE(n) + 0x08)
#define K230_I2S_CH_TER(n)  (K230_I2S_CH_BASE(n) + 0x0c)
#define K230_I2S_CH_RCR(n)  (K230_I2S_CH_BASE(n) + 0x10)
#define K230_I2S_CH_TCR(n)  (K230_I2S_CH_BASE(n) + 0x14)
#define K230_I2S_CH_ISR(n)  (K230_I2S_CH_BASE(n) + 0x18)
#define K230_I2S_CH_IMR(n)  (K230_I2S_CH_BASE(n) + 0x1c)
#define K230_I2S_CH_ROR(n)  (K230_I2S_CH_BASE(n) + 0x20)
#define K230_I2S_CH_TOR(n)  (K230_I2S_CH_BASE(n) + 0x24)
#define K230_I2S_CH_RFCR(n) (K230_I2S_CH_BASE(n) + 0x28)
#define K230_I2S_CH_TFCR(n) (K230_I2S_CH_BASE(n) + 0x2c)
#define K230_I2S_CH_RFF(n)  (K230_I2S_CH_BASE(n) + 0x30)
#define K230_I2S_CH_TFF(n)  (K230_I2S_CH_BASE(n) + 0x34)

REG32(RXDMA, 0x1c0)
REG32(RRXDMA, 0x1c4)
REG32(TXDMA, 0x1c8)
REG32(RTXDMA, 0x1cc)
REG32(COMP_PARAM_2, 0x1f0)
REG32(COMP_PARAM_1, 0x1f4)
REG32(COMP_VERSION, 0x1f8)
REG32(COMP_TYPE, 0x1fc)
REG32(AUDIO_IN_CTRL, 0x400)
    FIELD(AUDIO_IN_CTRL, ENABLE, 5, 1)
REG32(AUDIO_OUT_CTRL, 0xc00)
    FIELD(AUDIO_OUT_CTRL, ENABLE, 0, 1)
    FIELD(AUDIO_OUT_CTRL, WIDTH, 1, 2)
    FIELD(AUDIO_OUT_CTRL, MODE, 5, 2)

static void k230_i2s_ws2812_clear(K230I2SState *s)
{
    s->ws2812_byte_count = 0;
    memset(s->ws2812_byte, 0, sizeof(s->ws2812_byte));
    s->ws2812_padding_count = 0;
    s->ws2812_invalid_count = 0;
}

static bool k230_i2s_ws2812_symbol_decode(uint8_t symbol, uint8_t *bits)
{
    switch (symbol) {
    case 0x88:
        *bits = 0;
        return true;
    case 0x8e:
        *bits = 1;
        return true;
    case 0xe8:
        *bits = 2;
        return true;
    case 0xee:
        *bits = 3;
        return true;
    default:
        return false;
    }
}

static bool k230_i2s_ws2812_word_decode(uint32_t word, uint8_t *byte)
{
    uint8_t value = 0;

    for (int i = 0; i < 4; i++) {
        uint8_t bits;
        uint8_t symbol = extract32(word, i * 8, 8);

        if (!k230_i2s_ws2812_symbol_decode(symbol, &bits)) {
            return false;
        }
        value |= bits << (i * 2);
    }

    *byte = value;
    return true;
}

static void k230_i2s_ws2812_append(K230I2SState *s, uint8_t byte)
{
    if (s->ws2812_byte_count < K230_I2S_WS2812_SAMPLES) {
        s->ws2812_byte[s->ws2812_byte_count] = byte;
    }
    s->ws2812_byte_count++;
}

static bool k230_i2s_tx_dma_enabled(const K230I2SState *s)
{
    return FIELD_EX32(s->regs[R_CCR], CCR, TX_DMA_ENABLE) != 0;
}

static uint64_t k230_i2s_txdma_pre_write(RegisterInfo *reg, uint64_t val)
{
    K230I2SState *s = K230_I2S(reg->opaque);
    uint8_t byte;

    if (!k230_i2s_tx_dma_enabled(s)) {
        return s->regs[R_TXDMA];
    }

    if (val == 0) {
        s->ws2812_padding_count++;
        return s->regs[R_TXDMA];
    }
    if (k230_i2s_ws2812_word_decode(val, &byte)) {
        k230_i2s_ws2812_append(s, byte);
        return s->regs[R_TXDMA];
    }

    s->ws2812_invalid_count++;
    return s->regs[R_TXDMA];
}

static void k230_i2s_flush_post_write(RegisterInfo *reg, uint64_t val)
{
    K230I2SState *s = K230_I2S(reg->opaque);

    if (val) {
        k230_i2s_ws2812_clear(s);
    }
}

#define K230_I2S_CH_REG(n, reg) \
    { .name = "CH" #n "_" #reg, .addr = K230_I2S_CH_ ## reg(n) }
#define K230_I2S_CH_TFF_REG(n) \
    { .name = "CH" #n "_TFF", .addr = K230_I2S_CH_TFF(n), \
      .post_write = k230_i2s_flush_post_write }

static const RegisterAccessInfo k230_i2s_regs_info[] = {
    { .name = "IER", .addr = A_IER, .rsvd = ~R_IER_ENABLE_MASK },
    { .name = "IRER", .addr = A_IRER, .rsvd = ~R_IRER_ENABLE_MASK },
    { .name = "ITER", .addr = A_ITER, .rsvd = ~R_ITER_ENABLE_MASK },
    { .name = "CER", .addr = A_CER, .rsvd = ~R_CER_ENABLE_MASK },
    { .name = "CCR", .addr = A_CCR },
    { .name = "RXFFR", .addr = A_RXFFR,
      .post_write = k230_i2s_flush_post_write },
    { .name = "TXFFR", .addr = A_TXFFR,
      .post_write = k230_i2s_flush_post_write },
    K230_I2S_CH_REG(0, LRXT),
    K230_I2S_CH_REG(0, RRXT),
    K230_I2S_CH_REG(0, RER),
    K230_I2S_CH_REG(0, TER),
    K230_I2S_CH_REG(0, RCR),
    K230_I2S_CH_REG(0, TCR),
    K230_I2S_CH_REG(0, ISR),
    K230_I2S_CH_REG(0, IMR),
    K230_I2S_CH_REG(0, ROR),
    K230_I2S_CH_REG(0, TOR),
    K230_I2S_CH_REG(0, RFCR),
    K230_I2S_CH_REG(0, TFCR),
    K230_I2S_CH_REG(0, RFF),
    K230_I2S_CH_TFF_REG(0),
    K230_I2S_CH_REG(1, LRXT),
    K230_I2S_CH_REG(1, RRXT),
    K230_I2S_CH_REG(1, RER),
    K230_I2S_CH_REG(1, TER),
    K230_I2S_CH_REG(1, RCR),
    K230_I2S_CH_REG(1, TCR),
    K230_I2S_CH_REG(1, ISR),
    K230_I2S_CH_REG(1, IMR),
    K230_I2S_CH_REG(1, ROR),
    K230_I2S_CH_REG(1, TOR),
    K230_I2S_CH_REG(1, RFCR),
    K230_I2S_CH_REG(1, TFCR),
    K230_I2S_CH_REG(1, RFF),
    K230_I2S_CH_TFF_REG(1),
    K230_I2S_CH_REG(2, LRXT),
    K230_I2S_CH_REG(2, RRXT),
    K230_I2S_CH_REG(2, RER),
    K230_I2S_CH_REG(2, TER),
    K230_I2S_CH_REG(2, RCR),
    K230_I2S_CH_REG(2, TCR),
    K230_I2S_CH_REG(2, ISR),
    K230_I2S_CH_REG(2, IMR),
    K230_I2S_CH_REG(2, ROR),
    K230_I2S_CH_REG(2, TOR),
    K230_I2S_CH_REG(2, RFCR),
    K230_I2S_CH_REG(2, TFCR),
    K230_I2S_CH_REG(2, RFF),
    K230_I2S_CH_TFF_REG(2),
    K230_I2S_CH_REG(3, LRXT),
    K230_I2S_CH_REG(3, RRXT),
    K230_I2S_CH_REG(3, RER),
    K230_I2S_CH_REG(3, TER),
    K230_I2S_CH_REG(3, RCR),
    K230_I2S_CH_REG(3, TCR),
    K230_I2S_CH_REG(3, ISR),
    K230_I2S_CH_REG(3, IMR),
    K230_I2S_CH_REG(3, ROR),
    K230_I2S_CH_REG(3, TOR),
    K230_I2S_CH_REG(3, RFCR),
    K230_I2S_CH_REG(3, TFCR),
    K230_I2S_CH_REG(3, RFF),
    K230_I2S_CH_TFF_REG(3),
    { .name = "RXDMA", .addr = A_RXDMA },
    { .name = "RRXDMA", .addr = A_RRXDMA },
    { .name = "TXDMA", .addr = A_TXDMA,
      .pre_write = k230_i2s_txdma_pre_write },
    { .name = "RTXDMA", .addr = A_RTXDMA,
      .post_write = k230_i2s_flush_post_write },
    { .name = "COMP_PARAM_2", .addr = A_COMP_PARAM_2, .ro = ~0ull },
    { .name = "COMP_PARAM_1", .addr = A_COMP_PARAM_1, .ro = ~0ull },
    { .name = "COMP_VERSION", .addr = A_COMP_VERSION, .ro = ~0ull },
    { .name = "COMP_TYPE", .addr = A_COMP_TYPE, .ro = ~0ull },
    { .name = "AUDIO_IN_CTRL", .addr = A_AUDIO_IN_CTRL },
    { .name = "AUDIO_OUT_CTRL", .addr = A_AUDIO_OUT_CTRL },
};

static void k230_i2s_sync_compat_reg(K230I2SState *s, hwaddr addr)
{
    if (!s->compat_regs) {
        return;
    }

    stl_le_p(&s->compat_regs->regs[addr], s->regs[addr / sizeof(uint32_t)]);
}

static bool k230_i2s_is_modeled_addr(hwaddr addr)
{
    for (int i = 0; i < ARRAY_SIZE(k230_i2s_regs_info); i++) {
        if (k230_i2s_regs_info[i].addr == addr) {
            return true;
        }
    }

    return false;
}

static uint64_t k230_i2s_read_bytes(const uint8_t *regs, hwaddr addr,
                                    unsigned int size)
{
    uint64_t val = 0;

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_i2s_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                 unsigned int size)
{
    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static void k230_i2s_import_compat(K230I2SState *s)
{
    if (s->state_loaded || s->compat_imported || !s->compat_regs) {
        return;
    }

    for (unsigned int i = 0; i < K230_I2S_R_MAX; i++) {
        s->regs[i] = ldl_le_p(&s->compat_regs->regs[i * sizeof(uint32_t)]);
    }
    s->compat_imported = true;
}

static uint64_t k230_i2s_mmio_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    K230I2SState *s = K230_I2S(opaque);

    k230_i2s_import_compat(s);
    if (size == 4 && k230_i2s_is_modeled_addr(addr)) {
        return register_read_memory(s->reg_array, addr, size);
    }

    return s->compat_regs ? k230_i2s_read_bytes(s->compat_regs->regs, addr,
                                                size) : 0;
}

static void k230_i2s_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned int size)
{
    K230I2SState *s = K230_I2S(opaque);

    k230_i2s_import_compat(s);
    if (size == 4 && k230_i2s_is_modeled_addr(addr)) {
        register_write_memory(s->reg_array, addr, val, size);
        k230_i2s_sync_compat_reg(s, addr);
        return;
    }
    if (s->compat_regs) {
        k230_i2s_write_bytes(s->compat_regs->regs, addr, val, size);
    }
}

static const MemoryRegionOps k230_i2s_reg_array_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps k230_i2s_ops = {
    .read = k230_i2s_mmio_read,
    .write = k230_i2s_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void k230_i2s_reset(DeviceState *dev)
{
    K230I2SState *s = K230_I2S(dev);

    k230_i2s_ws2812_clear(s);
    s->state_loaded = false;
    s->compat_imported = false;
    for (int i = 0; i < ARRAY_SIZE(k230_i2s_regs_info); i++) {
        register_reset(&s->regs_info[k230_i2s_regs_info[i].addr / 4]);
        k230_i2s_sync_compat_reg(s, k230_i2s_regs_info[i].addr);
    }
}

static int k230_i2s_post_load(void *opaque, int version_id);


static const VMStateDescription vmstate_k230_i2s = {
    .name = TYPE_K230_I2S,
    .version_id = 1,
    .post_load = k230_i2s_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, K230I2SState, K230_I2S_R_MAX),
        VMSTATE_UINT32(ws2812_byte_count, K230I2SState),
        VMSTATE_UINT32_ARRAY(ws2812_byte, K230I2SState,
                             K230_I2S_WS2812_SAMPLES),
        VMSTATE_UINT32(ws2812_padding_count, K230I2SState),
        VMSTATE_UINT32(ws2812_invalid_count, K230I2SState),
        VMSTATE_END_OF_LIST(),
    },
};

static int k230_i2s_post_load(void *opaque, int version_id)
{
    K230I2SState *s = K230_I2S(opaque);

    s->state_loaded = true;
    s->compat_imported = false;
    for (int i = 0; i < ARRAY_SIZE(k230_i2s_regs_info); i++) {
        k230_i2s_sync_compat_reg(s, k230_i2s_regs_info[i].addr);
    }

    return 0;
}


static void k230_i2s_realize(DeviceState *dev, Error **errp)
{
    K230I2SState *s = K230_I2S(dev);
    s->reg_array = register_init_block32(dev, k230_i2s_regs_info,
                                         ARRAY_SIZE(k230_i2s_regs_info),
                                         s->regs_info, s->regs,
                                         &k230_i2s_reg_array_ops,
                                         false, K230_I2S_SIZE);
    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_i2s_ops, s,
                          TYPE_K230_I2S, K230_I2S_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_i2s_instance_init(Object *obj)
{
    K230I2SState *s = K230_I2S(obj);

    object_property_add_uint32_ptr(obj, "ws2812-byte-count",
                                   &s->ws2812_byte_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "ws2812-byte0",
                                   &s->ws2812_byte[0], OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "ws2812-byte1",
                                   &s->ws2812_byte[1], OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "ws2812-byte2",
                                   &s->ws2812_byte[2], OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "ws2812-byte3",
                                   &s->ws2812_byte[3], OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "ws2812-padding-count",
                                   &s->ws2812_padding_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "ws2812-invalid-count",
                                   &s->ws2812_invalid_count,
                                   OBJ_PROP_FLAG_READ);
}

static void k230_i2s_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_i2s_realize;
    device_class_set_legacy_reset(dc, k230_i2s_reset);
    dc->vmsd = &vmstate_k230_i2s;
    dc->desc = "K230 I2S / WS2812 registers";
}

static const TypeInfo k230_i2s_type_info = {
    .name = TYPE_K230_I2S,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230I2SState),
    .instance_init = k230_i2s_instance_init,
    .class_init = k230_i2s_class_init,
};

static void k230_i2s_register_types(void)
{
    type_register_static(&k230_i2s_type_info);
}

type_init(k230_i2s_register_types)

/*
 * Rockchip Crypto V2 SHA-256 engine
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "crypto/hash.h"
#include "hw/core/register.h"
#include "hw/misc/rockchip_crypto_v2.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/dma.h"

#define ROCKCHIP_CRYPTO_V2_REG_WORDS \
    (ROCKCHIP_CRYPTO_V2_MMIO_SIZE / sizeof(uint32_t))
#define ROCKCHIP_CRYPTO_V2_MAX_DMA (32 * MiB)

REG32(RST_CTL, 0x004)
    FIELD(RST_CTL, SW_CC_RESET, 0, 1)
    FIELD(RST_CTL, SW_PKA_RESET, 2, 1)
REG32(DMA_INT_EN, 0x008)
REG32(DMA_INT_ST, 0x00c)
    FIELD(DMA_INT_ST, SRC_ITEM_DONE, 2, 1)
    FIELD(DMA_INT_ST, SRC_ERR, 4, 1)
    FIELD(DMA_INT_ST, LIST_ERR, 5, 1)
REG32(DMA_CTL, 0x010)
    FIELD(DMA_CTL, START, 0, 1)
    FIELD(DMA_CTL, RESTART, 1, 1)
REG32(DMA_LLI_ADDR, 0x014)
REG32(FIFO_CTL, 0x040)
    FIELD(FIFO_CTL, DOIN_BYTESWAP, 0, 1)
    FIELD(FIFO_CTL, DOUT_BYTESWAP, 1, 1)
REG32(HASH_CTL, 0x048)
    FIELD(HASH_CTL, ENABLE, 0, 1)
    FIELD(HASH_CTL, HW_PAD_ENABLE, 2, 1)
    FIELD(HASH_CTL, MODE, 4, 4)
REG32(HASH_DOUT_0, 0x3a0)
REG32(HASH_DOUT_1, 0x3a4)
REG32(HASH_DOUT_2, 0x3a8)
REG32(HASH_DOUT_3, 0x3ac)
REG32(HASH_DOUT_4, 0x3b0)
REG32(HASH_DOUT_5, 0x3b4)
REG32(HASH_DOUT_6, 0x3b8)
REG32(HASH_DOUT_7, 0x3bc)
REG32(HASH_DOUT_8, 0x3c0)
REG32(HASH_DOUT_9, 0x3c4)
REG32(HASH_DOUT_10, 0x3c8)
REG32(HASH_DOUT_11, 0x3cc)
REG32(HASH_DOUT_12, 0x3d0)
REG32(HASH_DOUT_13, 0x3d4)
REG32(HASH_DOUT_14, 0x3d8)
REG32(HASH_DOUT_15, 0x3dc)
REG32(HASH_VALID, 0x3e4)
    FIELD(HASH_VALID, VALID, 0, 1)

#define ROCKCHIP_CRYPTO_WRITE_MASK_SHIFT 16
#define ROCKCHIP_CRYPTO_HASH_MODE_SHA256 2
#define ROCKCHIP_CRYPTO_LLI_USER_CIPHER_START BIT(0)
#define ROCKCHIP_CRYPTO_LLI_USER_STRING_START BIT(1)
#define ROCKCHIP_CRYPTO_LLI_USER_STRING_LAST BIT(2)
#define ROCKCHIP_CRYPTO_LLI_DMA_LAST BIT(0)
#define ROCKCHIP_CRYPTO_LLI_DMA_SRC_DONE BIT(10)

typedef struct QEMU_PACKED RockchipCryptoV2LLI {
    uint32_t src_addr;
    uint32_t src_len;
    uint32_t dst_addr;
    uint32_t dst_len;
    uint32_t user_define;
    uint32_t reserved;
    uint32_t dma_ctrl;
    uint32_t next_addr;
} RockchipCryptoV2LLI;

struct RockchipCryptoV2State {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    uint32_t regs[ROCKCHIP_CRYPTO_V2_REG_WORDS];
    RegisterInfo regs_info[ROCKCHIP_CRYPTO_V2_REG_WORDS];
    QEMUBH *dma_bh;
    bool dma_pending;
    bool start_requested;
    bool restart_requested;
    bool reset_requested;
};

static void rockchip_crypto_v2_clear_hash(RockchipCryptoV2State *s)
{
    s->regs[R_DMA_INT_ST] = 0;
    s->regs[R_DMA_CTL] = 0;
    s->regs[R_FIFO_CTL] = 0;
    s->regs[R_HASH_CTL] = 0;
    memset(&s->regs[R_HASH_DOUT_0], 0,
           (R_HASH_DOUT_15 - R_HASH_DOUT_0 + 1) * sizeof(uint32_t));
    s->regs[R_HASH_VALID] = 0;
}

static uint64_t rockchip_crypto_v2_write_mask(RegisterInfo *reg,
                                               uint64_t value)
{
    uint32_t old = *(uint32_t *)reg->data & UINT16_MAX;
    uint32_t mask = extract32(value, ROCKCHIP_CRYPTO_WRITE_MASK_SHIFT, 16);
    uint32_t data = value & UINT16_MAX;

    return (old & ~mask) | (data & mask);
}

static uint64_t rockchip_crypto_v2_reset_pre_write(RegisterInfo *reg,
                                                    uint64_t value)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(reg->opaque);
    uint32_t reset = rockchip_crypto_v2_write_mask(reg, value);

    s->reset_requested = reset & (R_RST_CTL_SW_CC_RESET_MASK |
                                  R_RST_CTL_SW_PKA_RESET_MASK);
    return 0;
}

static void rockchip_crypto_v2_reset_post_write(RegisterInfo *reg,
                                                 uint64_t value)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(reg->opaque);

    if (!s->reset_requested) {
        return;
    }

    s->reset_requested = false;
    s->dma_pending = false;
    if (s->dma_bh) {
        qemu_bh_cancel(s->dma_bh);
    }
    rockchip_crypto_v2_clear_hash(s);
}

static uint64_t rockchip_crypto_v2_dma_pre_write(RegisterInfo *reg,
                                                  uint64_t value)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(reg->opaque);
    uint32_t control = rockchip_crypto_v2_write_mask(reg, value);

    s->start_requested = control & R_DMA_CTL_START_MASK;
    s->restart_requested = control & R_DMA_CTL_RESTART_MASK;
    return 0;
}

static void rockchip_crypto_v2_dma_error(RockchipCryptoV2State *s,
                                          uint32_t status);

static void rockchip_crypto_v2_dma_post_write(RegisterInfo *reg,
                                               uint64_t value)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(reg->opaque);

    if (s->restart_requested) {
        s->restart_requested = false;
        s->start_requested = false;
        rockchip_crypto_v2_dma_error(s, R_DMA_INT_ST_LIST_ERR_MASK);
        return;
    }

    if (!s->start_requested) {
        return;
    }

    s->start_requested = false;
    s->dma_pending = true;
    qemu_bh_schedule(s->dma_bh);
}

static void rockchip_crypto_v2_dma_error(RockchipCryptoV2State *s,
                                          uint32_t status)
{
    s->regs[R_HASH_VALID] &= ~R_HASH_VALID_VALID_MASK;
    s->regs[R_DMA_INT_ST] |= status;
    s->dma_pending = false;
}

static bool rockchip_crypto_v2_hash_config_valid(RockchipCryptoV2State *s)
{
    uint32_t hash = s->regs[R_HASH_CTL];
    uint32_t fifo = s->regs[R_FIFO_CTL];

    return FIELD_EX32(hash, HASH_CTL, MODE) ==
               ROCKCHIP_CRYPTO_HASH_MODE_SHA256 &&
           (hash & (R_HASH_CTL_ENABLE_MASK |
                    R_HASH_CTL_HW_PAD_ENABLE_MASK)) ==
               (R_HASH_CTL_ENABLE_MASK | R_HASH_CTL_HW_PAD_ENABLE_MASK) &&
           (fifo & (R_FIFO_CTL_DOIN_BYTESWAP_MASK |
                    R_FIFO_CTL_DOUT_BYTESWAP_MASK)) ==
               (R_FIFO_CTL_DOIN_BYTESWAP_MASK |
                R_FIFO_CTL_DOUT_BYTESWAP_MASK);
}

static void rockchip_crypto_v2_dma_bh(void *opaque)
{
    RockchipCryptoV2State *s = opaque;
    RockchipCryptoV2LLI lli;
    g_autofree uint8_t *input = NULL;
    g_autofree uint8_t *digest = NULL;
    Error *local_err = NULL;
    size_t digest_len = 0;
    uint32_t src_addr;
    uint32_t src_len;
    uint32_t user_define;
    uint32_t dma_ctrl;

    if (!s->dma_pending) {
        return;
    }

    if (!rockchip_crypto_v2_hash_config_valid(s) ||
        dma_memory_read(&address_space_memory, s->regs[R_DMA_LLI_ADDR],
                        &lli, sizeof(lli), MEMTXATTRS_UNSPECIFIED) !=
            MEMTX_OK) {
        rockchip_crypto_v2_dma_error(s, R_DMA_INT_ST_LIST_ERR_MASK);
        return;
    }

    src_addr = le32_to_cpu(lli.src_addr);
    src_len = le32_to_cpu(lli.src_len);
    user_define = le32_to_cpu(lli.user_define);
    dma_ctrl = le32_to_cpu(lli.dma_ctrl);

    if (!src_len || src_len > ROCKCHIP_CRYPTO_V2_MAX_DMA ||
        src_addr > UINT32_MAX - (src_len - 1) ||
        user_define !=
            (ROCKCHIP_CRYPTO_LLI_USER_CIPHER_START |
             ROCKCHIP_CRYPTO_LLI_USER_STRING_START |
             ROCKCHIP_CRYPTO_LLI_USER_STRING_LAST) ||
        dma_ctrl !=
            (ROCKCHIP_CRYPTO_LLI_DMA_LAST |
             ROCKCHIP_CRYPTO_LLI_DMA_SRC_DONE) ||
        le32_to_cpu(lli.dst_addr) || le32_to_cpu(lli.dst_len) ||
        le32_to_cpu(lli.reserved) || le32_to_cpu(lli.next_addr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unsupported LLI src=%08x/%u dst=%08x/%u "
                      "user=%08x reserved=%08x dma=%08x next=%08x\n",
                      TYPE_ROCKCHIP_CRYPTO_V2, src_addr, src_len,
                      le32_to_cpu(lli.dst_addr), le32_to_cpu(lli.dst_len),
                      user_define, le32_to_cpu(lli.reserved), dma_ctrl,
                      le32_to_cpu(lli.next_addr));
        rockchip_crypto_v2_dma_error(s, R_DMA_INT_ST_LIST_ERR_MASK);
        return;
    }

    input = g_malloc(src_len);
    if (dma_memory_read(&address_space_memory, src_addr, input, src_len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        rockchip_crypto_v2_dma_error(s, R_DMA_INT_ST_SRC_ERR_MASK);
        return;
    }

    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, input, src_len,
                           &digest, &digest_len, &local_err) < 0 ||
        digest_len != QCRYPTO_HASH_DIGEST_LEN_SHA256) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: SHA-256 calculation failed: %s\n",
                      TYPE_ROCKCHIP_CRYPTO_V2,
                      local_err ? error_get_pretty(local_err) :
                                  "invalid digest length");
        error_free(local_err);
        rockchip_crypto_v2_dma_error(s, R_DMA_INT_ST_SRC_ERR_MASK);
        return;
    }

    for (unsigned int i = 0; i < digest_len / sizeof(uint32_t); i++) {
        s->regs[R_HASH_DOUT_0 + i] = ldl_be_p(digest +
                                              i * sizeof(uint32_t));
    }

    s->regs[R_HASH_VALID] |= R_HASH_VALID_VALID_MASK;
    s->regs[R_DMA_INT_ST] |= R_DMA_INT_ST_SRC_ITEM_DONE_MASK;
    s->dma_pending = false;
}

static const RegisterAccessInfo rockchip_crypto_v2_regs_info[] = {
    { .name = "RST_CTL", .addr = A_RST_CTL,
      .pre_write = rockchip_crypto_v2_reset_pre_write,
      .post_write = rockchip_crypto_v2_reset_post_write },
    { .name = "DMA_INT_EN", .addr = A_DMA_INT_EN },
    { .name = "DMA_INT_ST", .addr = A_DMA_INT_ST,
      .w1c = R_DMA_INT_ST_SRC_ITEM_DONE_MASK |
             R_DMA_INT_ST_SRC_ERR_MASK | R_DMA_INT_ST_LIST_ERR_MASK,
      .ro = ~(R_DMA_INT_ST_SRC_ITEM_DONE_MASK |
              R_DMA_INT_ST_SRC_ERR_MASK | R_DMA_INT_ST_LIST_ERR_MASK) },
    { .name = "DMA_CTL", .addr = A_DMA_CTL,
      .pre_write = rockchip_crypto_v2_dma_pre_write,
      .post_write = rockchip_crypto_v2_dma_post_write },
    { .name = "DMA_LLI_ADDR", .addr = A_DMA_LLI_ADDR },
    { .name = "FIFO_CTL", .addr = A_FIFO_CTL,
      .pre_write = rockchip_crypto_v2_write_mask },
    { .name = "HASH_CTL", .addr = A_HASH_CTL,
      .pre_write = rockchip_crypto_v2_write_mask },
    { .name = "HASH_DOUT_0", .addr = A_HASH_DOUT_0, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_1", .addr = A_HASH_DOUT_1, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_2", .addr = A_HASH_DOUT_2, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_3", .addr = A_HASH_DOUT_3, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_4", .addr = A_HASH_DOUT_4, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_5", .addr = A_HASH_DOUT_5, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_6", .addr = A_HASH_DOUT_6, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_7", .addr = A_HASH_DOUT_7, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_8", .addr = A_HASH_DOUT_8, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_9", .addr = A_HASH_DOUT_9, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_10", .addr = A_HASH_DOUT_10, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_11", .addr = A_HASH_DOUT_11, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_12", .addr = A_HASH_DOUT_12, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_13", .addr = A_HASH_DOUT_13, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_14", .addr = A_HASH_DOUT_14, .ro = UINT32_MAX },
    { .name = "HASH_DOUT_15", .addr = A_HASH_DOUT_15, .ro = UINT32_MAX },
    { .name = "HASH_VALID", .addr = A_HASH_VALID,
      .w1c = R_HASH_VALID_VALID_MASK,
      .ro = ~R_HASH_VALID_VALID_MASK },
};

static const MemoryRegionOps rockchip_crypto_v2_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rockchip_crypto_v2_reset_hold(Object *obj, ResetType type)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(obj);

    if (s->dma_bh) {
        qemu_bh_cancel(s->dma_bh);
    }
    s->dma_pending = false;
    s->start_requested = false;
    s->restart_requested = false;
    s->reset_requested = false;

    for (unsigned int i = 0;
         i < ARRAY_SIZE(rockchip_crypto_v2_regs_info); i++) {
        register_reset(&s->regs_info[
                       rockchip_crypto_v2_regs_info[i].addr / 4]);
    }
    rockchip_crypto_v2_clear_hash(s);
}

static int rockchip_crypto_v2_post_load(void *opaque, int version_id)
{
    RockchipCryptoV2State *s = opaque;

    s->start_requested = false;
    s->restart_requested = false;
    s->reset_requested = false;
    if (s->dma_pending) {
        qemu_bh_schedule(s->dma_bh);
    }

    return 0;
}

static const VMStateDescription vmstate_rockchip_crypto_v2 = {
    .name = TYPE_ROCKCHIP_CRYPTO_V2,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rockchip_crypto_v2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RockchipCryptoV2State,
                             ROCKCHIP_CRYPTO_V2_REG_WORDS),
        VMSTATE_BOOL(dma_pending, RockchipCryptoV2State),
        VMSTATE_END_OF_LIST()
    },
};

static void rockchip_crypto_v2_realize(DeviceState *dev, Error **errp)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(dev);

    s->dma_bh = qemu_bh_new_guarded(rockchip_crypto_v2_dma_bh, s,
                                    &dev->mem_reentrancy_guard);
}

static void rockchip_crypto_v2_init(Object *obj)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(
        DEVICE(obj), rockchip_crypto_v2_regs_info,
        ARRAY_SIZE(rockchip_crypto_v2_regs_info), s->regs_info, s->regs,
        &rockchip_crypto_v2_ops, false, ROCKCHIP_CRYPTO_V2_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
}

static void rockchip_crypto_v2_finalize(Object *obj)
{
    RockchipCryptoV2State *s = ROCKCHIP_CRYPTO_V2(obj);

    if (s->dma_bh) {
        qemu_bh_delete(s->dma_bh);
    }
}

static void rockchip_crypto_v2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = rockchip_crypto_v2_realize;
    dc->vmsd = &vmstate_rockchip_crypto_v2;
    rc->phases.hold = rockchip_crypto_v2_reset_hold;
}

static const TypeInfo rockchip_crypto_v2_info = {
    .name = TYPE_ROCKCHIP_CRYPTO_V2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipCryptoV2State),
    .instance_init = rockchip_crypto_v2_init,
    .instance_finalize = rockchip_crypto_v2_finalize,
    .class_init = rockchip_crypto_v2_class_init,
};

static void rockchip_crypto_v2_register_types(void)
{
    type_register_static(&rockchip_crypto_v2_info);
}

type_init(rockchip_crypto_v2_register_types)

/*
 * Rockchip RK3588 DDR controller compatibility model
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/misc/rk3588_ddr.h"
#include "hw/core/register.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define RK3588_DDR_LEGACY_WORDS \
    (RK3588_DDR_LEGACY_MMIO_SIZE / sizeof(uint32_t))
#define RK3588_DDR_LEGACY_PHY_WORDS \
    (RK3588_DDR_LEGACY_PHY_MMIO_SIZE / sizeof(uint32_t))
#define RK3588_DDR_LEGACY_PHY_AUX_WORDS \
    (RK3588_DDR_LEGACY_PHY_AUX_MMIO_SIZE / sizeof(uint32_t))
#define RK3588_DDR_GLOBAL_WORDS \
    (RK3588_DDR_GLOBAL_MMIO_SIZE / sizeof(uint32_t))
#define RK3588_DDR_CHANNEL_WORDS \
    (RK3588_DDR_CHANNEL_MMIO_SIZE / sizeof(uint32_t))
#define RK3588_DDRPHY_WORDS \
    (RK3588_DDRPHY_MMIO_SIZE / sizeof(uint32_t))

#define RK3588_DDR_CTRL_BUSY_MASK       (BIT(31) | BIT(3))
#define RK3588_DDR_STATUS_LOW_MASK      0x7
#define RK3588_DDR_STATUS_READY         BIT(0)
#define RK3588_DDR_STATUS_ACK           BIT(31)
#define RK3588_DDR_CMD_START            BIT(31)
#define RK3588_DDR_BUSY                 BIT(0)
#define RK3588_DDR_GATE_BUSY            BIT(0)
#define RK3588_DDR_GATE_ENABLE          BIT(5)
#define RK3588_DDR_CHANNEL_PHY_BUSY     BIT(16)
#define RK3588_DDRPHY_STATUS_ACTIVE     0x3

REG32(DDR_CTRL, 0x00000)
REG32(DDR_CTRL_STATUS, 0x00004)
REG32(CHANNEL_STATUS, 0x00014)
REG32(CHANNEL_CMD, 0x00080)
REG32(CHANNEL_BUSY, 0x00090)
REG32(CHANNEL_GATE_CMD, 0x00510)
REG32(CHANNEL_GATE_STATUS, 0x00514)
REG32(CHANNEL_PHY_STATUS, 0x00b90)
REG32(CHANNEL_GATE_CTRL, 0x00c80)

REG32(CHANNEL_STATUS_ALIAS, 0x10014)
REG32(CHANNEL_CMD_ALIAS, 0x10080)
REG32(CHANNEL_BUSY_ALIAS, 0x10090)
REG32(CHANNEL_GATE_CMD_ALIAS, 0x10510)
REG32(CHANNEL_GATE_STATUS_ALIAS, 0x10514)
REG32(CHANNEL_PHY_STATUS_ALIAS, 0x10b90)
REG32(CHANNEL_GATE_CTRL_ALIAS, 0x10c80)

REG32(DDRPHY_CTRL, 0x00154)
REG32(DDRPHY_STATUS, 0x00184)
REG32(DDR_PHY_GATE_CTRL, 0x000b0)

struct RK3588DDRState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_arrays[RK3588_DDR_MMIO_COUNT];
    RegisterInfo *regs_info[RK3588_DDR_MMIO_COUNT];

    uint32_t legacy_regs[RK3588_DDR_LEGACY_WINDOW_COUNT]
                        [RK3588_DDR_LEGACY_WORDS];
    uint32_t legacy_phy_regs[RK3588_DDR_LEGACY_PHY_WORDS];
    uint32_t legacy_phy_aux_regs[RK3588_DDR_LEGACY_PHY_AUX_WORDS];
    uint32_t global_regs[RK3588_DDR_GLOBAL_WORDS];
    uint32_t channel_regs[RK3588_DDR_CHANNEL_COUNT]
                         [RK3588_DDR_CHANNEL_WORDS];
    uint32_t ddrphy_regs[RK3588_DDRPHY_WORDS];

    bool gate_done;
    bool last_phy_gate;
    bool gate_bit5_clear;
};

static uint64_t rk3588_ddr_ctrl_post_read(RegisterInfo *reg, uint64_t value)
{
    return value & ~RK3588_DDR_CTRL_BUSY_MASK;
}

static uint64_t rk3588_ddr_ctrl_status_post_read(RegisterInfo *reg,
                                                  uint64_t value)
{
    return 0;
}

static uint64_t rk3588_ddr_handshake_post_read(RegisterInfo *reg,
                                                uint64_t value)
{
    bool request = value & RK3588_DDR_STATUS_READY;

    value &= ~RK3588_DDR_STATUS_LOW_MASK;
    value |= RK3588_DDR_STATUS_READY;
    if (request) {
        value |= RK3588_DDR_STATUS_ACK;
    }

    return value;
}

static uint64_t rk3588_ddr_legacy_status_post_read(RegisterInfo *reg,
                                                    uint64_t value)
{
    value &= ~RK3588_DDR_STATUS_LOW_MASK;
    return value | RK3588_DDR_STATUS_READY;
}

static uint64_t rk3588_ddr_cmd_post_read(RegisterInfo *reg, uint64_t value)
{
    return value & ~RK3588_DDR_CMD_START;
}

static uint64_t rk3588_ddr_busy_post_read(RegisterInfo *reg, uint64_t value)
{
    return value & ~RK3588_DDR_BUSY;
}

static uint64_t rk3588_ddr_gate_status_post_read(RegisterInfo *reg,
                                                  uint64_t value)
{
    RK3588DDRState *s = RK3588_DDR(reg->opaque);

    if (s->gate_done || s->gate_bit5_clear) {
        return value | RK3588_DDR_GATE_BUSY;
    }

    return value & ~RK3588_DDR_GATE_BUSY;
}

static uint64_t rk3588_ddr_channel_phy_post_read(RegisterInfo *reg,
                                                  uint64_t value)
{
    return value & ~RK3588_DDR_CHANNEL_PHY_BUSY;
}

static uint64_t rk3588_ddrphy_status_post_read(RegisterInfo *reg,
                                                uint64_t value)
{
    RK3588DDRState *s = RK3588_DDR(reg->opaque);

    value &= ~RK3588_DDRPHY_STATUS_ACTIVE;
    value |= s->ddrphy_regs[R_DDRPHY_CTRL] &
             RK3588_DDRPHY_STATUS_ACTIVE;
    return value;
}

static void rk3588_ddr_gate_cmd_post_write(RegisterInfo *reg, uint64_t value)
{
    RK3588DDRState *s = RK3588_DDR(reg->opaque);

    s->gate_bit5_clear = !(value & RK3588_DDR_GATE_ENABLE);
    s->last_phy_gate = false;
}

static void rk3588_ddr_gate_ctrl_post_write(RegisterInfo *reg, uint64_t value)
{
    RK3588DDRState *s = RK3588_DDR(reg->opaque);

    if (value == 0 && !s->last_phy_gate) {
        s->gate_done = false;
    }
    s->last_phy_gate = false;
}

static void rk3588_ddr_phy_gate_post_write(RegisterInfo *reg, uint64_t value)
{
    RK3588DDRState *s = RK3588_DDR(reg->opaque);

    if (!(value & RK3588_DDR_GATE_ENABLE)) {
        s->gate_done = true;
        s->last_phy_gate = true;
    } else {
        s->last_phy_gate = false;
    }
}

static const RegisterAccessInfo rk3588_ddr_legacy_regs_info[] = {
    { .name = "DDR_PHY_GATE_CTRL", .addr = A_DDR_PHY_GATE_CTRL,
      .reset = UINT32_MAX, .post_write = rk3588_ddr_phy_gate_post_write },
    { .name = "CHANNEL_STATUS", .addr = A_CHANNEL_STATUS,
      .reset = UINT32_MAX,
      .post_read = rk3588_ddr_legacy_status_post_read },
    { .name = "CHANNEL_CMD", .addr = A_CHANNEL_CMD,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_cmd_post_read },
    { .name = "CHANNEL_BUSY", .addr = A_CHANNEL_BUSY,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_busy_post_read },
    { .name = "CHANNEL_GATE_CMD", .addr = A_CHANNEL_GATE_CMD,
      .reset = UINT32_MAX, .post_write = rk3588_ddr_gate_cmd_post_write },
    { .name = "CHANNEL_GATE_STATUS", .addr = A_CHANNEL_GATE_STATUS,
      .reset = UINT32_MAX,
      .post_read = rk3588_ddr_gate_status_post_read },
    { .name = "CHANNEL_PHY_STATUS", .addr = A_CHANNEL_PHY_STATUS,
      .reset = UINT32_MAX,
      .post_read = rk3588_ddr_channel_phy_post_read },
    { .name = "CHANNEL_GATE_CTRL", .addr = A_CHANNEL_GATE_CTRL,
      .reset = UINT32_MAX, .post_write = rk3588_ddr_gate_ctrl_post_write },
};

static const RegisterAccessInfo rk3588_ddr_ctrl_regs_info[] = {
    { .name = "DDR_CTRL", .addr = A_DDR_CTRL,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_ctrl_post_read },
    { .name = "DDR_CTRL_STATUS", .addr = A_DDR_CTRL_STATUS,
      .reset = UINT32_MAX,
      .post_read = rk3588_ddr_ctrl_status_post_read },
    { .name = "CHANNEL_STATUS", .addr = A_CHANNEL_STATUS,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_handshake_post_read },
    { .name = "CHANNEL_CMD", .addr = A_CHANNEL_CMD,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_cmd_post_read },
    { .name = "CHANNEL_BUSY", .addr = A_CHANNEL_BUSY,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_busy_post_read },
    { .name = "CHANNEL_GATE_CMD", .addr = A_CHANNEL_GATE_CMD,
      .reset = UINT32_MAX, .post_write = rk3588_ddr_gate_cmd_post_write },
    { .name = "CHANNEL_GATE_STATUS", .addr = A_CHANNEL_GATE_STATUS,
      .reset = UINT32_MAX,
      .post_read = rk3588_ddr_gate_status_post_read },
    { .name = "CHANNEL_PHY_STATUS", .addr = A_CHANNEL_PHY_STATUS,
      .reset = UINT32_MAX,
      .post_read = rk3588_ddr_channel_phy_post_read },
    { .name = "CHANNEL_GATE_CTRL", .addr = A_CHANNEL_GATE_CTRL,
      .reset = UINT32_MAX, .post_write = rk3588_ddr_gate_ctrl_post_write },
    { .name = "CHANNEL_STATUS_ALIAS", .addr = A_CHANNEL_STATUS_ALIAS,
      .reset = UINT32_MAX,
      .post_read = rk3588_ddr_legacy_status_post_read },
    { .name = "CHANNEL_CMD_ALIAS", .addr = A_CHANNEL_CMD_ALIAS,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_cmd_post_read },
    { .name = "CHANNEL_BUSY_ALIAS", .addr = A_CHANNEL_BUSY_ALIAS,
      .reset = UINT32_MAX, .post_read = rk3588_ddr_busy_post_read },
    { .name = "CHANNEL_GATE_CMD_ALIAS", .addr = A_CHANNEL_GATE_CMD_ALIAS,
      .reset = UINT32_MAX, .post_write = rk3588_ddr_gate_cmd_post_write },
    { .name = "CHANNEL_GATE_STATUS_ALIAS",
      .addr = A_CHANNEL_GATE_STATUS_ALIAS, .reset = UINT32_MAX,
      .post_read = rk3588_ddr_gate_status_post_read },
    { .name = "CHANNEL_PHY_STATUS_ALIAS",
      .addr = A_CHANNEL_PHY_STATUS_ALIAS, .reset = UINT32_MAX,
      .post_read = rk3588_ddr_channel_phy_post_read },
    { .name = "CHANNEL_GATE_CTRL_ALIAS", .addr = A_CHANNEL_GATE_CTRL_ALIAS,
      .reset = UINT32_MAX, .post_write = rk3588_ddr_gate_ctrl_post_write },
};

static const RegisterAccessInfo rk3588_ddrphy_regs_info[] = {
    { .name = "DDRPHY_CTRL", .addr = A_DDRPHY_CTRL,
      .reset = UINT32_MAX },
    { .name = "DDRPHY_STATUS", .addr = A_DDRPHY_STATUS,
      .reset = UINT32_MAX, .post_read = rk3588_ddrphy_status_post_read },
};

static unsigned int rk3588_ddr_window_index(RK3588DDRState *s,
                                             RegisterInfoArray *reg_array)
{
    for (unsigned int i = 0; i < RK3588_DDR_MMIO_COUNT; i++) {
        if (s->reg_arrays[i] == reg_array) {
            return i;
        }
    }

    g_assert_not_reached();
}

static uint32_t *rk3588_ddr_window_regs(RK3588DDRState *s,
                                        unsigned int index)
{
    if (index < RK3588_DDR_LEGACY_WINDOW_COUNT) {
        return s->legacy_regs[index];
    }
    if (index == RK3588_DDR_MMIO_LEGACY_PHY) {
        return s->legacy_phy_regs;
    }
    if (index == RK3588_DDR_MMIO_LEGACY_PHY_AUX) {
        return s->legacy_phy_aux_regs;
    }
    if (index == RK3588_DDR_MMIO_GLOBAL) {
        return s->global_regs;
    }
    if (index >= RK3588_DDR_MMIO_CHANNEL0 &&
        index < RK3588_DDR_MMIO_DDRPHY) {
        return s->channel_regs[index - RK3588_DDR_MMIO_CHANNEL0];
    }
    if (index == RK3588_DDR_MMIO_DDRPHY) {
        return s->ddrphy_regs;
    }
    g_assert_not_reached();
}

static uint64_t rk3588_ddr_window_size(unsigned int index)
{
    if (index < RK3588_DDR_LEGACY_WINDOW_COUNT) {
        return RK3588_DDR_LEGACY_MMIO_SIZE;
    }
    if (index == RK3588_DDR_MMIO_LEGACY_PHY) {
        return RK3588_DDR_LEGACY_PHY_MMIO_SIZE;
    }
    if (index == RK3588_DDR_MMIO_LEGACY_PHY_AUX) {
        return RK3588_DDR_LEGACY_PHY_AUX_MMIO_SIZE;
    }
    if (index == RK3588_DDR_MMIO_GLOBAL) {
        return RK3588_DDR_GLOBAL_MMIO_SIZE;
    }
    if (index >= RK3588_DDR_MMIO_CHANNEL0 &&
        index < RK3588_DDR_MMIO_DDRPHY) {
        return RK3588_DDR_CHANNEL_MMIO_SIZE;
    }
    if (index == RK3588_DDR_MMIO_DDRPHY) {
        return RK3588_DDRPHY_MMIO_SIZE;
    }
    g_assert_not_reached();
}

static RegisterInfo *rk3588_ddr_find_register(RegisterInfoArray *reg_array,
                                               hwaddr addr)
{
    for (unsigned int i = 0; i < reg_array->num_elements; i++) {
        RegisterInfo *reg = reg_array->r[i];

        if (reg->access->addr == addr) {
            return reg;
        }
    }

    return NULL;
}

static bool rk3588_ddr_access_valid(hwaddr addr, unsigned int size,
                                    uint64_t window_size)
{
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return false;
    }

    return addr <= window_size && size <= window_size - addr;
}

static uint64_t rk3588_ddr_raw_read(uint32_t *regs, hwaddr addr,
                                    unsigned int size)
{
    uint64_t value = 0;

    for (unsigned int i = 0; i < size; i++) {
        unsigned int shift = ((addr + i) & 3) * 8;
        uint8_t byte = regs[(addr + i) / sizeof(uint32_t)] >> shift;

        value |= (uint64_t)byte << (i * 8);
    }

    return value;
}

static void rk3588_ddr_raw_write(uint32_t *regs, hwaddr addr,
                                 uint64_t value, unsigned int size)
{
    for (unsigned int i = 0; i < size; i++) {
        unsigned int index = (addr + i) / sizeof(uint32_t);
        unsigned int shift = ((addr + i) & 3) * 8;
        uint32_t mask = UINT32_C(0xff) << shift;

        regs[index] = (regs[index] & ~mask) |
                      (((value >> (i * 8)) & 0xff) << shift);
    }
}

static uint64_t rk3588_ddr_read(void *opaque, hwaddr addr, unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3588DDRState *s = RK3588_DDR(register_array_get_owner(reg_array));
    unsigned int index = rk3588_ddr_window_index(s, reg_array);
    uint64_t window_size = rk3588_ddr_window_size(index);

    if (!rk3588_ddr_access_valid(addr, size, window_size)) {
        return 0;
    }

    if (index <= RK3588_DDR_MMIO_LEGACY_PHY_AUX &&
        size == sizeof(uint32_t) &&
        (addr & 0xfff) == A_DDRPHY_STATUS) {
        uint32_t *regs = rk3588_ddr_window_regs(s, index);
        uint32_t value = rk3588_ddr_raw_read(regs, addr, size);
        hwaddr ctrl = addr - A_DDRPHY_STATUS + A_DDRPHY_CTRL;

        value &= ~RK3588_DDRPHY_STATUS_ACTIVE;
        value |= rk3588_ddr_raw_read(regs, ctrl, size) &
                 RK3588_DDRPHY_STATUS_ACTIVE;
        return value;
    }

    if (size == 4 && rk3588_ddr_find_register(reg_array, addr)) {
        return register_read_memory(reg_array, addr, size);
    }

    return rk3588_ddr_raw_read(rk3588_ddr_window_regs(s, index), addr, size);
}

static void rk3588_ddr_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3588DDRState *s = RK3588_DDR(register_array_get_owner(reg_array));
    unsigned int index = rk3588_ddr_window_index(s, reg_array);
    uint64_t window_size = rk3588_ddr_window_size(index);
    RegisterInfo *reg;

    if (!rk3588_ddr_access_valid(addr, size, window_size)) {
        return;
    }

    reg = size == 4 ? rk3588_ddr_find_register(reg_array, addr) : NULL;
    if (size == 4 &&
        (!reg || (reg->access->post_write != rk3588_ddr_gate_cmd_post_write &&
                  reg->access->post_write != rk3588_ddr_gate_ctrl_post_write &&
                  reg->access->post_write != rk3588_ddr_phy_gate_post_write))) {
        s->last_phy_gate = false;
    }

    if (reg) {
        register_write_memory(reg_array, addr, value, size);
        return;
    }

    rk3588_ddr_raw_write(rk3588_ddr_window_regs(s, index), addr, value,
                         size);
}

static const MemoryRegionOps rk3588_ddr_ops = {
    .read = rk3588_ddr_read,
    .write = rk3588_ddr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void rk3588_ddr_reset_hold(Object *obj, ResetType type)
{
    RK3588DDRState *s = RK3588_DDR(obj);

    memset(s->legacy_regs, 0xff, sizeof(s->legacy_regs));
    memset(s->legacy_phy_regs, 0xff, sizeof(s->legacy_phy_regs));
    memset(s->legacy_phy_aux_regs, 0xff,
           sizeof(s->legacy_phy_aux_regs));
    memset(s->global_regs, 0xff, sizeof(s->global_regs));
    memset(s->channel_regs, 0xff, sizeof(s->channel_regs));
    memset(s->ddrphy_regs, 0xff, sizeof(s->ddrphy_regs));

    for (unsigned int i = 0; i < RK3588_DDR_MMIO_COUNT; i++) {
        for (unsigned int j = 0; j < s->reg_arrays[i]->num_elements; j++) {
            register_reset(s->reg_arrays[i]->r[j]);
        }
    }

    s->gate_done = false;
    s->last_phy_gate = false;
    s->gate_bit5_clear = false;
}

static const VMStateDescription vmstate_rk3588_ddr = {
    .name = TYPE_RK3588_DDR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(legacy_regs, RK3588DDRState,
                              RK3588_DDR_LEGACY_WINDOW_COUNT,
                              RK3588_DDR_LEGACY_WORDS),
        VMSTATE_UINT32_ARRAY(legacy_phy_regs, RK3588DDRState,
                             RK3588_DDR_LEGACY_PHY_WORDS),
        VMSTATE_UINT32_ARRAY(legacy_phy_aux_regs, RK3588DDRState,
                             RK3588_DDR_LEGACY_PHY_AUX_WORDS),
        VMSTATE_UINT32_ARRAY(global_regs, RK3588DDRState,
                             RK3588_DDR_GLOBAL_WORDS),
        VMSTATE_UINT32_2DARRAY(channel_regs, RK3588DDRState,
                              RK3588_DDR_CHANNEL_COUNT,
                              RK3588_DDR_CHANNEL_WORDS),
        VMSTATE_UINT32_ARRAY(ddrphy_regs, RK3588DDRState,
                             RK3588_DDRPHY_WORDS),
        VMSTATE_BOOL(gate_done, RK3588DDRState),
        VMSTATE_BOOL(last_phy_gate, RK3588DDRState),
        VMSTATE_BOOL(gate_bit5_clear, RK3588DDRState),
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_ddr_init_window(RK3588DDRState *s, unsigned int index,
                                   uint32_t *regs, uint64_t size,
                                   const RegisterAccessInfo *regs_info,
                                   unsigned int regs_count)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(s);
    unsigned int max_index = 0;

    for (unsigned int i = 0; i < regs_count; i++) {
        max_index = MAX(max_index, (unsigned int)(regs_info[i].addr / 4));
    }

    s->regs_info[index] = g_new0(RegisterInfo, max_index + 1);
    s->reg_arrays[index] = register_init_block32(
        DEVICE(s), regs_info, regs_count, s->regs_info[index], regs,
        &rk3588_ddr_ops, false, size);
    sysbus_init_mmio(sbd, &s->reg_arrays[index]->mem);
}

static void rk3588_ddr_init(Object *obj)
{
    RK3588DDRState *s = RK3588_DDR(obj);

    for (unsigned int i = 0; i < RK3588_DDR_LEGACY_WINDOW_COUNT; i++) {
        rk3588_ddr_init_window(s, i, s->legacy_regs[i],
                               RK3588_DDR_LEGACY_MMIO_SIZE,
                               rk3588_ddr_legacy_regs_info,
                               ARRAY_SIZE(rk3588_ddr_legacy_regs_info));
    }
    rk3588_ddr_init_window(s, RK3588_DDR_MMIO_LEGACY_PHY,
                           s->legacy_phy_regs,
                           RK3588_DDR_LEGACY_PHY_MMIO_SIZE,
                           rk3588_ddr_legacy_regs_info,
                           ARRAY_SIZE(rk3588_ddr_legacy_regs_info));
    rk3588_ddr_init_window(s, RK3588_DDR_MMIO_LEGACY_PHY_AUX,
                           s->legacy_phy_aux_regs,
                           RK3588_DDR_LEGACY_PHY_AUX_MMIO_SIZE,
                           rk3588_ddr_legacy_regs_info,
                           ARRAY_SIZE(rk3588_ddr_legacy_regs_info));
    rk3588_ddr_init_window(s, RK3588_DDR_MMIO_GLOBAL, s->global_regs,
                           RK3588_DDR_GLOBAL_MMIO_SIZE,
                           rk3588_ddr_ctrl_regs_info,
                           ARRAY_SIZE(rk3588_ddr_ctrl_regs_info));

    for (unsigned int i = 0; i < RK3588_DDR_CHANNEL_COUNT; i++) {
        rk3588_ddr_init_window(s, RK3588_DDR_MMIO_CHANNEL(i),
                               s->channel_regs[i],
                               RK3588_DDR_CHANNEL_MMIO_SIZE,
                               rk3588_ddr_ctrl_regs_info,
                               ARRAY_SIZE(rk3588_ddr_ctrl_regs_info));
    }

    rk3588_ddr_init_window(s, RK3588_DDR_MMIO_DDRPHY, s->ddrphy_regs,
                           RK3588_DDRPHY_MMIO_SIZE,
                           rk3588_ddrphy_regs_info,
                           ARRAY_SIZE(rk3588_ddrphy_regs_info));
}

static void rk3588_ddr_finalize(Object *obj)
{
    RK3588DDRState *s = RK3588_DDR(obj);

    for (unsigned int i = 0; i < RK3588_DDR_MMIO_COUNT; i++) {
        g_free(s->regs_info[i]);
    }
}

static void rk3588_ddr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_rk3588_ddr;
    rc->phases.hold = rk3588_ddr_reset_hold;
}

static const TypeInfo rk3588_ddr_info = {
    .name = TYPE_RK3588_DDR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588DDRState),
    .instance_init = rk3588_ddr_init,
    .instance_finalize = rk3588_ddr_finalize,
    .class_init = rk3588_ddr_class_init,
};

static void rk3588_ddr_register_types(void)
{
    type_register_static(&rk3588_ddr_info);
}

type_init(rk3588_ddr_register_types)

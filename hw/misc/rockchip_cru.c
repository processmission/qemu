/*
 * Rockchip RK3588 clock registers
 *
 * Copyright (C) 2026 Process Mission
 *
 * Authors:
 *    Chao Liu <chao.liu@processmission.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_CRU "rockchip-cru"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipCRUState, ROCKCHIP_CRU)

#define ROCKCHIP_CRU_SIZE 0x5c000
#define ROCKCHIP_CRU_NUM_PLL_REGS (4 * 3 + 5 * 4)
#define ROCKCHIP_CRU_NUM_DDR_REGS (4 * (2 * 7 + 3))
#define ROCKCHIP_CRU_NUM_REGS (ROCKCHIP_CRU_NUM_PLL_REGS + 10 + 31 + 5 + \
                             ROCKCHIP_CRU_NUM_DDR_REGS)
#define ROCKCHIP_CRU_NUM_WORDS (ROCKCHIP_CRU_SIZE / 4)
#define ROCKCHIP_CRU_RKNPU_RESET_COUNT 3
#define CRU_SOFTRST_CON(n) (0xa00 + (n) * 4)

struct RockchipCRUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    RegisterInfo regs_info[ROCKCHIP_CRU_NUM_REGS];
    uint32_t regs[ROCKCHIP_CRU_NUM_REGS];
    /* Other registers use storage with a high-word write mask. */
    uint32_t generic[ROCKCHIP_CRU_NUM_WORDS];
    qemu_irq rknpu_reset[ROCKCHIP_CRU_RKNPU_RESET_COUNT];
};

#define PLL_POWER_DOWN BIT(13)
#define PLL_LOCKED BIT(15)

static const struct {
    hwaddr offset;
    uint32_t mask;
} rockchip_cru_rknpu_resets[ROCKCHIP_CRU_RKNPU_RESET_COUNT] = {
    [0] = { CRU_SOFTRST_CON(30), BIT(6) | BIT(8) },
    [1] = { CRU_SOFTRST_CON(27), BIT(0) | BIT(2) },
    [2] = { CRU_SOFTRST_CON(28), BIT(0) | BIT(2) },
};

static void rockchip_cru_update_rknpu_reset(RockchipCRUState *s, hwaddr offset,
                                           uint32_t value)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(rockchip_cru_rknpu_resets); i++) {
        if (offset == rockchip_cru_rknpu_resets[i].offset) {
            qemu_set_irq(s->rknpu_reset[i],
                         !!(value & rockchip_cru_rknpu_resets[i].mask));
            return;
        }
    }
}

static uint64_t rockchip_cru_masked_write(RegisterInfo *reg, uint64_t value)
{
    uint32_t old = *(uint32_t *)reg->data;
    uint16_t mask = value >> 16;

    return (old & ~mask) | (value & mask);
}

static RegisterInfo *rockchip_cru_register(RockchipCRUState *s, hwaddr offset)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        if (s->regs_info[i].access->addr == offset) {
            return &s->regs_info[i];
        }
    }
    return NULL;
}

static uint64_t rockchip_cru_pll_lock(RegisterInfo *reg, uint64_t value)
{
    RockchipCRUState *s = ROCKCHIP_CRU(reg->opaque);
    hwaddr base = reg->access->addr - 0x18;
    uint32_t con0 = *(uint32_t *)rockchip_cru_register(s, base)->data;
    uint32_t con1 = *(uint32_t *)rockchip_cru_register(s, base + 4)->data;
    bool powered = !(con1 & PLL_POWER_DOWN);
    bool configured = (con0 & 0x3ff) >= 64 &&
                      (con1 & 0x3f) &&
                      extract32(con1, 6, 3) <= 6;

    /* PLL settling is synchronous after configuration and power-up. */
    return (value & ~PLL_LOCKED) | (powered && configured ? PLL_LOCKED : 0);
}

#define CRU_PLL_CON0(label, base) \
    { .name = label "_CON0", .addr = base, \
      .pre_write = rockchip_cru_masked_write }

#define CRU_PLL_CON1(label, base) \
    { .name = label "_CON1", .addr = base + 4, \
      .pre_write = rockchip_cru_masked_write }

#define CRU_PLL_CON2(label, base) \
    { .name = label "_CON2", .addr = base + 8, .ro = 0xffff0000 }

#define CRU_PLL_CON6(label, base) \
    { .name = label "_CON6", .addr = base + 0x18, .ro = UINT32_MAX, \
      .post_read = rockchip_cru_pll_lock }

#define CRU_CLOCK(label, offset) \
    { .name = label, .addr = offset, .pre_write = rockchip_cru_masked_write }

#define CRU_FRAC(label, offset, reset_value) \
    { .name = label, .addr = offset, .reset = reset_value }

static const RegisterAccessInfo rockchip_cru_pll_regs[] = {
    CRU_PLL_CON0("B0PLL", 0x50000),
    CRU_PLL_CON1("B0PLL", 0x50000),
    CRU_PLL_CON6("B0PLL", 0x50000),
    CRU_PLL_CON0("B1PLL", 0x52020),
    CRU_PLL_CON1("B1PLL", 0x52020),
    CRU_PLL_CON6("B1PLL", 0x52020),
    CRU_PLL_CON0("LPLL", 0x58040),
    CRU_PLL_CON1("LPLL", 0x58040),
    CRU_PLL_CON6("LPLL", 0x58040),
    CRU_PLL_CON0("V0PLL", 0x00160),
    CRU_PLL_CON1("V0PLL", 0x00160),
    CRU_PLL_CON2("V0PLL", 0x00160),
    CRU_PLL_CON6("V0PLL", 0x00160),
    CRU_PLL_CON0("AUPLL", 0x00180),
    CRU_PLL_CON1("AUPLL", 0x00180),
    CRU_PLL_CON2("AUPLL", 0x00180),
    CRU_PLL_CON6("AUPLL", 0x00180),
    CRU_PLL_CON0("CPLL", 0x001a0),
    CRU_PLL_CON1("CPLL", 0x001a0),
    CRU_PLL_CON2("CPLL", 0x001a0),
    CRU_PLL_CON6("CPLL", 0x001a0),
    CRU_PLL_CON0("GPLL", 0x001c0),
    CRU_PLL_CON1("GPLL", 0x001c0),
    CRU_PLL_CON2("GPLL", 0x001c0),
    CRU_PLL_CON6("GPLL", 0x001c0),
    CRU_PLL_CON0("NPLL", 0x001e0),
    CRU_PLL_CON1("NPLL", 0x001e0),
    CRU_PLL_CON6("NPLL", 0x001e0),
    CRU_PLL_CON0("PPLL", 0x08200),
    CRU_PLL_CON1("PPLL", 0x08200),
    CRU_PLL_CON2("PPLL", 0x08200),
    CRU_PLL_CON6("PPLL", 0x08200),
};

static const RegisterAccessInfo rockchip_cru_clock_regs[] = {
    CRU_CLOCK("MODE_CON0", 0x00280),
    CRU_CLOCK("CLKSEL_CON8", 0x00320),
    CRU_CLOCK("CLKSEL_CON9", 0x00324),
    CRU_CLOCK("CLKSEL_CON38", 0x00398),
    CRU_CLOCK("CLKSEL_CON78", 0x00438),
    CRU_CLOCK("CLKSEL_CON165", 0x00594),
    CRU_CLOCK("CLKGATE_CON19", 0x0084c),
    CRU_CLOCK("BIGCORE0_MODE_CON0", 0x50280),
    CRU_CLOCK("BIGCORE1_MODE_CON0", 0x52280),
    CRU_CLOCK("DSU_MODE_CON0", 0x58280),

    /* Fractional dividers store the numerator and denominator in 32 bits. */
    CRU_FRAC("CLKSEL_CON25", 0x00364, 0x03355460),
    CRU_FRAC("CLKSEL_CON27", 0x0036c, 0x03355460),
    CRU_FRAC("CLKSEL_CON29", 0x00374, 0x03355460),
    CRU_FRAC("CLKSEL_CON31", 0x0037c, 0x03355460),
    CRU_FRAC("CLKSEL_CON33", 0x00384, 0x03355460),
    CRU_FRAC("CLKSEL_CON35", 0x0038c, 0x03355460),
    CRU_FRAC("CLKSEL_CON42", 0x003a8, 0x001403de),
    CRU_FRAC("CLKSEL_CON44", 0x003b0, 0x001403de),
    CRU_FRAC("CLKSEL_CON46", 0x003b8, 0x001403de),
    CRU_FRAC("CLKSEL_CON48", 0x003c0, 0x001403de),
    CRU_FRAC("CLKSEL_CON50", 0x003c8, 0x001403de),
    CRU_FRAC("CLKSEL_CON52", 0x003d0, 0x001403de),
    CRU_FRAC("CLKSEL_CON54", 0x003d8, 0x001403de),
    CRU_FRAC("CLKSEL_CON56", 0x003e0, 0x001403de),
    CRU_FRAC("CLKSEL_CON58", 0x003e8, 0x001403de),
    CRU_FRAC("CLKSEL_CON119", 0x004dc, 0x03355460),
    CRU_FRAC("CLKSEL_CON121", 0x004e4, 0x03355460),
    CRU_FRAC("CLKSEL_CON123", 0x004ec, 0x03355460),
    CRU_FRAC("CLKSEL_CON125", 0x004f4, 0x03355460),
    CRU_FRAC("CLKSEL_CON130", 0x00508, 0x03355460),
    CRU_FRAC("CLKSEL_CON139", 0x0052c, 0x03355460),
    CRU_FRAC("CLKSEL_CON141", 0x00534, 0x03355460),
    CRU_FRAC("CLKSEL_CON145", 0x00544, 0x03355460),
    CRU_FRAC("CLKSEL_CON147", 0x0054c, 0x03355460),
    CRU_FRAC("CLKSEL_CON149", 0x00554, 0x03355460),
    CRU_FRAC("CLKSEL_CON151", 0x0055c, 0x03355460),
    CRU_FRAC("CLKSEL_CON154", 0x00568, 0x03355460),
    CRU_FRAC("CLKSEL_CON156", 0x00570, 0x03355460),
    CRU_FRAC("PMU1_CLKSEL_CON4", 0x30310, 0x001403de),
    CRU_FRAC("PMU1_CLKSEL_CON6", 0x30318, 0x03355460),
    CRU_FRAC("PMU1_CLKSEL_CON8", 0x30320, 0x03355460),

    { .name = "GLB_CNT_TH", .addr = 0xc00, .reset = 0x64,
      .ro = 0xfffffc00 },
    { .name = "GLBRST_ST", .addr = 0xc04, .ro = 0xffff0000 },
    { .name = "GLB_SRST_FST_VALUE", .addr = 0xc08, .ro = 0xffff0000 },
    { .name = "GLB_SRST_SND_VALUE", .addr = 0xc0c, .ro = 0xffff0000 },
    { .name = "GLB_RST_CON", .addr = 0xc10, .ro = 0xffff0020 },
};

#define CRU_DDR_HIWORD(label, offset, mask, reset_value) \
    { .name = label, .addr = offset, .reset = reset_value, \
      .ro = (uint16_t)~(mask), .pre_write = rockchip_cru_masked_write }

#define CRU_DDR_PLL_CON6(label, base) \
    { .name = label "_CON6", .addr = base + 0x18, .ro = 0x03ff, \
      .pre_write = rockchip_cru_masked_write, \
      .post_read = rockchip_cru_pll_lock }

/* DDR CRU registers: RK3588 TRM Part1, sections 2.5-2.8. */
static const RegisterAccessInfo rockchip_cru_ddr_regs[] = {
    CRU_DDR_HIWORD("D0APLL_CON0", 0x40000, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D0APLL_CON1", 0x40004, 0x21ff, 0),
    CRU_PLL_CON2("D0APLL", 0x40000),
    CRU_DDR_HIWORD("D0APLL_CON3", 0x4000c, 0xffff, 0),
    CRU_DDR_HIWORD("D0APLL_CON4", 0x40010, 0xc1f9, 0),
    CRU_DDR_HIWORD("D0APLL_CON5", 0x40014, 0x0001, 0),
    CRU_DDR_PLL_CON6("D0APLL", 0x40000),
    CRU_DDR_HIWORD("D0BPLL_CON0", 0x40020, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D0BPLL_CON1", 0x40024, 0x21ff, 0),
    CRU_PLL_CON2("D0BPLL", 0x40020),
    CRU_DDR_HIWORD("D0BPLL_CON3", 0x4002c, 0xffff, 0),
    CRU_DDR_HIWORD("D0BPLL_CON4", 0x40030, 0xc1f9, 0),
    CRU_DDR_HIWORD("D0BPLL_CON5", 0x40034, 0x0001, 0),
    CRU_DDR_PLL_CON6("D0BPLL", 0x40020),
    CRU_DDR_HIWORD("DDR0CRU_CLKSEL_CON00", 0x40300, 0x1, 0),
    CRU_DDR_HIWORD("DDR0CRU_GATE_CON00", 0x40800, 0x38, 0),
    CRU_DDR_HIWORD("DDR0CRU_SOFTRST_CON00", 0x40a00, 0x1e, 0x6),

    CRU_DDR_HIWORD("D1APLL_CON0", 0x44000, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D1APLL_CON1", 0x44004, 0x21ff, 0),
    CRU_PLL_CON2("D1APLL", 0x44000),
    CRU_DDR_HIWORD("D1APLL_CON3", 0x4400c, 0xffff, 0),
    CRU_DDR_HIWORD("D1APLL_CON4", 0x44010, 0xc1f9, 0),
    CRU_DDR_HIWORD("D1APLL_CON5", 0x44014, 0x0001, 0),
    CRU_DDR_PLL_CON6("D1APLL", 0x44000),
    CRU_DDR_HIWORD("D1BPLL_CON0", 0x44020, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D1BPLL_CON1", 0x44024, 0x21ff, 0),
    CRU_PLL_CON2("D1BPLL", 0x44020),
    CRU_DDR_HIWORD("D1BPLL_CON3", 0x4402c, 0xffff, 0),
    CRU_DDR_HIWORD("D1BPLL_CON4", 0x44030, 0xc1f9, 0),
    CRU_DDR_HIWORD("D1BPLL_CON5", 0x44034, 0x0001, 0),
    CRU_DDR_PLL_CON6("D1BPLL", 0x44020),
    CRU_DDR_HIWORD("DDR1CRU_CLKSEL_CON00", 0x44300, 0x1, 0),
    CRU_DDR_HIWORD("DDR1CRU_GATE_CON00", 0x44800, 0x38, 0),
    CRU_DDR_HIWORD("DDR1CRU_SOFTRST_CON00", 0x44a00, 0x1e, 0x6),

    CRU_DDR_HIWORD("D2APLL_CON0", 0x48000, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D2APLL_CON1", 0x48004, 0x21ff, 0),
    CRU_PLL_CON2("D2APLL", 0x48000),
    CRU_DDR_HIWORD("D2APLL_CON3", 0x4800c, 0xffff, 0),
    CRU_DDR_HIWORD("D2APLL_CON4", 0x48010, 0xc1f9, 0),
    CRU_DDR_HIWORD("D2APLL_CON5", 0x48014, 0x0001, 0),
    CRU_DDR_PLL_CON6("D2APLL", 0x48000),
    CRU_DDR_HIWORD("D2BPLL_CON0", 0x48020, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D2BPLL_CON1", 0x48024, 0x21ff, 0),
    CRU_PLL_CON2("D2BPLL", 0x48020),
    CRU_DDR_HIWORD("D2BPLL_CON3", 0x4802c, 0xffff, 0),
    CRU_DDR_HIWORD("D2BPLL_CON4", 0x48030, 0xc1f9, 0),
    CRU_DDR_HIWORD("D2BPLL_CON5", 0x48034, 0x0001, 0),
    CRU_DDR_PLL_CON6("D2BPLL", 0x48020),
    CRU_DDR_HIWORD("DDR2CRU_CLKSEL_CON00", 0x48300, 0x1, 0),
    CRU_DDR_HIWORD("DDR2CRU_GATE_CON00", 0x48800, 0x38, 0),
    CRU_DDR_HIWORD("DDR2CRU_SOFTRST_CON00", 0x48a00, 0x1e, 0x6),

    CRU_DDR_HIWORD("D3APLL_CON0", 0x4c000, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D3APLL_CON1", 0x4c004, 0x21ff, 0),
    CRU_PLL_CON2("D3APLL", 0x4c000),
    CRU_DDR_HIWORD("D3APLL_CON3", 0x4c00c, 0xffff, 0),
    CRU_DDR_HIWORD("D3APLL_CON4", 0x4c010, 0xc1f9, 0),
    CRU_DDR_HIWORD("D3APLL_CON5", 0x4c014, 0x0001, 0),
    CRU_DDR_PLL_CON6("D3APLL", 0x4c000),
    CRU_DDR_HIWORD("D3BPLL_CON0", 0x4c020, 0x83ff, 0x8000),
    CRU_DDR_HIWORD("D3BPLL_CON1", 0x4c024, 0x21ff, 0),
    CRU_PLL_CON2("D3BPLL", 0x4c020),
    CRU_DDR_HIWORD("D3BPLL_CON3", 0x4c02c, 0xffff, 0),
    CRU_DDR_HIWORD("D3BPLL_CON4", 0x4c030, 0xc1f9, 0),
    CRU_DDR_HIWORD("D3BPLL_CON5", 0x4c034, 0x0001, 0),
    CRU_DDR_PLL_CON6("D3BPLL", 0x4c020),
    CRU_DDR_HIWORD("DDR3CRU_CLKSEL_CON00", 0x4c300, 0x1, 0),
    CRU_DDR_HIWORD("DDR3CRU_GATE_CON00", 0x4c800, 0x38, 0),
    CRU_DDR_HIWORD("DDR3CRU_SOFTRST_CON00", 0x4ca00, 0x1e, 0x6),
};

G_STATIC_ASSERT(ARRAY_SIZE(rockchip_cru_ddr_regs) == ROCKCHIP_CRU_NUM_DDR_REGS);
G_STATIC_ASSERT(ARRAY_SIZE(rockchip_cru_pll_regs) +
                ARRAY_SIZE(rockchip_cru_clock_regs) +
                ARRAY_SIZE(rockchip_cru_ddr_regs) == ROCKCHIP_CRU_NUM_REGS);

static bool rockchip_cru_reserved(hwaddr offset)
{
    /* Unlisted offsets in DDR0..3 CRU are reserved. */
    if (offset >= 0x40000 && offset < 0x50000) {
        return true;
    }

    switch (offset) {
    case 0x50008: /* B0PLL */
    case 0x52028: /* B1PLL */
    case 0x58048: /* LPLL */
    case 0x001e8: /* NPLL */
        return true;
    default:
        return false;
    }
}

static uint64_t rockchip_cru_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipCRUState *s = opaque;
    RegisterInfo *reg = rockchip_cru_register(s, offset);

    if (reg) {
        return register_read(reg, UINT32_MAX, TYPE_ROCKCHIP_CRU, false);
    }
    if (rockchip_cru_reserved(offset)) {
        return 0;
    }
    if ((offset & (sizeof(uint32_t) - 1)) == 0 &&
        offset + sizeof(uint32_t) <= ROCKCHIP_CRU_SIZE) {
        return s->generic[offset / sizeof(uint32_t)];
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rockchip-cru: invalid read at 0x%" HWADDR_PRIx
                  " size %u\n", offset, size);
    return 0;
}

static void rockchip_cru_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RockchipCRUState *s = opaque;
    RegisterInfo *reg = rockchip_cru_register(s, offset);

    if (reg) {
        register_write(reg, value, UINT32_MAX, TYPE_ROCKCHIP_CRU, false);
        return;
    }
    if (rockchip_cru_reserved(offset)) {
        return;
    }
    if ((offset & (sizeof(uint32_t) - 1)) == 0 &&
        offset + sizeof(uint32_t) <= ROCKCHIP_CRU_SIZE) {
        uint32_t *slot = &s->generic[offset / sizeof(uint32_t)];
        uint16_t mask = value >> 16;

        /* Rockchip high-word write mask: only masked low bits are written. */
        *slot = (*slot & ~mask) | (value & mask);
        rockchip_cru_update_rknpu_reset(s, offset, *slot);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rockchip-cru: invalid write at 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 " size %u\n", offset, value, size);
}

static const MemoryRegionOps rockchip_cru_ops = {
    .read = rockchip_cru_read,
    .write = rockchip_cru_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rockchip_cru_reset(DeviceState *dev)
{
    RockchipCRUState *s = ROCKCHIP_CRU(dev);

    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    memset(s->generic, 0, sizeof(s->generic));
    for (unsigned int i = 0; i < ARRAY_SIZE(s->rknpu_reset); i++) {
        qemu_set_irq(s->rknpu_reset[i], 0);
    }
}

static void rockchip_cru_init(Object *obj)
{
    RockchipCRUState *s = ROCKCHIP_CRU(obj);

    /* Keep sparse CRU addresses in a compact RegisterInfo array. */
    for (unsigned int i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        const RegisterAccessInfo *access;
        unsigned int pll_regs = ARRAY_SIZE(rockchip_cru_pll_regs);
        unsigned int clock_regs = ARRAY_SIZE(rockchip_cru_clock_regs);

        if (i < pll_regs) {
            access = &rockchip_cru_pll_regs[i];
        } else if (i < pll_regs + clock_regs) {
            access = &rockchip_cru_clock_regs[i - pll_regs];
        } else {
            access = &rockchip_cru_ddr_regs[i - pll_regs - clock_regs];
        }
        s->regs_info[i] = (RegisterInfo) {
            .data = &s->regs[i],
            .data_size = sizeof(s->regs[i]),
            .access = access,
            .opaque = s,
        };
    }
    memory_region_init_io(&s->iomem, obj, &rockchip_cru_ops, s,
                          TYPE_ROCKCHIP_CRU, ROCKCHIP_CRU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), s->rknpu_reset, "rknpu-reset",
                             ARRAY_SIZE(s->rknpu_reset));
}

static void rockchip_cru_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, rockchip_cru_reset);
}

static const TypeInfo rockchip_cru_type_info = {
    .name = TYPE_ROCKCHIP_CRU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipCRUState),
    .instance_init = rockchip_cru_init,
    .class_init = rockchip_cru_class_init,
};

static void rockchip_cru_register_types(void)
{
    type_register_static(&rockchip_cru_type_info);
}

type_init(rockchip_cru_register_types)

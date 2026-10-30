/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RK3566 CRU/PMUCRU with RK3328-style PLLs and high-word register updates. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/clock.h"
#include "migration/vmstate.h"

#define TYPE_RK3566_CRU "rk3566-cru"
#define CRU_REG_SIZE 0x1000
#define CLKSEL_CON(n) (0x100 + 4 * (n))
#define CLKGATE_CON(n) (0x300 + 4 * (n))
#define SOFTRST_CON(n) (0x400 + 4 * (n))
OBJECT_DECLARE_SIMPLE_TYPE(RK3566CRUState, RK3566_CRU)

/*
 * The clock and reset control words live in a RegisterInfo bank.  The PLL
 * parameter words keep their address-class write behaviour through the
 * pre-write hook, and addresses outside the table keep the legacy masked
 * storage semantics through the wrapper callbacks below.
 */
struct RK3566CRUState {
    SysBusDevice parent_obj;

    uint32_t regs[CRU_REG_SIZE / 4];
    RegisterInfo regs_info[CRU_REG_SIZE / 4];
    RegisterInfoArray *reg_array;

    bool pmu;
    bool resetting;

    Clock *i2s1_tx;
    Clock *i2s1_rx;
    Clock *pdm;
    Clock *spi[4];
    Clock *uart[3];
};

static uint64_t pll_rate(RK3566CRUState *s, unsigned base, unsigned mode_shift)
{
    uint32_t mode = (s->regs[0xc0 / 4] >> mode_shift) & 3;
    uint32_t con0 = s->regs[base / 4];
    uint32_t con1 = s->regs[base / 4 + 1];
    unsigned refdiv = con1 & 0x3f;
    unsigned postdiv1 = (con0 >> 12) & 7;
    unsigned postdiv2 = (con1 >> 6) & 7;
    uint64_t feedback = (uint64_t)(con0 & 0xfff) << 24;

    if (mode != 1) {
        return mode == 0 ? 24000000 : 32768;
    }
    if (!refdiv || !postdiv1 || !postdiv2 || (con1 & BIT(13))) {
        return 0;
    }
    if (!(con1 & BIT(12))) {
        feedback += s->regs[base / 4 + 2] & 0xffffff;
    }
    return 24000000ULL * feedback /
           ((uint64_t)refdiv * postdiv1 * postdiv2 << 24);
}

static bool clock_gated(RK3566CRUState *s, unsigned index, unsigned bit)
{
    return (s->regs[CLKGATE_CON(index) / 4] & BIT(bit)) != 0;
}

static uint64_t divided_rate(RK3566CRUState *s, uint64_t parent,
                             unsigned index, unsigned shift, unsigned mask)
{
    return parent / (((s->regs[CLKSEL_CON(index) / 4] >> shift) & mask) + 1);
}

static uint64_t i2s1_rate(RK3566CRUState *s, bool receive,
                         uint64_t gpll, uint64_t cpll, uint64_t npll)
{
    unsigned index = receive ? 17 : 15;
    unsigned gate = receive ? 12 : 8;
    uint32_t select = s->regs[CLKSEL_CON(index) / 4];
    uint32_t fraction = s->regs[CLKSEL_CON(index + 1) / 4];
    uint64_t parents[] = { gpll, cpll, npll, 0 };
    unsigned mux = (select >> 10) & 3;
    uint64_t rate;

    if (clock_gated(s, 6, gate + 2)) {
        return 0;
    }
    if (mux == 3) {
        return 12000000;
    }
    if (mux == 2 || clock_gated(s, 6, gate)) {
        return 0;
    }
    rate = parents[(select >> 8) & 3] / ((select & 0x7f) + 1);
    if (mux == 1) {
        unsigned denominator = fraction & 0xffff;

        if (!denominator || clock_gated(s, 6, gate + 1)) {
            return 0;
        }
        rate = muldiv64(rate, fraction >> 16, denominator);
    }
    return rate;
}

static void cru_update_clocks(RK3566CRUState *s)
{
    uint64_t gpll, cpll, npll, gpll200;
    uint64_t pdm_parents[4], spi_parents[4];
    uint32_t select;

    if (s->pmu) {
        return;
    }
    gpll = pll_rate(s, 0x40, 6);
    cpll = pll_rate(s, 0x60, 4);
    npll = pll_rate(s, 0x80, 10);
    gpll200 = clock_gated(s, 35, 2) ? 0 : divided_rate(s, gpll, 76, 0, 31);
    clock_update_hz(s->i2s1_tx, i2s1_rate(s, false, gpll, cpll, npll));
    clock_update_hz(s->i2s1_rx, i2s1_rate(s, true, gpll, cpll, npll));
    pdm_parents[0] = clock_gated(s, 35, 1) ? 0 :
                     divided_rate(s, gpll, 75, 8, 31);
    pdm_parents[1] = clock_gated(s, 35, 9) ? 0 :
                     divided_rate(s, cpll, 79, 8, 31);
    pdm_parents[2] = gpll200;
    pdm_parents[3] = clock_gated(s, 35, 4) ? 0 :
                     divided_rate(s, gpll, 77, 0, 31);
    select = (s->regs[CLKSEL_CON(23) / 4] >> 8) & 3;
    clock_update_hz(s->pdm, clock_gated(s, 5, 15) ? 0 : pdm_parents[select]);
    spi_parents[0] = gpll200;
    spi_parents[1] = 24000000;
    spi_parents[2] = clock_gated(s, 35, 11) ? 0 :
                     divided_rate(s, cpll, 82, 0, 31);
    spi_parents[3] = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(s->spi); i++) {
        unsigned gate_index = i == 3 ? 31 : 30;
        unsigned gate_bit = i == 3 ? 1 : 11 + i * 2;

        select = (s->regs[CLKSEL_CON(72) / 4] >> (i * 2)) & 3;
        clock_update_hz(s->spi[i], clock_gated(s, gate_index, gate_bit) ? 0 :
                        spi_parents[select]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->uart); i++) {
        unsigned index = 52 + 2 * i;
        unsigned gate_index = i == 0 ? 27 : 28;
        unsigned gate_bit = i == 0 ? 13 : 1 + 4 * (i - 1);
        uint32_t config = s->regs[CLKSEL_CON(index) / 4];
        uint32_t fraction = s->regs[CLKSEL_CON(index + 1) / 4];
        uint64_t parents[] = { gpll, cpll, 480000000, 0 };
        unsigned mux = (config >> 12) & 3;
        uint64_t hz = 0;

        if (!clock_gated(s, gate_index, gate_bit + 2)) {
            if (mux == 2) {
                hz = 24000000;
            } else if (mux < 2 && !clock_gated(s, gate_index, gate_bit)) {
                hz = parents[(config >> 8) & 3] / ((config & 127) + 1);
                if (mux == 1) {
                    unsigned denominator = fraction & 0xffff;

                    hz = denominator &&
                         !clock_gated(s, gate_index, gate_bit + 1) ?
                         muldiv64(hz, fraction >> 16, denominator) : 0;
                }
            }
        }
        clock_update_hz(s->uart[i], hz);
    }
}

static bool pll_register(RK3566CRUState *s, hwaddr offset)
{
    return offset < (s->pmu ? 0x60 : 0xc0);
}

static bool fractional_register(RK3566CRUState *s, hwaddr offset)
{
    unsigned index;

    if (offset < CLKSEL_CON(0) || offset > CLKSEL_CON(84)) {
        return false;
    }
    index = (offset - CLKSEL_CON(0)) / 4;
    if (s->pmu) {
        return index == 1 || index == 5;
    }
    return (index >= 12 && index <= 26 && !(index & 1)) || index == 84 ||
           (index >= 53 && index <= 69 && (index & 1));
}

static uint64_t cru_pll_lock(RK3566CRUState *s, hwaddr offset, uint64_t value)
{
    uint32_t con0 = s->regs[(offset - 4) / 4];
    bool configured = (con0 & 0xfff) && ((con0 >> 12) & 7) &&
                      (value & 0x3f) && ((value >> 6) & 7);

    value &= ~BIT(10);
    if (configured && !(value & BIT(13))) {
        value |= BIT(10);
    }
    return value;
}

static uint64_t cru_pre_write(RegisterInfo *reg, uint64_t val)
{
    RK3566CRUState *s = RK3566_CRU(reg->opaque);
    hwaddr offset = reg->access->addr;
    uint32_t *data = reg->data;
    uint32_t mask;

    if (pll_register(s, offset) && (offset & 0x1f) == 8) {
        return val & 0xffffff;
    }
    if (fractional_register(s, offset)) {
        return val;
    }
    mask = val >> 16;
    return (*data & ~mask) | (val & mask);
}

static uint64_t cru_pll_con1_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566CRUState *s = RK3566_CRU(reg->opaque);

    return cru_pll_lock(s, reg->access->addr, val);
}

static void cru_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566CRUState *s = RK3566_CRU(reg->opaque);

    if (!s->resetting) {
        cru_update_clocks(s);
    }
}

#define CRU_REG(_label, _addr) \
    { .name = _label, .addr = (_addr), \
      .pre_write = cru_pre_write, .post_write = cru_post_write }

#define CRU_REG_RESET(_label, _addr, _value) \
    { .name = _label, .addr = (_addr), .reset = (_value), \
      .pre_write = cru_pre_write, .post_write = cru_post_write }

#define CRU_CON1_REG(_label, _addr, _value) \
    { .name = _label, .addr = (_addr), .reset = (_value), \
      .pre_write = cru_pre_write, .post_read = cru_pll_con1_post_read, \
      .post_write = cru_post_write }

/*
 * The PLL block uses CON0, CON1 and CON2 words.  GPLL at 0x40, CPLL at 0x60
 * and NPLL at 0x80 drive the clock tree; the other parameter words are
 * reset-programmed storage.  CON2 keeps the high-word mask and CON1 derives
 * the LOCK bit on read.
 */
static const RegisterAccessInfo cru_regs_info[] = {
    CRU_REG_RESET("PLL0_CON0", 0x00, 0x102e),
    CRU_CON1_REG("PLL0_CON1", 0x04, 0x1041),
    CRU_REG("PLL0_CON2", 0x08),
    CRU_REG_RESET("PLL1_CON0", 0x20, 0x1041),
    CRU_CON1_REG("PLL1_CON1", 0x24, 0x1041),
    CRU_REG("PLL1_CON2", 0x28),
    CRU_REG_RESET("GPLL_CON0", 0x40, 0x1032),
    CRU_CON1_REG("GPLL_CON1", 0x44, 0x1041),
    CRU_REG("GPLL_CON2", 0x48),
    CRU_REG_RESET("CPLL_CON0", 0x60, 0x107d),
    CRU_CON1_REG("CPLL_CON1", 0x64, 0x1043),
    CRU_REG("CPLL_CON2", 0x68),
    CRU_REG_RESET("NPLL_CON0", 0x80, 0x1032),
    CRU_CON1_REG("NPLL_CON1", 0x84, 0x1041),
    CRU_REG("NPLL_CON2", 0x88),
    CRU_REG_RESET("PLL5_CON0", 0xa0, 0x1032),
    CRU_CON1_REG("PLL5_CON1", 0xa4, 0x1041),
    CRU_REG("PLL5_CON2", 0xa8),
    CRU_REG_RESET("MODE", 0xc0, 0x1455),
    /* Clock selectors; the write class is decoded by cru_pre_write(). */
    CRU_REG("CLKSEL_CON0", CLKSEL_CON(0)),
    CRU_REG("CLKSEL_CON1", CLKSEL_CON(1)),
    CRU_REG("CLKSEL_CON2", CLKSEL_CON(2)),
    CRU_REG("CLKSEL_CON3", CLKSEL_CON(3)),
    CRU_REG("CLKSEL_CON4", CLKSEL_CON(4)),
    CRU_REG("CLKSEL_CON5", CLKSEL_CON(5)),
    CRU_REG("CLKSEL_CON6", CLKSEL_CON(6)),
    CRU_REG("CLKSEL_CON7", CLKSEL_CON(7)),
    CRU_REG("CLKSEL_CON8", CLKSEL_CON(8)),
    CRU_REG("CLKSEL_CON9", CLKSEL_CON(9)),
    CRU_REG("CLKSEL_CON10", CLKSEL_CON(10)),
    CRU_REG("CLKSEL_CON11", CLKSEL_CON(11)),
    CRU_REG("CLKSEL_CON12", CLKSEL_CON(12)),
    CRU_REG("CLKSEL_CON13", CLKSEL_CON(13)),
    CRU_REG("CLKSEL_CON14", CLKSEL_CON(14)),
    CRU_REG("CLKSEL_CON15", CLKSEL_CON(15)),
    CRU_REG("CLKSEL_CON16", CLKSEL_CON(16)),
    CRU_REG("CLKSEL_CON17", CLKSEL_CON(17)),
    CRU_REG("CLKSEL_CON18", CLKSEL_CON(18)),
    CRU_REG("CLKSEL_CON19", CLKSEL_CON(19)),
    CRU_REG("CLKSEL_CON20", CLKSEL_CON(20)),
    CRU_REG("CLKSEL_CON21", CLKSEL_CON(21)),
    CRU_REG("CLKSEL_CON22", CLKSEL_CON(22)),
    CRU_REG("CLKSEL_CON23", CLKSEL_CON(23)),
    CRU_REG("CLKSEL_CON24", CLKSEL_CON(24)),
    CRU_REG("CLKSEL_CON25", CLKSEL_CON(25)),
    CRU_REG("CLKSEL_CON26", CLKSEL_CON(26)),
    CRU_REG("CLKSEL_CON27", CLKSEL_CON(27)),
    CRU_REG("CLKSEL_CON28", CLKSEL_CON(28)),
    CRU_REG("CLKSEL_CON29", CLKSEL_CON(29)),
    CRU_REG("CLKSEL_CON30", CLKSEL_CON(30)),
    CRU_REG("CLKSEL_CON31", CLKSEL_CON(31)),
    CRU_REG("CLKSEL_CON32", CLKSEL_CON(32)),
    CRU_REG("CLKSEL_CON33", CLKSEL_CON(33)),
    CRU_REG("CLKSEL_CON34", CLKSEL_CON(34)),
    CRU_REG("CLKSEL_CON35", CLKSEL_CON(35)),
    CRU_REG("CLKSEL_CON36", CLKSEL_CON(36)),
    CRU_REG("CLKSEL_CON37", CLKSEL_CON(37)),
    CRU_REG("CLKSEL_CON38", CLKSEL_CON(38)),
    CRU_REG("CLKSEL_CON39", CLKSEL_CON(39)),
    CRU_REG("CLKSEL_CON40", CLKSEL_CON(40)),
    CRU_REG("CLKSEL_CON41", CLKSEL_CON(41)),
    CRU_REG("CLKSEL_CON42", CLKSEL_CON(42)),
    CRU_REG("CLKSEL_CON43", CLKSEL_CON(43)),
    CRU_REG("CLKSEL_CON44", CLKSEL_CON(44)),
    CRU_REG("CLKSEL_CON45", CLKSEL_CON(45)),
    CRU_REG("CLKSEL_CON46", CLKSEL_CON(46)),
    CRU_REG("CLKSEL_CON47", CLKSEL_CON(47)),
    CRU_REG("CLKSEL_CON48", CLKSEL_CON(48)),
    CRU_REG("CLKSEL_CON49", CLKSEL_CON(49)),
    CRU_REG("CLKSEL_CON50", CLKSEL_CON(50)),
    CRU_REG("CLKSEL_CON51", CLKSEL_CON(51)),
    CRU_REG("CLKSEL_CON52", CLKSEL_CON(52)),
    CRU_REG("CLKSEL_CON53", CLKSEL_CON(53)),
    CRU_REG("CLKSEL_CON54", CLKSEL_CON(54)),
    CRU_REG("CLKSEL_CON55", CLKSEL_CON(55)),
    CRU_REG("CLKSEL_CON56", CLKSEL_CON(56)),
    CRU_REG("CLKSEL_CON57", CLKSEL_CON(57)),
    CRU_REG("CLKSEL_CON58", CLKSEL_CON(58)),
    CRU_REG("CLKSEL_CON59", CLKSEL_CON(59)),
    CRU_REG("CLKSEL_CON60", CLKSEL_CON(60)),
    CRU_REG("CLKSEL_CON61", CLKSEL_CON(61)),
    CRU_REG("CLKSEL_CON62", CLKSEL_CON(62)),
    CRU_REG("CLKSEL_CON63", CLKSEL_CON(63)),
    CRU_REG("CLKSEL_CON64", CLKSEL_CON(64)),
    CRU_REG("CLKSEL_CON65", CLKSEL_CON(65)),
    CRU_REG("CLKSEL_CON66", CLKSEL_CON(66)),
    CRU_REG("CLKSEL_CON67", CLKSEL_CON(67)),
    CRU_REG("CLKSEL_CON68", CLKSEL_CON(68)),
    CRU_REG("CLKSEL_CON69", CLKSEL_CON(69)),
    CRU_REG("CLKSEL_CON70", CLKSEL_CON(70)),
    CRU_REG("CLKSEL_CON71", CLKSEL_CON(71)),
    CRU_REG("CLKSEL_CON72", CLKSEL_CON(72)),
    CRU_REG("CLKSEL_CON73", CLKSEL_CON(73)),
    CRU_REG("CLKSEL_CON74", CLKSEL_CON(74)),
    CRU_REG_RESET("CLKSEL_CON75", CLKSEL_CON(75), 2 | (3 << 8)),
    CRU_REG_RESET("CLKSEL_CON76", CLKSEL_CON(76), 5 | (7 << 8)),
    CRU_REG_RESET("CLKSEL_CON77", CLKSEL_CON(77), 11 | (15 << 8)),
    CRU_REG_RESET("CLKSEL_CON78", CLKSEL_CON(78), 59 | (1 << 8)),
    CRU_REG_RESET("CLKSEL_CON79", CLKSEL_CON(79), 2 | (3 << 8)),
    CRU_REG_RESET("CLKSEL_CON80", CLKSEL_CON(80), 7 | (15 << 8)),
    CRU_REG_RESET("CLKSEL_CON81", CLKSEL_CON(81), 19 | (39 << 8)),
    CRU_REG_RESET("CLKSEL_CON82", CLKSEL_CON(82), 9 | (31 << 8)),
    CRU_REG("CLKSEL_CON83", CLKSEL_CON(83)),
    CRU_REG("CLKSEL_CON84", CLKSEL_CON(84)),
    /* Clock gates and software resets are masked-storage words. */
    CRU_REG("CLKGATE_CON0", CLKGATE_CON(0)),
    CRU_REG("CLKGATE_CON1", CLKGATE_CON(1)),
    CRU_REG("CLKGATE_CON2", CLKGATE_CON(2)),
    CRU_REG("CLKGATE_CON3", CLKGATE_CON(3)),
    CRU_REG("CLKGATE_CON4", CLKGATE_CON(4)),
    CRU_REG("CLKGATE_CON5", CLKGATE_CON(5)),
    CRU_REG("CLKGATE_CON6", CLKGATE_CON(6)),
    CRU_REG("CLKGATE_CON7", CLKGATE_CON(7)),
    CRU_REG("CLKGATE_CON8", CLKGATE_CON(8)),
    CRU_REG("CLKGATE_CON9", CLKGATE_CON(9)),
    CRU_REG("CLKGATE_CON10", CLKGATE_CON(10)),
    CRU_REG("CLKGATE_CON11", CLKGATE_CON(11)),
    CRU_REG("CLKGATE_CON12", CLKGATE_CON(12)),
    CRU_REG("CLKGATE_CON13", CLKGATE_CON(13)),
    CRU_REG("CLKGATE_CON14", CLKGATE_CON(14)),
    CRU_REG("CLKGATE_CON15", CLKGATE_CON(15)),
    CRU_REG("CLKGATE_CON16", CLKGATE_CON(16)),
    CRU_REG("CLKGATE_CON17", CLKGATE_CON(17)),
    CRU_REG("CLKGATE_CON18", CLKGATE_CON(18)),
    CRU_REG("CLKGATE_CON19", CLKGATE_CON(19)),
    CRU_REG("CLKGATE_CON20", CLKGATE_CON(20)),
    CRU_REG("CLKGATE_CON21", CLKGATE_CON(21)),
    CRU_REG("CLKGATE_CON22", CLKGATE_CON(22)),
    CRU_REG("CLKGATE_CON23", CLKGATE_CON(23)),
    CRU_REG("CLKGATE_CON24", CLKGATE_CON(24)),
    CRU_REG("CLKGATE_CON25", CLKGATE_CON(25)),
    CRU_REG("CLKGATE_CON26", CLKGATE_CON(26)),
    CRU_REG("CLKGATE_CON27", CLKGATE_CON(27)),
    CRU_REG("CLKGATE_CON28", CLKGATE_CON(28)),
    CRU_REG("CLKGATE_CON29", CLKGATE_CON(29)),
    CRU_REG("CLKGATE_CON30", CLKGATE_CON(30)),
    CRU_REG("CLKGATE_CON31", CLKGATE_CON(31)),
    CRU_REG("CLKGATE_CON32", CLKGATE_CON(32)),
    CRU_REG("CLKGATE_CON33", CLKGATE_CON(33)),
    CRU_REG("CLKGATE_CON34", CLKGATE_CON(34)),
    CRU_REG("CLKGATE_CON35", CLKGATE_CON(35)),
    CRU_REG("SOFTRST_CON0", SOFTRST_CON(0)),
    CRU_REG("SOFTRST_CON1", SOFTRST_CON(1)),
    CRU_REG("SOFTRST_CON2", SOFTRST_CON(2)),
    CRU_REG("SOFTRST_CON3", SOFTRST_CON(3)),
    CRU_REG("SOFTRST_CON4", SOFTRST_CON(4)),
    CRU_REG("SOFTRST_CON5", SOFTRST_CON(5)),
    CRU_REG("SOFTRST_CON6", SOFTRST_CON(6)),
    CRU_REG("SOFTRST_CON7", SOFTRST_CON(7)),
};

/*
 * The PMUCRU variant has no clock outputs.  Its table covers the PLL block,
 * the mode word and the two fractional selector words; the rest of the PMU
 * window keeps the wrapper fallback.
 */
static const RegisterAccessInfo pmucru_regs_info[] = {
    CRU_REG_RESET("PLL0_CON0", 0x00, 0x1032),
    CRU_CON1_REG("PLL0_CON1", 0x04, 0x1041),
    CRU_REG("PLL0_CON2", 0x08),
    CRU_REG_RESET("PLL1_CON0", 0x20, 0x1032),
    CRU_CON1_REG("PLL1_CON1", 0x24, 0x1041),
    CRU_REG("PLL1_CON2", 0x28),
    CRU_REG_RESET("PLL2_CON0", 0x40, 0x1032),
    CRU_CON1_REG("PLL2_CON1", 0x44, 0x1041),
    CRU_REG("PLL2_CON2", 0x48),
    CRU_REG_RESET("MODE", 0x80, 0x5),
    CRU_REG("CLKSEL_CON1", CLKSEL_CON(1)),
    CRU_REG("CLKSEL_CON5", CLKSEL_CON(5)),
};

static const RegisterAccessInfo *cru_reg_table(RK3566CRUState *s,
                                               unsigned *count)
{
    if (s->pmu) {
        *count = ARRAY_SIZE(pmucru_regs_info);
        return pmucru_regs_info;
    }
    *count = ARRAY_SIZE(cru_regs_info);
    return cru_regs_info;
}

static uint64_t cru_read(void *opaque, hwaddr offset, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566CRUState *s = RK3566_CRU(register_array_get_owner(reg_array));
    uint32_t value;

    if (s->regs_info[offset / 4].access) {
        return register_read_memory(reg_array, offset, size);
    }
    value = s->regs[offset / 4];
    if (pll_register(s, offset) && (offset & 0x1f) == 4) {
        value = cru_pll_lock(s, offset, value);
    }
    return value;
}

static void cru_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566CRUState *s = RK3566_CRU(register_array_get_owner(reg_array));
    uint32_t *reg = &s->regs[offset / 4];

    if (s->regs_info[offset / 4].access) {
        register_write_memory(reg_array, offset, value, size);
        return;
    }
    if (pll_register(s, offset) && (offset & 0x1f) == 8) {
        *reg = value & 0xffffff;
    } else if (fractional_register(s, offset)) {
        *reg = value;
    } else {
        uint32_t mask = value >> 16;

        *reg = (*reg & ~mask) | (value & mask);
    }
    cru_update_clocks(s);
}

static const MemoryRegionOps cru_ops = {
    .read = cru_read,
    .write = cru_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void cru_reset(DeviceState *dev)
{
    RK3566CRUState *s = RK3566_CRU(dev);
    unsigned count;
    const RegisterAccessInfo *table = cru_reg_table(s, &count);

    s->resetting = true;
    memset(s->regs, 0, sizeof(s->regs));
    for (unsigned i = 0; i < count; i++) {
        register_reset(&s->regs_info[table[i].addr / 4]);
    }
    s->resetting = false;
    cru_update_clocks(s);
}

static int cru_post_load(void *opaque, int version_id)
{
    cru_update_clocks(opaque);
    return 0;
}

static void cru_init(Object *obj)
{
    RK3566CRUState *s = RK3566_CRU(obj);

    s->i2s1_tx = qdev_init_clock_out(DEVICE(s), "mclk-i2s1");
    s->i2s1_rx = qdev_init_clock_out(DEVICE(s), "mclk-i2s1-rx");
    s->pdm = qdev_init_clock_out(DEVICE(s), "mclk-pdm");
    for (unsigned i = 0; i < ARRAY_SIZE(s->spi); i++) {
        g_autofree char *name = g_strdup_printf("sclk-spi%u", i);

        s->spi[i] = qdev_init_clock_out(DEVICE(s), name);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->uart); i++) {
        g_autofree char *name = g_strdup_printf("sclk-uart%u", i + 1);

        s->uart[i] = qdev_init_clock_out(DEVICE(s), name);
    }
}

static void cru_realize(DeviceState *dev, Error **errp)
{
    RK3566CRUState *s = RK3566_CRU(dev);
    unsigned count;
    const RegisterAccessInfo *table = cru_reg_table(s, &count);

    s->reg_array = register_init_block32(dev, table, count, s->regs_info,
                                         s->regs, &cru_ops, false,
                                         CRU_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->reg_array->mem);
}

static const Property cru_properties[] = {
    DEFINE_PROP_BOOL("pmu", RK3566CRUState, pmu, false),
};

static const VMStateDescription vmstate_cru = {
    .name = TYPE_RK3566_CRU,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = cru_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3566CRUState, CRU_REG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void cru_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = cru_realize;
    dc->vmsd = &vmstate_cru;
    device_class_set_legacy_reset(dc, cru_reset);
    device_class_set_props(dc, cru_properties);
}

static const TypeInfo cru_info = {
    .name = TYPE_RK3566_CRU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566CRUState),
    .instance_init = cru_init,
    .class_init = cru_class_init,
};

static void cru_register(void)
{
    type_register_static(&cru_info);
}
type_init(cru_register)

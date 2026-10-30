/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RK3566 CRU/PMUCRU with RK3328-style PLLs and high-word register updates. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/clock.h"
#include "migration/vmstate.h"

#define TYPE_RK3566_CRU "rk3566-cru"
#define CLKSEL_CON(n) (0x100 + 4 * (n))
#define CLKGATE_CON(n) (0x300 + 4 * (n))
OBJECT_DECLARE_SIMPLE_TYPE(RK3566CRUState, RK3566_CRU)

struct RK3566CRUState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    uint32_t regs[1024];
    bool pmu;
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

static uint64_t cru_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3566CRUState *s = opaque;
    uint32_t value = s->regs[offset / 4];
    if (pll_register(s, offset) && (offset & 0x1f) == 4) {
        uint32_t con0 = s->regs[(offset - 4) / 4];
        bool configured = (con0 & 0xfff) && ((con0 >> 12) & 7) &&
                          (value & 0x3f) && ((value >> 6) & 7);
        value &= ~BIT(10);
        if (configured && !(value & BIT(13))) {
            value |= BIT(10);
        }
    }
    return value;
}

static void cru_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RK3566CRUState *s = opaque;
    uint32_t *reg = &s->regs[offset / 4];
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
    memset(s->regs, 0, sizeof(s->regs));
    for (unsigned base = 0; base < (s->pmu ? 0x60 : 0xc0); base += 0x20) {
        s->regs[base / 4] = 50 | BIT(12);
        s->regs[base / 4 + 1] = 1 | BIT(6) | BIT(12);
    }
    s->regs[(s->pmu ? 0x80 : 0xc0) / 4] = s->pmu ? 0x5 : 0x1455;
    if (!s->pmu) {
        /* Direct-kernel boot uses the loader's PLL and bus source setup. */
        s->regs[0] = 46 | BIT(12);
        s->regs[0x20 / 4] = 65 | BIT(12);
        s->regs[0x60 / 4] = 125 | BIT(12);
        s->regs[0x64 / 4] = 3 | BIT(6) | BIT(12);
        s->regs[CLKSEL_CON(75) / 4] = 2 | (3 << 8);
        s->regs[CLKSEL_CON(76) / 4] = 5 | (7 << 8);
        s->regs[CLKSEL_CON(77) / 4] = 11 | (15 << 8);
        s->regs[CLKSEL_CON(78) / 4] = 59 | (1 << 8);
        s->regs[CLKSEL_CON(79) / 4] = 2 | (3 << 8);
        s->regs[CLKSEL_CON(80) / 4] = 7 | (15 << 8);
        s->regs[CLKSEL_CON(81) / 4] = 19 | (39 << 8);
        s->regs[CLKSEL_CON(82) / 4] = 9 | (31 << 8);
    }
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
    memory_region_init_io(&s->io, OBJECT(s), &cru_ops, s, "rk3566-cru", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io);
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
        VMSTATE_UINT32_ARRAY(regs, RK3566CRUState, 1024),
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

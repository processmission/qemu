/*
 * STM32G474 system configuration controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/misc/stm32g474_syscfg.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define STM32G474_SYSCFG_NUM_REGS 10

typedef enum Stm32g474SyscfgKeyPhase {
    STM32G474_SYSCFG_KEY_LOCKED,
    STM32G474_SYSCFG_KEY_HAVE_CA,
    STM32G474_SYSCFG_KEY_UNLOCKED,
} Stm32g474SyscfgKeyPhase;

struct Stm32g474SyscfgState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_SYSCFG_NUM_REGS];
    uint32_t regs[STM32G474_SYSCFG_NUM_REGS];

    Clock *clk;
    qemu_irq exti_out[STM32G474_SYSCFG_NUM_LINES];

    uint16_t gpio_levels[STM32G474_SYSCFG_NUM_PORTS];
    uint16_t output_cache;
    uint32_t raw_exticr;
    uint8_t key_phase;
    bool peripheral_reset_asserted;
    bool resetting;
    bool output_cache_valid;
    bool raw_exticr_valid;
};

REG32(MEMRMP, 0x00)
    FIELD(MEMRMP, MEM_MODE, 0, 3)
    FIELD(MEMRMP, FB_MODE, 8, 1)
REG32(CFGR1, 0x04)
    FIELD(CFGR1, BOOSTEN, 8, 1)
    FIELD(CFGR1, ANASWVDD, 9, 1)
    FIELD(CFGR1, I2C_PB6_FMP, 16, 1)
    FIELD(CFGR1, I2C_PB7_FMP, 17, 1)
    FIELD(CFGR1, I2C_PB8_FMP, 18, 1)
    FIELD(CFGR1, I2C_PB9_FMP, 19, 1)
    FIELD(CFGR1, I2C1_FMP, 20, 1)
    FIELD(CFGR1, I2C2_FMP, 21, 1)
    FIELD(CFGR1, I2C3_FMP, 22, 1)
    FIELD(CFGR1, I2C4_FMP, 23, 1)
    FIELD(CFGR1, FPU_IE, 26, 6)
REG32(EXTICR1, 0x08)
    FIELD(EXTICR1, EXTI0, 0, 4)
    FIELD(EXTICR1, EXTI1, 4, 4)
    FIELD(EXTICR1, EXTI2, 8, 4)
    FIELD(EXTICR1, EXTI3, 12, 4)
REG32(EXTICR2, 0x0c)
    FIELD(EXTICR2, EXTI4, 0, 4)
    FIELD(EXTICR2, EXTI5, 4, 4)
    FIELD(EXTICR2, EXTI6, 8, 4)
    FIELD(EXTICR2, EXTI7, 12, 4)
REG32(EXTICR3, 0x10)
    FIELD(EXTICR3, EXTI8, 0, 4)
    FIELD(EXTICR3, EXTI9, 4, 4)
    FIELD(EXTICR3, EXTI10, 8, 4)
    FIELD(EXTICR3, EXTI11, 12, 4)
REG32(EXTICR4, 0x14)
    FIELD(EXTICR4, EXTI12, 0, 4)
    FIELD(EXTICR4, EXTI13, 4, 4)
    FIELD(EXTICR4, EXTI14, 8, 4)
    FIELD(EXTICR4, EXTI15, 12, 4)
REG32(SCSR, 0x18)
    FIELD(SCSR, CCMER, 0, 1)
    FIELD(SCSR, CCMBSY, 1, 1)
REG32(CFGR2, 0x1c)
    FIELD(CFGR2, LOCK, 0, 4)
    FIELD(CFGR2, SPF, 8, 1)
REG32(SWPR, 0x20)
    FIELD(SWPR, PAGE, 0, 32)
REG32(SKR, 0x24)
    FIELD(SKR, KEY, 0, 8)

#define SYSCFG_MEMRMP_RESERVED 0xfffffef8U
#define SYSCFG_MEMRMP_UNIMPLEMENTED 0x00000107U
#define SYSCFG_CFGR1_RESERVED 0x0300fcffU
#define SYSCFG_CFGR1_UNIMPLEMENTED 0xfcff0300U
#define SYSCFG_EXTICR_RESERVED 0xffff8888U
#define SYSCFG_SCSR_RESERVED 0xfffffffcU
#define SYSCFG_CFGR2_RESERVED 0xfffffef0U

#define SYSCFG_EXTICR_FIELDS_PER_REG 4
#define SYSCFG_EXTICR_FIELD_WIDTH 4
#define SYSCFG_GPIOG_PORT 6
#define SYSCFG_GPIOG_LAST_LINE 10

#define SYSCFG_SKR_KEY1 0xca
#define SYSCFG_SKR_KEY2 0x53

static bool stm32g474_syscfg_selector_valid(unsigned int line,
                                            unsigned int selector)
{
    return selector < SYSCFG_GPIOG_PORT ||
           (selector == SYSCFG_GPIOG_PORT &&
            line <= SYSCFG_GPIOG_LAST_LINE);
}

static unsigned int
stm32g474_syscfg_selector(Stm32g474SyscfgState *s, unsigned int line)
{
    unsigned int reg = R_EXTICR1 + line / SYSCFG_EXTICR_FIELDS_PER_REG;
    unsigned int shift =
        line % SYSCFG_EXTICR_FIELDS_PER_REG * SYSCFG_EXTICR_FIELD_WIDTH;

    return extract32(s->regs[reg], shift, 3);
}

static void stm32g474_syscfg_route_line(Stm32g474SyscfgState *s,
                                        unsigned int line, bool force)
{
    unsigned int port = stm32g474_syscfg_selector(s, line);
    uint16_t bit = BIT(line);
    bool level = (s->gpio_levels[port] & bit) != 0;

    if (force || !s->output_cache_valid ||
        ((s->output_cache & bit) != 0) != level) {
        qemu_set_irq(s->exti_out[line], level);
    }

    if (level) {
        s->output_cache |= bit;
    } else {
        s->output_cache &= ~bit;
    }
}

static void stm32g474_syscfg_route_all(Stm32g474SyscfgState *s, bool force)
{
    for (unsigned int line = 0; line < STM32G474_SYSCFG_NUM_LINES; line++) {
        stm32g474_syscfg_route_line(s, line, force);
    }
    s->output_cache_valid = true;
}

static uint64_t stm32g474_syscfg_memrmp_pre_write(RegisterInfo *reg,
                                                   uint64_t val)
{
    return 0;
}

static uint64_t stm32g474_syscfg_exticr_pre_write(RegisterInfo *reg,
                                                   uint64_t val)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(reg->opaque);
    unsigned int reg_index = reg->access->addr / sizeof(uint32_t);
    unsigned int first_line =
        (reg->access->addr - A_EXTICR1) / sizeof(uint32_t) *
        SYSCFG_EXTICR_FIELDS_PER_REG;
    uint32_t old = s->regs[reg_index];
    uint32_t raw = s->raw_exticr_valid ? s->raw_exticr : val;
    uint32_t result = old;

    for (unsigned int field = 0;
         field < SYSCFG_EXTICR_FIELDS_PER_REG; field++) {
        unsigned int shift = field * SYSCFG_EXTICR_FIELD_WIDTH;
        unsigned int selector =
            extract32(raw, shift, SYSCFG_EXTICR_FIELD_WIDTH);

        if (stm32g474_syscfg_selector_valid(first_line + field,
                                             selector)) {
            result = deposit32(result, shift, 3, selector);
        } else if (selector <= SYSCFG_GPIOG_PORT + 1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          TYPE_STM32G474_SYSCFG
                          ": rejected GPIO selector %u for EXTI line %u\n",
                          selector, first_line + field);
        }
    }

    return result;
}

static void stm32g474_syscfg_exticr_post_write(RegisterInfo *reg,
                                                uint64_t val)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(reg->opaque);
    unsigned int first_line =
        (reg->access->addr - A_EXTICR1) / sizeof(uint32_t) *
        SYSCFG_EXTICR_FIELDS_PER_REG;

    if (s->resetting) {
        return;
    }

    for (unsigned int line = first_line;
         line < first_line + SYSCFG_EXTICR_FIELDS_PER_REG; line++) {
        stm32g474_syscfg_route_line(s, line, false);
    }
    s->output_cache_valid = true;
}

static uint64_t stm32g474_syscfg_scsr_pre_write(RegisterInfo *reg,
                                                 uint64_t val)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(reg->opaque);
    uint32_t old = s->regs[R_SCSR];

    if ((old & R_SCSR_CCMER_MASK) ||
        ((val & R_SCSR_CCMER_MASK) &&
         s->key_phase == STM32G474_SYSCFG_KEY_UNLOCKED)) {
        return R_SCSR_CCMER_MASK;
    }
    return 0;
}

static uint64_t stm32g474_syscfg_cfgr2_pre_write(RegisterInfo *reg,
                                                  uint64_t val)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(reg->opaque);

    return val | (s->regs[R_CFGR2] & R_CFGR2_LOCK_MASK);
}

static uint64_t stm32g474_syscfg_swpr_pre_write(RegisterInfo *reg,
                                                 uint64_t val)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(reg->opaque);

    return val | s->regs[R_SWPR];
}

static uint64_t stm32g474_syscfg_skr_pre_write(RegisterInfo *reg,
                                                uint64_t val)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(reg->opaque);
    uint8_t key = FIELD_EX32(val, SKR, KEY);

    if (key == SYSCFG_SKR_KEY1) {
        s->key_phase = STM32G474_SYSCFG_KEY_HAVE_CA;
    } else if (key == SYSCFG_SKR_KEY2 &&
               s->key_phase == STM32G474_SYSCFG_KEY_HAVE_CA) {
        s->key_phase = STM32G474_SYSCFG_KEY_UNLOCKED;
    } else {
        s->key_phase = STM32G474_SYSCFG_KEY_LOCKED;
    }

    return 0;
}

static uint64_t stm32g474_syscfg_skr_post_read(RegisterInfo *reg,
                                                uint64_t val)
{
    return 0;
}

static const RegisterAccessInfo stm32g474_syscfg_regs_info[] = {
    {
        .name = "MEMRMP",
        .addr = A_MEMRMP,
        .rsvd = SYSCFG_MEMRMP_RESERVED,
        .unimp = SYSCFG_MEMRMP_UNIMPLEMENTED,
        .pre_write = stm32g474_syscfg_memrmp_pre_write,
    }, {
        .name = "CFGR1",
        .addr = A_CFGR1,
        .reset = 0x7c000000,
        .rsvd = SYSCFG_CFGR1_RESERVED,
        .unimp = SYSCFG_CFGR1_UNIMPLEMENTED,
    }, {
        .name = "EXTICR1",
        .addr = A_EXTICR1,
        .rsvd = SYSCFG_EXTICR_RESERVED,
        .pre_write = stm32g474_syscfg_exticr_pre_write,
        .post_write = stm32g474_syscfg_exticr_post_write,
    }, {
        .name = "EXTICR2",
        .addr = A_EXTICR2,
        .rsvd = SYSCFG_EXTICR_RESERVED,
        .pre_write = stm32g474_syscfg_exticr_pre_write,
        .post_write = stm32g474_syscfg_exticr_post_write,
    }, {
        .name = "EXTICR3",
        .addr = A_EXTICR3,
        .rsvd = SYSCFG_EXTICR_RESERVED,
        .pre_write = stm32g474_syscfg_exticr_pre_write,
        .post_write = stm32g474_syscfg_exticr_post_write,
    }, {
        .name = "EXTICR4",
        .addr = A_EXTICR4,
        .rsvd = SYSCFG_EXTICR_RESERVED,
        .pre_write = stm32g474_syscfg_exticr_pre_write,
        .post_write = stm32g474_syscfg_exticr_post_write,
    }, {
        .name = "SCSR",
        .addr = A_SCSR,
        .rsvd = SYSCFG_SCSR_RESERVED,
        .ro = R_SCSR_CCMBSY_MASK,
        .unimp = R_SCSR_CCMER_MASK,
        .pre_write = stm32g474_syscfg_scsr_pre_write,
    }, {
        .name = "CFGR2",
        .addr = A_CFGR2,
        .rsvd = SYSCFG_CFGR2_RESERVED,
        .w1c = R_CFGR2_SPF_MASK,
        .unimp = R_CFGR2_LOCK_MASK,
        .pre_write = stm32g474_syscfg_cfgr2_pre_write,
    }, {
        .name = "SWPR",
        .addr = A_SWPR,
        .unimp = UINT32_MAX,
        .pre_write = stm32g474_syscfg_swpr_pre_write,
    }, {
        .name = "SKR",
        .addr = A_SKR,
        .rsvd = UINT32_MAX & ~R_SKR_KEY_MASK,
        .pre_write = stm32g474_syscfg_skr_pre_write,
        .post_read = stm32g474_syscfg_skr_post_read,
    },
};

G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_syscfg_regs_info) ==
                STM32G474_SYSCFG_NUM_REGS);

static void stm32g474_syscfg_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    Stm32g474SyscfgState *s =
        STM32G474_SYSCFG(register_array_get_owner(reg_array));

    if (s->resetting || s->peripheral_reset_asserted) {
        return;
    }

    if (addr >= A_EXTICR1 && addr <= A_EXTICR4) {
        s->raw_exticr = value;
        s->raw_exticr_valid = true;
    }
    register_write_memory(opaque, addr, value, size);
    s->raw_exticr_valid = false;
}

static const MemoryRegionOps stm32g474_syscfg_ops = {
    .read = register_read_memory,
    .write = stm32g474_syscfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void stm32g474_syscfg_reset_registers(Stm32g474SyscfgState *s,
                                              bool preserve_locks)
{
    uint32_t locks = s->regs[R_CFGR2] & R_CFGR2_LOCK_MASK;
    uint32_t swpr = s->regs[R_SWPR];
    bool was_resetting = s->resetting;

    s->resetting = true;
    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_syscfg_regs_info); i++) {
        unsigned int index =
            stm32g474_syscfg_regs_info[i].addr / sizeof(uint32_t);

        register_reset(&s->regs_info[index]);
    }
    if (preserve_locks) {
        s->regs[R_CFGR2] = locks;
        s->regs[R_SWPR] = swpr;
    }
    s->key_phase = STM32G474_SYSCFG_KEY_LOCKED;
    s->raw_exticr = 0;
    s->raw_exticr_valid = false;
    s->resetting = was_resetting;
}

static void stm32g474_syscfg_reset_input(void *opaque, int n, int level)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(opaque);
    bool asserted = level != 0;

    if (asserted == s->peripheral_reset_asserted) {
        return;
    }

    s->peripheral_reset_asserted = asserted;
    if (asserted) {
        stm32g474_syscfg_reset_registers(s, true);
    }
    if (s->resetting) {
        return;
    }
    stm32g474_syscfg_route_all(s, true);
}

static void stm32g474_syscfg_gpio_input(void *opaque, int n, int level)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(opaque);
    unsigned int port = n / STM32G474_SYSCFG_NUM_LINES;
    unsigned int line = n % STM32G474_SYSCFG_NUM_LINES;
    uint16_t bit = BIT(line);
    bool high = level > 0;
    bool old = (s->gpio_levels[port] & bit) != 0;

    if (old == high) {
        return;
    }
    if (high) {
        s->gpio_levels[port] |= bit;
    } else {
        s->gpio_levels[port] &= ~bit;
    }

    if (!s->resetting && stm32g474_syscfg_selector(s, line) == port) {
        stm32g474_syscfg_route_line(s, line, false);
        s->output_cache_valid = true;
    }
}

static void stm32g474_syscfg_parity_error_input(void *opaque, int n,
                                                 int level)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(opaque);

    if (level > 0 && !s->resetting && !s->peripheral_reset_asserted) {
        s->regs[R_CFGR2] |= R_CFGR2_SPF_MASK;
    }
}

static void stm32g474_syscfg_reset_enter(Object *obj, ResetType type)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(obj);

    s->resetting = true;
    stm32g474_syscfg_reset_registers(s, false);
    s->peripheral_reset_asserted = false;
    s->output_cache_valid = false;
}

static void stm32g474_syscfg_reset_hold(Object *obj, ResetType type)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(obj);

    stm32g474_syscfg_route_all(s, true);
}

static void stm32g474_syscfg_reset_exit(Object *obj, ResetType type)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(obj);

    s->resetting = false;
    s->output_cache_valid = false;
    stm32g474_syscfg_route_all(s, true);
}

static bool stm32g474_syscfg_exticr_valid(Stm32g474SyscfgState *s)
{
    for (unsigned int line = 0; line < STM32G474_SYSCFG_NUM_LINES; line++) {
        unsigned int reg =
            R_EXTICR1 + line / SYSCFG_EXTICR_FIELDS_PER_REG;

        if (s->regs[reg] & SYSCFG_EXTICR_RESERVED) {
            return false;
        }
        if (!stm32g474_syscfg_selector_valid(
                line, stm32g474_syscfg_selector(s, line))) {
            return false;
        }
    }
    return true;
}

static int stm32g474_syscfg_post_load(void *opaque, int version_id)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(opaque);

    if (s->key_phase > STM32G474_SYSCFG_KEY_UNLOCKED ||
        !stm32g474_syscfg_exticr_valid(s)) {
        return -EINVAL;
    }

    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_syscfg_regs_info); i++) {
        const RegisterAccessInfo *access = &stm32g474_syscfg_regs_info[i];
        unsigned int index = access->addr / sizeof(uint32_t);

        s->regs[index] = (s->regs[index] & ~access->rsvd) |
                         (access->reset & access->rsvd);
    }
    s->regs[R_MEMRMP] = 0;
    s->regs[R_SCSR] &= ~R_SCSR_CCMBSY_MASK;
    s->regs[R_SKR] = 0;
    s->resetting = false;
    s->raw_exticr = 0;
    s->raw_exticr_valid = false;
    s->output_cache_valid = false;

    if (s->peripheral_reset_asserted) {
        stm32g474_syscfg_reset_registers(s, true);
    }

    stm32g474_syscfg_route_all(s, true);
    return 0;
}

static const VMStateDescription vmstate_stm32g474_syscfg = {
    .name = TYPE_STM32G474_SYSCFG,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stm32g474_syscfg_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474SyscfgState,
                             STM32G474_SYSCFG_NUM_REGS),
        VMSTATE_UINT16_ARRAY(gpio_levels, Stm32g474SyscfgState,
                             STM32G474_SYSCFG_NUM_PORTS),
        VMSTATE_UINT8(key_phase, Stm32g474SyscfgState),
        VMSTATE_BOOL(peripheral_reset_asserted, Stm32g474SyscfgState),
        VMSTATE_CLOCK(clk, Stm32g474SyscfgState),
        VMSTATE_END_OF_LIST()
    },
};

static void stm32g474_syscfg_realize(DeviceState *dev, Error **errp)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(dev);

    if (!clock_has_source(s->clk)) {
        error_setg(errp, TYPE_STM32G474_SYSCFG
                   ": clk clock must be connected");
    }
}

static void stm32g474_syscfg_init(Object *obj)
{
    Stm32g474SyscfgState *s = STM32G474_SYSCFG(obj);
    DeviceState *dev = DEVICE(obj);

    s->reg_array = register_init_block32(
        dev, stm32g474_syscfg_regs_info,
        ARRAY_SIZE(stm32g474_syscfg_regs_info), s->regs_info, s->regs,
        &stm32g474_syscfg_ops, false, STM32G474_SYSCFG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
    s->clk = qdev_init_clock_in(dev, "clk", NULL, NULL, 0);
    qdev_init_gpio_in_named(dev, stm32g474_syscfg_reset_input, "reset", 1);
    qdev_init_gpio_in_named(dev, stm32g474_syscfg_gpio_input, "gpio-in",
                            STM32G474_SYSCFG_NUM_PORTS *
                            STM32G474_SYSCFG_NUM_LINES);
    qdev_init_gpio_out_named(dev, s->exti_out, "exti-out",
                             STM32G474_SYSCFG_NUM_LINES);
    qdev_init_gpio_in_named(dev, stm32g474_syscfg_parity_error_input,
                            "parity-error", 1);
}

static void stm32g474_syscfg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_syscfg_realize;
    dc->vmsd = &vmstate_stm32g474_syscfg;
    dc->user_creatable = false;
    rc->phases.enter = stm32g474_syscfg_reset_enter;
    rc->phases.hold = stm32g474_syscfg_reset_hold;
    rc->phases.exit = stm32g474_syscfg_reset_exit;
}

static const TypeInfo stm32g474_syscfg_info = {
    .name = TYPE_STM32G474_SYSCFG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32g474SyscfgState),
    .instance_init = stm32g474_syscfg_init,
    .class_init = stm32g474_syscfg_class_init,
};

static void stm32g474_syscfg_register_types(void)
{
    type_register_static(&stm32g474_syscfg_info);
}

type_init(stm32g474_syscfg_register_types)

/*
 * STM32G474 general-purpose I/O ports
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
#include "hw/gpio/stm32g474_gpio.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define STM32G474_GPIO_NUM_REGS 11

typedef struct Stm32g474GpioVariant Stm32g474GpioVariant;

struct Stm32g474GpioState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_GPIO_NUM_REGS];
    uint32_t regs[STM32G474_GPIO_NUM_REGS];

    Clock *clk;
    qemu_irq pin_out[STM32G474_GPIO_NUM_PINS];

    uint16_t external_driven;
    uint16_t external_level;
    uint16_t lock_candidate;
    uint16_t locked_mask;
    uint16_t resolved_cache;
    uint8_t lock_phase;
    uint8_t current_access_size;
    uint8_t current_access_lane;
    bool peripheral_reset_asserted;
    bool resetting;
    bool resolved_cache_valid;
};

struct Stm32g474GpioClass {
    SysBusDeviceClass parent_class;

    const Stm32g474GpioVariant *variant;
};

REG32(MODER, 0x00)
REG32(OTYPER, 0x04)
    FIELD(OTYPER, OT, 0, 16)
REG32(OSPEEDR, 0x08)
REG32(PUPDR, 0x0c)
REG32(IDR, 0x10)
    FIELD(IDR, ID, 0, 16)
REG32(ODR, 0x14)
    FIELD(ODR, OD, 0, 16)
REG32(BSRR, 0x18)
    FIELD(BSRR, BS, 0, 16)
    FIELD(BSRR, BR, 16, 16)
REG32(LCKR, 0x1c)
    FIELD(LCKR, LCK, 0, 16)
    FIELD(LCKR, LCKK, 16, 1)
REG32(AFRL, 0x20)
REG32(AFRH, 0x24)
REG32(BRR, 0x28)
    FIELD(BRR, BR, 0, 16)

#define STM32G474_GPIO_PIN_MASK UINT16_MAX

enum Stm32g474GpioLockPhase {
    STM32G474_GPIO_LOCK_IDLE,
    STM32G474_GPIO_LOCK_HAVE_KEY1,
    STM32G474_GPIO_LOCK_HAVE_KEY0,
    STM32G474_GPIO_LOCK_ARMED_FOR_READ,
    STM32G474_GPIO_LOCK_LOCKED,
    STM32G474_GPIO_LOCK_PHASE_COUNT,
};

struct Stm32g474GpioVariant {
    const char *name;
    const RegisterAccessInfo *regs_info;
    size_t num_regs;
};

static const Stm32g474GpioVariant *
stm32g474_gpio_get_variant(Stm32g474GpioState *s)
{
    return STM32G474_GPIO_GET_CLASS(s)->variant;
}

static bool stm32g474_gpio_in_reset(Stm32g474GpioState *s)
{
    return s->resetting || s->peripheral_reset_asserted;
}

static uint32_t stm32g474_gpio_access_mask(Stm32g474GpioState *s)
{
    unsigned int shift = 8 * s->current_access_lane;

    if (!s->current_access_size) {
        return 0;
    }

    return MAKE_64BIT_MASK(shift, 8 * s->current_access_size);
}

static uint32_t stm32g474_gpio_resolved_value(Stm32g474GpioState *s)
{
    uint32_t moder = s->regs[R_MODER];
    uint32_t otyper = s->regs[R_OTYPER];
    uint32_t pupdr = s->regs[R_PUPDR];
    uint32_t odr = s->regs[R_ODR];
    uint32_t resolved = 0;

    for (unsigned int pin = 0; pin < STM32G474_GPIO_NUM_PINS; pin++) {
        uint32_t bit = BIT(pin);
        unsigned int mode = extract32(moder, 2 * pin, 2);
        unsigned int pull = extract32(pupdr, 2 * pin, 2);
        bool open_drain = (otyper & bit) != 0;
        bool latch_high = (odr & bit) != 0;
        bool level;

        if (mode == 1 && (!open_drain || !latch_high)) {
            level = latch_high;
        } else if (s->external_driven & bit) {
            level = (s->external_level & bit) != 0;
        } else {
            level = pull == 1;
        }

        if (level) {
            resolved |= bit;
        }
    }

    return resolved;
}

static void stm32g474_gpio_resolve(Stm32g474GpioState *s, bool force)
{
    uint16_t old = s->resolved_cache;
    uint16_t resolved = stm32g474_gpio_resolved_value(s);
    uint16_t changed = old ^ resolved;

    s->regs[R_IDR] = resolved;
    if (force || !s->resolved_cache_valid) {
        changed = STM32G474_GPIO_PIN_MASK;
    }

    s->resolved_cache = resolved;
    s->resolved_cache_valid = true;
    for (unsigned int pin = 0; pin < STM32G474_GPIO_NUM_PINS; pin++) {
        if (changed & BIT(pin)) {
            qemu_set_irq(s->pin_out[pin], (resolved & BIT(pin)) != 0);
        }
    }
}

static uint32_t stm32g474_gpio_expand_locked_mask(uint16_t pins,
                                                  unsigned int width,
                                                  unsigned int first_pin,
                                                  unsigned int num_pins)
{
    uint32_t mask = 0;

    for (unsigned int pin = first_pin; pin < first_pin + num_pins; pin++) {
        if (pins & BIT(pin)) {
            mask |= MAKE_64BIT_MASK(width * (pin - first_pin), width);
        }
    }

    return mask;
}

static uint64_t stm32g474_gpio_config_pre_write(RegisterInfo *reg,
                                                 uint64_t val,
                                                 uint32_t locked_fields)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t old = *(uint32_t *)reg->data;

    if (stm32g474_gpio_in_reset(s)) {
        return old;
    }

    return (val & ~locked_fields) | (old & locked_fields);
}

static uint64_t stm32g474_gpio_moder_pre_write(RegisterInfo *reg,
                                                uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t mask = stm32g474_gpio_expand_locked_mask(
        s->locked_mask, 2, 0, STM32G474_GPIO_NUM_PINS);

    return stm32g474_gpio_config_pre_write(reg, val, mask);
}

static uint64_t stm32g474_gpio_otyper_pre_write(RegisterInfo *reg,
                                                 uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);

    return stm32g474_gpio_config_pre_write(reg, val, s->locked_mask);
}

static uint64_t stm32g474_gpio_ospeedr_pre_write(RegisterInfo *reg,
                                                  uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t mask = stm32g474_gpio_expand_locked_mask(
        s->locked_mask, 2, 0, STM32G474_GPIO_NUM_PINS);

    return stm32g474_gpio_config_pre_write(reg, val, mask);
}

static uint64_t stm32g474_gpio_pupdr_pre_write(RegisterInfo *reg,
                                                uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t old = *(uint32_t *)reg->data;
    uint32_t mask = stm32g474_gpio_expand_locked_mask(
        s->locked_mask, 2, 0, STM32G474_GPIO_NUM_PINS);
    uint32_t new_val = stm32g474_gpio_config_pre_write(reg, val, mask);

    if (stm32g474_gpio_in_reset(s)) {
        return new_val;
    }

    for (unsigned int pin = 0; pin < STM32G474_GPIO_NUM_PINS; pin++) {
        if (extract32(new_val, 2 * pin, 2) == 3 &&
            extract32(old, 2 * pin, 2) != 3) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: reserved PUPDR encoding for pin %u\n",
                          object_get_typename(OBJECT(s)), pin);
        }
    }

    return new_val;
}

static uint64_t stm32g474_gpio_odr_pre_write(RegisterInfo *reg,
                                              uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);

    return stm32g474_gpio_in_reset(s) ? *(uint32_t *)reg->data : val;
}

static uint64_t stm32g474_gpio_afrl_pre_write(RegisterInfo *reg,
                                               uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t mask = stm32g474_gpio_expand_locked_mask(
        s->locked_mask, 4, 0, STM32G474_GPIO_NUM_PINS / 2);

    return stm32g474_gpio_config_pre_write(reg, val, mask);
}

static uint64_t stm32g474_gpio_afrh_pre_write(RegisterInfo *reg,
                                               uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t mask = stm32g474_gpio_expand_locked_mask(
        s->locked_mask, 4, STM32G474_GPIO_NUM_PINS / 2,
        STM32G474_GPIO_NUM_PINS / 2);

    return stm32g474_gpio_config_pre_write(reg, val, mask);
}

static void stm32g474_gpio_resolve_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);

    if (!stm32g474_gpio_in_reset(s)) {
        stm32g474_gpio_resolve(s, false);
    }
}

static void stm32g474_gpio_bsrr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t command = val;
    uint32_t set_mask = command & R_BSRR_BS_MASK;
    uint32_t reset_mask = FIELD_EX32(command, BSRR, BR);

    s->regs[R_BSRR] = 0;
    if (stm32g474_gpio_in_reset(s)) {
        return;
    }

    s->regs[R_ODR] = ((s->regs[R_ODR] & ~reset_mask) | set_mask) &
                     STM32G474_GPIO_PIN_MASK;
    stm32g474_gpio_resolve(s, false);
}

static void stm32g474_gpio_brr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t command = val & R_BRR_BR_MASK;

    s->regs[R_BRR] = 0;
    if (stm32g474_gpio_in_reset(s)) {
        return;
    }

    s->regs[R_ODR] &= ~command;
    stm32g474_gpio_resolve(s, false);
}

static uint32_t stm32g474_gpio_lckr_image(Stm32g474GpioState *s)
{
    switch (s->lock_phase) {
    case STM32G474_GPIO_LOCK_HAVE_KEY1:
    case STM32G474_GPIO_LOCK_ARMED_FOR_READ:
        return s->lock_candidate | R_LCKR_LCKK_MASK;
    case STM32G474_GPIO_LOCK_HAVE_KEY0:
        return s->lock_candidate;
    case STM32G474_GPIO_LOCK_LOCKED:
        return s->locked_mask | R_LCKR_LCKK_MASK;
    case STM32G474_GPIO_LOCK_IDLE:
    default:
        return s->regs[R_LCKR] & R_LCKR_LCK_MASK;
    }
}

static uint32_t stm32g474_gpio_lckr_abort(Stm32g474GpioState *s,
                                          uint16_t last_mask)
{
    s->lock_phase = STM32G474_GPIO_LOCK_IDLE;
    s->lock_candidate = 0;
    s->locked_mask = 0;
    s->regs[R_LCKR] = last_mask;
    return last_mask;
}

static bool stm32g474_gpio_is_word_access(Stm32g474GpioState *s)
{
    return s->current_access_size == sizeof(uint32_t) &&
           s->current_access_lane == 0;
}

static uint64_t stm32g474_gpio_lckr_pre_write(RegisterInfo *reg,
                                               uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t old = *(uint32_t *)reg->data;
    uint16_t mask = val & R_LCKR_LCK_MASK;
    bool key = (val & R_LCKR_LCKK_MASK) != 0;

    if (stm32g474_gpio_in_reset(s)) {
        return old;
    }
    if (s->lock_phase == STM32G474_GPIO_LOCK_LOCKED) {
        return s->locked_mask | R_LCKR_LCKK_MASK;
    }
    if (!stm32g474_gpio_is_word_access(s)) {
        return stm32g474_gpio_lckr_abort(s, old & R_LCKR_LCK_MASK);
    }

    switch (s->lock_phase) {
    case STM32G474_GPIO_LOCK_IDLE:
        if (key) {
            s->lock_candidate = mask;
            s->lock_phase = STM32G474_GPIO_LOCK_HAVE_KEY1;
            return mask | R_LCKR_LCKK_MASK;
        }
        break;
    case STM32G474_GPIO_LOCK_HAVE_KEY1:
        if (!key && mask == s->lock_candidate) {
            s->lock_phase = STM32G474_GPIO_LOCK_HAVE_KEY0;
            return mask;
        }
        break;
    case STM32G474_GPIO_LOCK_HAVE_KEY0:
        if (key && mask == s->lock_candidate) {
            s->lock_phase = STM32G474_GPIO_LOCK_ARMED_FOR_READ;
            return mask | R_LCKR_LCKK_MASK;
        }
        break;
    case STM32G474_GPIO_LOCK_ARMED_FOR_READ:
        break;
    default:
        g_assert_not_reached();
    }

    return stm32g474_gpio_lckr_abort(s, mask);
}

static uint64_t stm32g474_gpio_lckr_post_read(RegisterInfo *reg, uint64_t val)
{
    Stm32g474GpioState *s = STM32G474_GPIO(reg->opaque);
    uint32_t access_mask = stm32g474_gpio_access_mask(s);
    uint32_t image;

    if (s->lock_phase == STM32G474_GPIO_LOCK_LOCKED) {
        image = s->locked_mask | R_LCKR_LCKK_MASK;
        s->regs[R_LCKR] = image;
        return image & access_mask;
    }

    if (s->lock_phase == STM32G474_GPIO_LOCK_ARMED_FOR_READ &&
        stm32g474_gpio_is_word_access(s)) {
        s->locked_mask = s->lock_candidate;
        s->lock_phase = STM32G474_GPIO_LOCK_LOCKED;
        image = s->locked_mask | R_LCKR_LCKK_MASK;
        s->regs[R_LCKR] = image;
        return image;
    }

    if (s->lock_phase != STM32G474_GPIO_LOCK_IDLE) {
        image = stm32g474_gpio_lckr_abort(
            s, s->regs[R_LCKR] & R_LCKR_LCK_MASK);
        return image & access_mask;
    }

    return val;
}

static const RegisterAccessInfo stm32g474_gpio_a_regs_info[] = {
    {
        .name = "MODER",
        .addr = A_MODER,
        .reset = 0xabffffff,
        .pre_write = stm32g474_gpio_moder_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "OTYPER",
        .addr = A_OTYPER,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_gpio_otyper_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "OSPEEDR",
        .addr = A_OSPEEDR,
        .reset = 0x0c000000,
        .pre_write = stm32g474_gpio_ospeedr_pre_write,
    }, {
        .name = "PUPDR",
        .addr = A_PUPDR,
        .reset = 0x64000000,
        .pre_write = stm32g474_gpio_pupdr_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "IDR",
        .addr = A_IDR,
        .ro = 0x0000ffff,
        .rsvd = 0xffff0000,
    }, {
        .name = "ODR",
        .addr = A_ODR,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_gpio_odr_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "BSRR",
        .addr = A_BSRR,
        .post_write = stm32g474_gpio_bsrr_post_write,
    }, {
        .name = "LCKR",
        .addr = A_LCKR,
        .rsvd = 0xfffe0000,
        .pre_write = stm32g474_gpio_lckr_pre_write,
        .post_read = stm32g474_gpio_lckr_post_read,
    }, {
        .name = "AFRL",
        .addr = A_AFRL,
        .pre_write = stm32g474_gpio_afrl_pre_write,
    }, {
        .name = "AFRH",
        .addr = A_AFRH,
        .pre_write = stm32g474_gpio_afrh_pre_write,
    }, {
        .name = "BRR",
        .addr = A_BRR,
        .rsvd = 0xffff0000,
        .post_write = stm32g474_gpio_brr_post_write,
    },
};

static const RegisterAccessInfo stm32g474_gpio_b_regs_info[] = {
    {
        .name = "MODER",
        .addr = A_MODER,
        .reset = 0xfffffebf,
        .pre_write = stm32g474_gpio_moder_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "OTYPER",
        .addr = A_OTYPER,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_gpio_otyper_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "OSPEEDR",
        .addr = A_OSPEEDR,
        .pre_write = stm32g474_gpio_ospeedr_pre_write,
    }, {
        .name = "PUPDR",
        .addr = A_PUPDR,
        .reset = 0x00000100,
        .pre_write = stm32g474_gpio_pupdr_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "IDR",
        .addr = A_IDR,
        .ro = 0x0000ffff,
        .rsvd = 0xffff0000,
    }, {
        .name = "ODR",
        .addr = A_ODR,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_gpio_odr_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "BSRR",
        .addr = A_BSRR,
        .post_write = stm32g474_gpio_bsrr_post_write,
    }, {
        .name = "LCKR",
        .addr = A_LCKR,
        .rsvd = 0xfffe0000,
        .pre_write = stm32g474_gpio_lckr_pre_write,
        .post_read = stm32g474_gpio_lckr_post_read,
    }, {
        .name = "AFRL",
        .addr = A_AFRL,
        .pre_write = stm32g474_gpio_afrl_pre_write,
    }, {
        .name = "AFRH",
        .addr = A_AFRH,
        .pre_write = stm32g474_gpio_afrh_pre_write,
    }, {
        .name = "BRR",
        .addr = A_BRR,
        .rsvd = 0xffff0000,
        .post_write = stm32g474_gpio_brr_post_write,
    },
};

static const RegisterAccessInfo stm32g474_gpio_cg_regs_info[] = {
    {
        .name = "MODER",
        .addr = A_MODER,
        .reset = 0xffffffff,
        .pre_write = stm32g474_gpio_moder_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "OTYPER",
        .addr = A_OTYPER,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_gpio_otyper_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "OSPEEDR",
        .addr = A_OSPEEDR,
        .pre_write = stm32g474_gpio_ospeedr_pre_write,
    }, {
        .name = "PUPDR",
        .addr = A_PUPDR,
        .pre_write = stm32g474_gpio_pupdr_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "IDR",
        .addr = A_IDR,
        .ro = 0x0000ffff,
        .rsvd = 0xffff0000,
    }, {
        .name = "ODR",
        .addr = A_ODR,
        .rsvd = 0xffff0000,
        .pre_write = stm32g474_gpio_odr_pre_write,
        .post_write = stm32g474_gpio_resolve_post_write,
    }, {
        .name = "BSRR",
        .addr = A_BSRR,
        .post_write = stm32g474_gpio_bsrr_post_write,
    }, {
        .name = "LCKR",
        .addr = A_LCKR,
        .rsvd = 0xfffe0000,
        .pre_write = stm32g474_gpio_lckr_pre_write,
        .post_read = stm32g474_gpio_lckr_post_read,
    }, {
        .name = "AFRL",
        .addr = A_AFRL,
        .pre_write = stm32g474_gpio_afrl_pre_write,
    }, {
        .name = "AFRH",
        .addr = A_AFRH,
        .pre_write = stm32g474_gpio_afrh_pre_write,
    }, {
        .name = "BRR",
        .addr = A_BRR,
        .rsvd = 0xffff0000,
        .post_write = stm32g474_gpio_brr_post_write,
    },
};

static const Stm32g474GpioVariant stm32g474_gpio_a_variant = {
    .name = "GPIOA",
    .regs_info = stm32g474_gpio_a_regs_info,
    .num_regs = ARRAY_SIZE(stm32g474_gpio_a_regs_info),
};

static const Stm32g474GpioVariant stm32g474_gpio_b_variant = {
    .name = "GPIOB",
    .regs_info = stm32g474_gpio_b_regs_info,
    .num_regs = ARRAY_SIZE(stm32g474_gpio_b_regs_info),
};

static const Stm32g474GpioVariant stm32g474_gpio_cg_variant = {
    .name = "GPIOC-G",
    .regs_info = stm32g474_gpio_cg_regs_info,
    .num_regs = ARRAY_SIZE(stm32g474_gpio_cg_regs_info),
};

G_STATIC_ASSERT(STM32G474_GPIO_NUM_REGS ==
                A_BRR / sizeof(uint32_t) + 1);

static RegisterInfo *stm32g474_gpio_find_register(RegisterInfoArray *reg_array,
                                                  hwaddr addr)
{
    for (int i = 0; i < reg_array->num_elements; i++) {
        if (reg_array->r[i]->access->addr == addr) {
            return reg_array->r[i];
        }
    }

    return NULL;
}

static bool stm32g474_gpio_valid_access(RegisterInfoArray *reg_array,
                                         hwaddr addr, unsigned int size)
{
    if ((size != 1 && size != 2 && size != 4) ||
        (addr & (size - 1)) != 0 || (addr & 3) + size > sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid access at address 0x%" HWADDR_PRIx
                      " of size %u\n", reg_array->prefix, addr, size);
        return false;
    }

    return true;
}

static uint64_t stm32g474_gpio_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    Stm32g474GpioState *s =
        STM32G474_GPIO(register_array_get_owner(reg_array));
    hwaddr reg_addr = addr & ~(hwaddr)3;
    RegisterInfo *reg;
    unsigned int lane;
    unsigned int shift;
    uint64_t re;
    uint64_t value;

    if (!stm32g474_gpio_valid_access(reg_array, addr, size)) {
        return 0;
    }

    reg = stm32g474_gpio_find_register(reg_array, reg_addr);
    if (!reg) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read from unimplemented register at address 0x%"
                      HWADDR_PRIx "\n", reg_array->prefix, addr);
        return 0;
    }

    lane = addr & 3;
    shift = 8 * lane;
    re = MAKE_64BIT_MASK(shift, size * 8);
    s->current_access_size = size;
    s->current_access_lane = lane;
    value = register_read(reg, re, reg_array->prefix, reg_array->debug);
    s->current_access_size = 0;
    s->current_access_lane = 0;

    return (value >> shift) & MAKE_64BIT_MASK(0, size * 8);
}

static void stm32g474_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    Stm32g474GpioState *s =
        STM32G474_GPIO(register_array_get_owner(reg_array));
    hwaddr reg_addr = addr & ~(hwaddr)3;
    RegisterInfo *reg;
    unsigned int lane;
    unsigned int shift;
    uint64_t we;
    uint64_t lane_value;

    if (!stm32g474_gpio_valid_access(reg_array, addr, size)) {
        return;
    }

    reg = stm32g474_gpio_find_register(reg_array, reg_addr);
    if (!reg) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to unimplemented register at address 0x%"
                      HWADDR_PRIx "\n", reg_array->prefix, addr);
        return;
    }

    lane = addr & 3;
    shift = 8 * lane;
    we = MAKE_64BIT_MASK(shift, size * 8);
    lane_value = (value & MAKE_64BIT_MASK(0, size * 8)) << shift;
    s->current_access_size = size;
    s->current_access_lane = lane;
    register_write(reg, lane_value, we, reg_array->prefix,
                   reg_array->debug);
    s->current_access_size = 0;
    s->current_access_lane = 0;
}

static const MemoryRegionOps stm32g474_gpio_ops = {
    .read = stm32g474_gpio_read,
    .write = stm32g474_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void stm32g474_gpio_reset_registers(Stm32g474GpioState *s)
{
    bool was_resetting = s->resetting;

    s->resetting = true;
    for (int i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }
    s->regs[R_BSRR] = 0;
    s->regs[R_BRR] = 0;
    s->lock_phase = STM32G474_GPIO_LOCK_IDLE;
    s->lock_candidate = 0;
    s->locked_mask = 0;
    s->current_access_size = 0;
    s->current_access_lane = 0;
    s->resetting = was_resetting;
    s->resolved_cache_valid = false;
}

static void stm32g474_gpio_reset_input(void *opaque, int n, int level)
{
    Stm32g474GpioState *s = STM32G474_GPIO(opaque);
    bool asserted = level != 0;

    if (asserted == s->peripheral_reset_asserted) {
        return;
    }

    s->peripheral_reset_asserted = asserted;
    if (asserted) {
        stm32g474_gpio_reset_registers(s);
        stm32g474_gpio_resolve(s, true);
    }
}

static void stm32g474_gpio_pin_input(void *opaque, int n, int level)
{
    Stm32g474GpioState *s = STM32G474_GPIO(opaque);
    uint16_t bit = BIT(n);

    if (level < 0) {
        if (!(s->external_driven & bit)) {
            return;
        }
        s->external_driven &= ~bit;
    } else {
        bool was_driven = (s->external_driven & bit) != 0;
        bool was_high = (s->external_level & bit) != 0;
        bool high = level > 0;

        if (was_driven && was_high == high) {
            return;
        }
        s->external_driven |= bit;
        if (high) {
            s->external_level |= bit;
        } else {
            s->external_level &= ~bit;
        }
    }

    stm32g474_gpio_resolve(s, false);
}

static void stm32g474_gpio_reset_enter(Object *obj, ResetType type)
{
    Stm32g474GpioState *s = STM32G474_GPIO(obj);

    s->resetting = true;
}

static void stm32g474_gpio_reset_hold(Object *obj, ResetType type)
{
    Stm32g474GpioState *s = STM32G474_GPIO(obj);

    stm32g474_gpio_reset_registers(s);
    s->peripheral_reset_asserted = false;
}

static void stm32g474_gpio_reset_exit(Object *obj, ResetType type)
{
    Stm32g474GpioState *s = STM32G474_GPIO(obj);

    s->resetting = false;
    s->resolved_cache_valid = false;
    stm32g474_gpio_resolve(s, true);
}

static bool stm32g474_gpio_valid_lock_state(Stm32g474GpioState *s)
{
    switch (s->lock_phase) {
    case STM32G474_GPIO_LOCK_IDLE:
        return s->lock_candidate == 0 && s->locked_mask == 0;
    case STM32G474_GPIO_LOCK_HAVE_KEY1:
    case STM32G474_GPIO_LOCK_HAVE_KEY0:
    case STM32G474_GPIO_LOCK_ARMED_FOR_READ:
        return s->locked_mask == 0;
    case STM32G474_GPIO_LOCK_LOCKED:
        return s->lock_candidate == s->locked_mask;
    default:
        return false;
    }
}

static int stm32g474_gpio_post_load(void *opaque, int version_id)
{
    Stm32g474GpioState *s = STM32G474_GPIO(opaque);
    const Stm32g474GpioVariant *variant = stm32g474_gpio_get_variant(s);

    s->resetting = false;
    s->current_access_size = 0;
    s->current_access_lane = 0;
    if (!stm32g474_gpio_valid_lock_state(s)) {
        return -EINVAL;
    }

    for (size_t i = 0; i < variant->num_regs; i++) {
        const RegisterAccessInfo *access = &variant->regs_info[i];
        unsigned int index = access->addr / sizeof(uint32_t);

        s->regs[index] = (s->regs[index] & ~access->rsvd) |
                         (access->reset & access->rsvd);
    }

    s->external_driven &= STM32G474_GPIO_PIN_MASK;
    s->external_level &= STM32G474_GPIO_PIN_MASK;
    s->lock_candidate &= STM32G474_GPIO_PIN_MASK;
    s->locked_mask &= STM32G474_GPIO_PIN_MASK;
    s->regs[R_BSRR] = 0;
    s->regs[R_BRR] = 0;
    s->regs[R_LCKR] = stm32g474_gpio_lckr_image(s);

    if (s->peripheral_reset_asserted) {
        stm32g474_gpio_reset_registers(s);
    }

    s->resolved_cache_valid = false;
    stm32g474_gpio_resolve(s, true);
    return 0;
}

static const VMStateDescription vmstate_stm32g474_gpio = {
    .name = TYPE_STM32G474_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stm32g474_gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474GpioState,
                             STM32G474_GPIO_NUM_REGS),
        VMSTATE_UINT16(external_driven, Stm32g474GpioState),
        VMSTATE_UINT16(external_level, Stm32g474GpioState),
        VMSTATE_UINT16(lock_candidate, Stm32g474GpioState),
        VMSTATE_UINT16(locked_mask, Stm32g474GpioState),
        VMSTATE_UINT8(lock_phase, Stm32g474GpioState),
        VMSTATE_BOOL(peripheral_reset_asserted, Stm32g474GpioState),
        VMSTATE_CLOCK(clk, Stm32g474GpioState),
        VMSTATE_END_OF_LIST()
    },
};

static void stm32g474_gpio_realize(DeviceState *dev, Error **errp)
{
    Stm32g474GpioState *s = STM32G474_GPIO(dev);
    const Stm32g474GpioVariant *variant = stm32g474_gpio_get_variant(s);

    if (!variant || !s->reg_array) {
        error_setg(errp, TYPE_STM32G474_GPIO
                   ": concrete variant is missing");
        return;
    }
    if (!clock_has_source(s->clk)) {
        error_setg(errp, TYPE_STM32G474_GPIO
                   ": clk clock must be connected");
        return;
    }
}

static void stm32g474_gpio_init(Object *obj)
{
    Stm32g474GpioState *s = STM32G474_GPIO(obj);
    Stm32g474GpioClass *gc = STM32G474_GPIO_GET_CLASS(obj);
    DeviceState *dev = DEVICE(obj);

    g_assert(gc->variant);
    g_assert(gc->variant->num_regs <= STM32G474_GPIO_NUM_REGS);

    s->reg_array = register_init_block32(
        dev, gc->variant->regs_info, gc->variant->num_regs,
        s->regs_info, s->regs, &stm32g474_gpio_ops, false,
        STM32G474_GPIO_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);
    s->clk = qdev_init_clock_in(dev, "clk", NULL, NULL, 0);
    qdev_init_gpio_in_named(dev, stm32g474_gpio_reset_input, "reset", 1);
    qdev_init_gpio_in_named(dev, stm32g474_gpio_pin_input, "pin-in",
                            STM32G474_GPIO_NUM_PINS);
    qdev_init_gpio_out_named(dev, s->pin_out, "pin-out",
                             STM32G474_GPIO_NUM_PINS);
}

static void stm32g474_gpio_base_class_init(ObjectClass *klass,
                                            const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_gpio_realize;
    dc->vmsd = &vmstate_stm32g474_gpio;
    dc->user_creatable = false;
    rc->phases.enter = stm32g474_gpio_reset_enter;
    rc->phases.hold = stm32g474_gpio_reset_hold;
    rc->phases.exit = stm32g474_gpio_reset_exit;
}

static void stm32g474_gpio_a_class_init(ObjectClass *klass, const void *data)
{
    Stm32g474GpioClass *gc = STM32G474_GPIO_CLASS(klass);

    gc->variant = &stm32g474_gpio_a_variant;
}

static void stm32g474_gpio_b_class_init(ObjectClass *klass, const void *data)
{
    Stm32g474GpioClass *gc = STM32G474_GPIO_CLASS(klass);

    gc->variant = &stm32g474_gpio_b_variant;
}

static void stm32g474_gpio_cg_class_init(ObjectClass *klass, const void *data)
{
    Stm32g474GpioClass *gc = STM32G474_GPIO_CLASS(klass);

    gc->variant = &stm32g474_gpio_cg_variant;
}

static const TypeInfo stm32g474_gpio_types[] = {
    {
        .name = TYPE_STM32G474_GPIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Stm32g474GpioState),
        .class_size = sizeof(Stm32g474GpioClass),
        .instance_init = stm32g474_gpio_init,
        .class_init = stm32g474_gpio_base_class_init,
        .abstract = true,
    }, {
        .name = TYPE_STM32G474_GPIO_A,
        .parent = TYPE_STM32G474_GPIO,
        .class_init = stm32g474_gpio_a_class_init,
    }, {
        .name = TYPE_STM32G474_GPIO_B,
        .parent = TYPE_STM32G474_GPIO,
        .class_init = stm32g474_gpio_b_class_init,
    }, {
        .name = TYPE_STM32G474_GPIO_CG,
        .parent = TYPE_STM32G474_GPIO,
        .class_init = stm32g474_gpio_cg_class_init,
    },
};

DEFINE_TYPES(stm32g474_gpio_types)

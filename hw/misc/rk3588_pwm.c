/*
 * Rockchip RK3588 PWM controller
 *
 * The register layout and continuous output behavior are documented in the
 * RK3588 TRM V1.0, chapter 18. Capture, FIFO, power-key capture, one-shot
 * and center-aligned output are outside the current task scope.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/rk3588_pwm.h"
#include "migration/vmstate.h"

#define PWM_CHANNEL_STRIDE      0x10
#define PWM_CNT                 0x00
#define PWM_PERIOD              0x04
#define PWM_DUTY                0x08
#define PWM_CTRL                0x0c

#define PWM_INTSTS              0x40
#define PWM_INT_EN              0x44
#define PWM_FIFO_CTRL           0x50
#define PWM_FIFO_INTSTS         0x54
#define PWM_FIFO_TOUTTHR        0x58
#define PWM_VERSION_ID          0x5c
#define PWM_FIFO                0x60
#define PWM_PWRMATCH_CTRL       0x80
#define PWM_PWRMATCH_LPRE       0x84
#define PWM_PWRMATCH_HPRE       0x88
#define PWM_PWRMATCH_LD         0x8c
#define PWM_PWRMATCH_HD_ZERO    0x90
#define PWM_PWRMATCH_HD_ONE     0x94
#define PWM_PWRMATCH_VALUE0     0x98
#define PWM_PWRMATCH_VALUE9     0xbc
#define PWM_PWR_CAPTURE         0xcc
#define PWM_CHANNEL_IO_CTRL     0xd0

#define PWM_CTRL_RPT_MASK        (0xffu << 24)
#define PWM_CTRL_SCALE_MASK      (0xffu << 16)
#define PWM_CTRL_PRESCALE_MASK   (0x7u << 12)
#define PWM_CTRL_CLK_SRC_SEL     BIT(10)
#define PWM_CTRL_CLK_SEL         BIT(9)
#define PWM_CTRL_FORCE_CLK_EN    BIT(8)
#define PWM_CTRL_CH_CNT_EN       BIT(7)
#define PWM_CTRL_CONLOCK         BIT(6)
#define PWM_CTRL_OUTPUT_MODE     BIT(5)
#define PWM_CTRL_INACTIVE_POL    BIT(4)
#define PWM_CTRL_DUTY_POL        BIT(3)
#define PWM_CTRL_MODE_MASK       (3u << 1)
#define PWM_CTRL_MODE_ONESHOT    0u
#define PWM_CTRL_MODE_CONTINUOUS (1u << 1)
#define PWM_CTRL_MODE_CAPTURE    (2u << 1)
#define PWM_CTRL_MODE_RESERVED   (3u << 1)
#define PWM_CTRL_PWM_EN          BIT(0)

#define PWM_CTRL_WRITE_MASK \
    (PWM_CTRL_RPT_MASK | PWM_CTRL_SCALE_MASK | PWM_CTRL_PRESCALE_MASK | \
     PWM_CTRL_CLK_SRC_SEL | PWM_CTRL_CLK_SEL | PWM_CTRL_FORCE_CLK_EN | \
     PWM_CTRL_CH_CNT_EN | PWM_CTRL_CONLOCK | PWM_CTRL_OUTPUT_MODE | \
     PWM_CTRL_INACTIVE_POL | PWM_CTRL_DUTY_POL | PWM_CTRL_MODE_MASK | \
     PWM_CTRL_PWM_EN)

#define PWM_INTSTS_POL_MASK      (0xfu << 8)
#define PWM_INTSTS_CH_MASK       0xfu
#define PWM_INTSTS_POWER_MASK    BIT(7)
#define PWM_INTSTS_W1C_MASK      (PWM_INTSTS_CH_MASK | PWM_INTSTS_POWER_MASK)
#define PWM_INTSTS_READ_MASK     (PWM_INTSTS_POL_MASK | PWM_INTSTS_W1C_MASK)
#define PWM_INT_EN_MASK          PWM_INTSTS_W1C_MASK

#define PWM_FIFO_INTSTS_EMPTY    BIT(4)
#define PWM_FIFO_INTSTS_W1C_MASK 0xfu

static unsigned int pwm_channel_offset(unsigned int channel,
                                       unsigned int reg)
{
    return channel * PWM_CHANNEL_STRIDE + reg;
}

static uint32_t pwm_reg_read(RK3588PWMState *s, hwaddr offset)
{
    return s->regs[offset >> 2];
}

static bool pwm_polarity(RK3588PWMChannel *channel)
{
    return channel->active_ctrl & PWM_CTRL_DUTY_POL;
}

static void pwm_set_output(RK3588PWMChannel *channel, bool level)
{
    RK3588PWMState *s = channel->parent;
    unsigned int index = channel - s->channels;

    if (channel->output_level == level) {
        return;
    }

    channel->output_level = level;
    qemu_set_irq(s->outputs[index], level);
}

static int64_t pwm_ticks_to_ns(uint32_t ticks, uint32_t root_hz,
                               uint32_t divider)
{
    int64_t ns;

    ns = muldiv64((uint64_t)ticks * divider, NANOSECONDS_PER_SECOND,
                  root_hz);
    return MAX(ns, 1);
}

static void pwm_effective_clock(RK3588PWMState *s, uint32_t ctrl,
                                uint32_t *root_hz, uint32_t *divider)
{
    unsigned int prescale = extract32(ctrl, 12, 3);
    uint32_t selected_root_hz = ctrl & PWM_CTRL_CLK_SRC_SEL ?
                                s->osc_clock_hz : s->clock_hz;

    *root_hz = MAX(selected_root_hz, 1);
    *divider = 1u << prescale;

    if (ctrl & PWM_CTRL_CLK_SEL) {
        unsigned int scale = extract32(ctrl, 16, 8);

        *divider *= scale ? 2 * scale : 512;
    }
}

static void pwm_timer_stop(RK3588PWMChannel *channel)
{
    timer_del(channel->timer);
    channel->running = false;
    channel->timer_at_period_end = false;
}

static void pwm_channel_stop(RK3588PWMChannel *channel, uint32_t ctrl)
{
    pwm_timer_stop(channel);
    channel->active_ctrl = ctrl;
    channel->cycle_start_ns = 0;
    pwm_set_output(channel, !!(ctrl & PWM_CTRL_INACTIVE_POL));
}

static void pwm_schedule_duty(RK3588PWMChannel *channel)
{
    channel->timer_at_period_end = false;
    timer_mod(channel->timer,
              channel->cycle_start_ns +
              pwm_ticks_to_ns(channel->active_duty,
                              channel->active_root_hz,
                              channel->active_divider));
}

static void pwm_schedule_period(RK3588PWMChannel *channel)
{
    channel->timer_at_period_end = true;
    timer_mod(channel->timer,
              channel->cycle_start_ns +
              pwm_ticks_to_ns(channel->active_period,
                              channel->active_root_hz,
                              channel->active_divider));
}

static bool pwm_channel_is_flat(RK3588PWMChannel *channel)
{
    return channel->active_duty == 0 ||
           channel->active_duty == channel->active_period;
}

static void pwm_schedule_flat_update(RK3588PWMChannel *channel)
{
    int64_t boundary_ns;
    int64_t elapsed_ns;
    int64_t now_ns;
    int64_t period_ns;

    /* Flat outputs need a timer only to commit staged updates. */
    if (!channel->running || !pwm_channel_is_flat(channel) ||
        timer_pending(channel->timer)) {
        return;
    }

    period_ns = pwm_ticks_to_ns(channel->active_period,
                                channel->active_root_hz,
                                channel->active_divider);
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    boundary_ns = channel->cycle_start_ns + period_ns;
    if (boundary_ns <= now_ns) {
        elapsed_ns = now_ns - channel->cycle_start_ns;
        boundary_ns += (elapsed_ns / period_ns) * period_ns;
    }

    channel->cycle_start_ns = boundary_ns - period_ns;
    channel->timer_at_period_end = true;
    timer_mod(channel->timer, boundary_ns);
}

static bool pwm_channel_mode_supported(RK3588PWMChannel *channel,
                                       uint32_t ctrl)
{
    unsigned int index = channel - channel->parent->channels;
    uint32_t mode = ctrl & PWM_CTRL_MODE_MASK;

    if (mode == PWM_CTRL_MODE_ONESHOT) {
        qemu_log_mask(LOG_UNIMP,
                      "rk3588-pwm: channel %u: one-shot mode is not modeled\n",
                      index);
        return false;
    }
    if (mode == PWM_CTRL_MODE_CAPTURE) {
        qemu_log_mask(LOG_UNIMP,
                      "rk3588-pwm: channel %u: capture mode is not modeled\n",
                      index);
        return false;
    }
    if (mode == PWM_CTRL_MODE_RESERVED) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3588-pwm: channel %u: reserved mode selected\n",
                      index);
        return false;
    }

    if (ctrl & PWM_CTRL_CH_CNT_EN) {
        qemu_log_mask(LOG_UNIMP,
                      "rk3588-pwm: channel %u: "
                      "ch_cnt_en is not modeled\n", index);
        return false;
    }

    if (ctrl & PWM_CTRL_OUTPUT_MODE) {
        qemu_log_mask(LOG_UNIMP,
                      "rk3588-pwm: channel %u: "
                      "center-aligned output is not modeled\n", index);
        return false;
    }

    return true;
}

static void pwm_channel_load_registers(RK3588PWMChannel *channel)
{
    RK3588PWMState *s = channel->parent;
    unsigned int index = channel - s->channels;

    channel->active_period =
        pwm_reg_read(s, pwm_channel_offset(index, PWM_PERIOD));
    channel->active_duty =
        pwm_reg_read(s, pwm_channel_offset(index, PWM_DUTY));
    channel->active_ctrl =
        pwm_reg_read(s, pwm_channel_offset(index, PWM_CTRL));
}

static bool pwm_channel_begin_period(RK3588PWMChannel *channel, bool commit,
                                     int64_t start_ns)
{
    RK3588PWMState *s = channel->parent;
    unsigned int index = channel - s->channels;

    if (commit) {
        pwm_channel_load_registers(channel);
    }

    if (!pwm_channel_mode_supported(channel, channel->active_ctrl)) {
        pwm_channel_stop(channel, channel->active_ctrl);
        return false;
    }

    if (!channel->active_period) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3588-pwm: channel %u: period must be non-zero\n",
                      index);
        pwm_channel_stop(channel, channel->active_ctrl);
        return false;
    }

    if (channel->active_duty > channel->active_period) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3588-pwm: channel %u: duty exceeds period\n",
                      index);
        pwm_channel_stop(channel, channel->active_ctrl);
        return false;
    }

    channel->cycle_start_ns = start_ns;
    pwm_effective_clock(s, channel->active_ctrl, &channel->active_root_hz,
                        &channel->active_divider);
    channel->running = true;

    if (channel->active_duty == 0) {
        channel->timer_at_period_end = false;
        pwm_set_output(channel, !pwm_polarity(channel));
        return true;
    }

    if (channel->active_duty == channel->active_period) {
        channel->timer_at_period_end = false;
        pwm_set_output(channel, pwm_polarity(channel));
        return true;
    }

    pwm_set_output(channel, pwm_polarity(channel));
    pwm_schedule_duty(channel);
    return true;
}

static void pwm_channel_timer(void *opaque)
{
    RK3588PWMChannel *channel = opaque;

    if (!channel->running) {
        return;
    }

    if (!channel->timer_at_period_end) {
        pwm_set_output(channel, !pwm_polarity(channel));
        pwm_schedule_period(channel);
        return;
    }

    int64_t next_cycle_start_ns =
        channel->cycle_start_ns +
        pwm_ticks_to_ns(channel->active_period,
                        channel->active_root_hz,
                        channel->active_divider);

    if (!pwm_channel_begin_period(
            channel,
            !(pwm_reg_read(
                  channel->parent,
                  pwm_channel_offset(channel - channel->parent->channels,
                                     PWM_CTRL)) &
              PWM_CTRL_CONLOCK),
            next_cycle_start_ns)) {
        return;
    }
}

static uint32_t pwm_channel_counter(RK3588PWMChannel *channel)
{
    uint64_t ticks;
    int64_t now;

    if (!channel->running || !channel->active_period) {
        return 0;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ticks = muldiv64(now - channel->cycle_start_ns,
                     channel->active_root_hz, NANOSECONDS_PER_SECOND);
    return ticks / channel->active_divider % channel->active_period;
}

static uint64_t pwm_counter_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3588PWMState *s = RK3588_PWM(reg->opaque);
    unsigned int channel = reg->access->addr / PWM_CHANNEL_STRIDE;

    return pwm_channel_counter(&s->channels[channel]);
}

static uint64_t pwm_intsts_post_read(RegisterInfo *reg, uint64_t val)
{
    return val & PWM_INTSTS_READ_MASK;
}

static void pwm_ctrl_write(RK3588PWMChannel *channel, uint32_t value)
{
    bool was_running = channel->running;

    value &= PWM_CTRL_WRITE_MASK;

    if (!(value & PWM_CTRL_PWM_EN)) {
        pwm_channel_stop(channel, value);
    } else if (!was_running) {
        pwm_channel_begin_period(channel, true,
                                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    } else if (!pwm_channel_mode_supported(channel, value)) {
        pwm_channel_stop(channel, value);
    } else {
        pwm_schedule_flat_update(channel);
    }
}

static void pwm_channel_reg_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3588PWMState *s = RK3588_PWM(reg->opaque);
    unsigned int channel = reg->access->addr / PWM_CHANNEL_STRIDE;

    pwm_schedule_flat_update(&s->channels[channel]);
}

static void pwm_ctrl_reg_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3588PWMState *s = RK3588_PWM(reg->opaque);
    unsigned int channel = reg->access->addr / PWM_CHANNEL_STRIDE;

    pwm_ctrl_write(&s->channels[channel], val);
}

#define PWM_CNT_REG(n)                                                 \
    {                                                                  \
        .name = "PWM" #n "_CNT",                                      \
        .addr = PWM_CHANNEL_STRIDE * n + PWM_CNT,                     \
        .ro = UINT32_MAX,                                              \
        .post_read = pwm_counter_post_read,                            \
    }

#define PWM_PERIOD_REG(n)                                              \
    {                                                                  \
        .name = "PWM" #n "_PERIOD",                                   \
        .addr = PWM_CHANNEL_STRIDE * n + PWM_PERIOD,                  \
        .post_write = pwm_channel_reg_post_write,                      \
    }

#define PWM_DUTY_REG(n)                                                \
    {                                                                  \
        .name = "PWM" #n "_DUTY",                                     \
        .addr = PWM_CHANNEL_STRIDE * n + PWM_DUTY,                    \
        .post_write = pwm_channel_reg_post_write,                      \
    }

#define PWM_CTRL_REG(n)                                                \
    {                                                                  \
        .name = "PWM" #n "_CTRL",                                     \
        .addr = PWM_CHANNEL_STRIDE * n + PWM_CTRL,                    \
        .rsvd = ~PWM_CTRL_WRITE_MASK,                                 \
        .post_write = pwm_ctrl_reg_post_write,                         \
    }

#define PWM_PWRMATCH_VALUE_REG(n)                                     \
    {                                                                  \
        .name = "PWM_PWRMATCH_VALUE" #n,                              \
        .addr = PWM_PWRMATCH_VALUE0 + n * 4,                          \
        .unimp = UINT32_MAX,                                          \
    }

static const RegisterAccessInfo rk3588_pwm_regs_info[] = {
    PWM_CNT_REG(0),
    PWM_PERIOD_REG(0),
    PWM_DUTY_REG(0),
    PWM_CTRL_REG(0),
    PWM_CNT_REG(1),
    PWM_PERIOD_REG(1),
    PWM_DUTY_REG(1),
    PWM_CTRL_REG(1),
    PWM_CNT_REG(2),
    PWM_PERIOD_REG(2),
    PWM_DUTY_REG(2),
    PWM_CTRL_REG(2),
    PWM_CNT_REG(3),
    PWM_PERIOD_REG(3),
    PWM_DUTY_REG(3),
    PWM_CTRL_REG(3),
    {
        .name = "PWM_INTSTS",
        .addr = PWM_INTSTS,
        .ro = ~PWM_INTSTS_W1C_MASK,
        .w1c = PWM_INTSTS_W1C_MASK,
        .post_read = pwm_intsts_post_read,
    }, {
        .name = "PWM_INT_EN",
        .addr = PWM_INT_EN,
        .rsvd = ~PWM_INT_EN_MASK,
    }, {
        .name = "PWM_FIFO_CTRL",
        .addr = PWM_FIFO_CTRL,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_FIFO_INTSTS",
        .addr = PWM_FIFO_INTSTS,
        .ro = UINT32_MAX,
        .w1c = PWM_FIFO_INTSTS_W1C_MASK,
        .reset = PWM_FIFO_INTSTS_EMPTY,
    }, {
        .name = "PWM_FIFO_TOUTTHR",
        .addr = PWM_FIFO_TOUTTHR,
        .rsvd = ~0xfffff,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_VERSION_ID",
        .addr = PWM_VERSION_ID,
        .ro = UINT32_MAX,
        .reset = RK3588_PWM_VERSION_ID,
    }, {
        .name = "PWM_FIFO",
        .addr = PWM_FIFO,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_PWRMATCH_CTRL",
        .addr = PWM_PWRMATCH_CTRL,
        .rsvd = ~(BIT(15) | BIT(11) | BIT(7) | BIT(3)),
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_PWRMATCH_LPRE",
        .addr = PWM_PWRMATCH_LPRE,
        .reset = 0x238c22c4,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_PWRMATCH_HPRE",
        .addr = PWM_PWRMATCH_HPRE,
        .reset = 0x11f81130,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_PWRMATCH_LD",
        .addr = PWM_PWRMATCH_LD,
        .reset = 0x029401cc,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_PWRMATCH_HD_ZERO",
        .addr = PWM_PWRMATCH_HD_ZERO,
        .reset = 0x029401cc,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_PWRMATCH_HD_ONE",
        .addr = PWM_PWRMATCH_HD_ONE,
        .reset = 0x06fe0636,
        .unimp = UINT32_MAX,
    },
    PWM_PWRMATCH_VALUE_REG(0),
    PWM_PWRMATCH_VALUE_REG(1),
    PWM_PWRMATCH_VALUE_REG(2),
    PWM_PWRMATCH_VALUE_REG(3),
    PWM_PWRMATCH_VALUE_REG(4),
    PWM_PWRMATCH_VALUE_REG(5),
    PWM_PWRMATCH_VALUE_REG(6),
    PWM_PWRMATCH_VALUE_REG(7),
    PWM_PWRMATCH_VALUE_REG(8),
    PWM_PWRMATCH_VALUE_REG(9),
    {
        .name = "PWM_PWR_CAPTURE",
        .addr = PWM_PWR_CAPTURE,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "PWM_CHANNEL_IO_CTRL",
        .addr = PWM_CHANNEL_IO_CTRL,
        .unimp = UINT32_MAX,
    },
};

static const MemoryRegionOps rk3588_pwm_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void rk3588_pwm_reset(DeviceState *dev)
{
    RK3588PWMState *s = RK3588_PWM(dev);

    for (unsigned int i = 0; i < RK3588_PWM_NUM_CHANNELS; i++) {
        pwm_timer_stop(&s->channels[i]);
    }
    for (unsigned int i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }

    for (unsigned int i = 0; i < RK3588_PWM_NUM_CHANNELS; i++) {
        RK3588PWMChannel *channel = &s->channels[i];

        channel->active_period = 0;
        channel->active_duty = 0;
        channel->active_ctrl = 0;
        channel->active_root_hz = s->clock_hz;
        channel->active_divider = 1;
        channel->cycle_start_ns = 0;
        pwm_set_output(channel, false);
    }
}

static void rk3588_pwm_init(Object *obj)
{
    RK3588PWMState *s = RK3588_PWM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    for (unsigned int i = 0; i < RK3588_PWM_NUM_CHANNELS; i++) {
        s->channels[i].parent = s;
        s->channels[i].timer =
            timer_new_ns(QEMU_CLOCK_VIRTUAL, pwm_channel_timer,
                         &s->channels[i]);
    }

    s->reg_array = register_init_block32(DEVICE(obj), rk3588_pwm_regs_info,
                                         ARRAY_SIZE(rk3588_pwm_regs_info),
                                         s->regs_info, s->regs,
                                         &rk3588_pwm_ops, false,
                                         RK3588_PWM_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
}

static void rk3588_pwm_finalize(Object *obj)
{
    RK3588PWMState *s = RK3588_PWM(obj);

    for (unsigned int i = 0; i < RK3588_PWM_NUM_CHANNELS; i++) {
        timer_free(s->channels[i].timer);
    }
}

static void rk3588_pwm_realize(DeviceState *dev, Error **errp)
{
    RK3588PWMState *s = RK3588_PWM(dev);

    qdev_init_gpio_out_named(dev, s->outputs, "pwm-out",
                             RK3588_PWM_NUM_CHANNELS);
}

static const Property rk3588_pwm_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", RK3588PWMState, clock_hz,
                       RK3588_PWM_DEFAULT_CLOCK_HZ),
    DEFINE_PROP_UINT32("osc-clock-hz", RK3588PWMState, osc_clock_hz,
                       RK3588_PWM_OSC_CLOCK_HZ),
};

static const VMStateDescription vmstate_rk3588_pwm_channel = {
    .name = "rk3588-pwm/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, RK3588PWMChannel),
        VMSTATE_UINT32(active_period, RK3588PWMChannel),
        VMSTATE_UINT32(active_duty, RK3588PWMChannel),
        VMSTATE_UINT32(active_ctrl, RK3588PWMChannel),
        VMSTATE_UINT32(active_root_hz, RK3588PWMChannel),
        VMSTATE_UINT32(active_divider, RK3588PWMChannel),
        VMSTATE_INT64(cycle_start_ns, RK3588PWMChannel),
        VMSTATE_BOOL(running, RK3588PWMChannel),
        VMSTATE_BOOL(timer_at_period_end, RK3588PWMChannel),
        VMSTATE_BOOL(output_level, RK3588PWMChannel),
        VMSTATE_END_OF_LIST()
    },
};

static int rk3588_pwm_post_load(void *opaque, int version_id)
{
    RK3588PWMState *s = opaque;

    for (unsigned int i = 0; i < RK3588_PWM_NUM_CHANNELS; i++) {
        qemu_set_irq(s->outputs[i], s->channels[i].output_level);
    }

    return 0;
}

static const VMStateDescription vmstate_rk3588_pwm = {
    .name = TYPE_RK3588_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rk3588_pwm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3588PWMState,
                             RK3588_PWM_MMIO_SIZE / 4),
        VMSTATE_UINT32(clock_hz, RK3588PWMState),
        VMSTATE_UINT32(osc_clock_hz, RK3588PWMState),
        VMSTATE_STRUCT_ARRAY(channels, RK3588PWMState,
                             RK3588_PWM_NUM_CHANNELS, 0,
                             vmstate_rk3588_pwm_channel, RK3588PWMChannel),
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_pwm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3588_pwm_realize;
    device_class_set_legacy_reset(dc, rk3588_pwm_reset);
    device_class_set_props(dc, rk3588_pwm_properties);
    dc->vmsd = &vmstate_rk3588_pwm;
    dc->desc = "Rockchip RK3588 PWM controller";
    dc->user_creatable = false;
}

static const TypeInfo rk3588_pwm_type_info = {
    .name = TYPE_RK3588_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3588PWMState),
    .instance_init = rk3588_pwm_init,
    .instance_finalize = rk3588_pwm_finalize,
    .class_init = rk3588_pwm_class_init,
};

static void rk3588_pwm_register_types(void)
{
    type_register_static(&rk3588_pwm_type_info);
}

type_init(rk3588_pwm_register_types)

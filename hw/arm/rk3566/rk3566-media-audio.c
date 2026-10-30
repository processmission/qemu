/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RK3566 I2S/TDM and PDM FIFOs, clocking, interrupts and DMA handshakes. */

#include "qemu/osdep.h"
#include "qemu/audio.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

#define TYPE_RK3566_AUDIO "rk3566-audio"
#define TYPE_RK3566_I2S "rk3566-i2s-tdm"
#define TYPE_RK3566_PDM "rk3566-pdm"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566AudioState, RK3566_AUDIO)

enum {
    I2S_TXCR = 0x00, I2S_RXCR = 0x04, I2S_CKR = 0x08,
    I2S_TXFIFOLR = 0x0c, I2S_DMACR = 0x10, I2S_INTCR = 0x14,
    I2S_INTSR = 0x18, I2S_XFER = 0x1c, I2S_CLR = 0x20,
    I2S_TXDR = 0x24, I2S_RXDR = 0x28, I2S_RXFIFOLR = 0x2c,
    I2S_TDM_TXCR = 0x30, I2S_TDM_RXCR = 0x34, I2S_CLKDIV = 0x38,
};

enum {
    PDM_SYSCONFIG = 0x00, PDM_CTRL0 = 0x04, PDM_CTRL1 = 0x08,
    PDM_CLK_CTRL = 0x0c, PDM_HPF_CTRL = 0x10, PDM_FIFO_CTRL = 0x14,
    PDM_DMA_CTRL = 0x18, PDM_INT_EN = 0x1c, PDM_INT_CLR = 0x20,
    PDM_INT_ST = 0x24, PDM_RXFIFO_DATA = 0x30,
};

#define FIFO_WORDS 128
#define PCM_BYTES 16384
#define R(s, address) ((s)->regs[(address) / 4])

struct RK3566AudioState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq dma_busy[2];
    qemu_irq lrck;
    Clock *mclk;
    QEMUTimer *timer;
    AudioBackend *audio_be;
    SWVoiceOut *output;
    SWVoiceIn *input;
    uint32_t regs[0x100 / 4];
    uint32_t tx[FIFO_WORDS];
    uint32_t rx[FIFO_WORDS];
    uint32_t tx_head, tx_count, rx_head, rx_count;
    uint8_t output_pcm[PCM_BYTES];
    uint8_t input_pcm[PCM_BYTES];
    uint32_t output_count, input_count;
    uint32_t tx_partial_bytes, rx_partial_bytes;
    uint64_t transmitted_words, received_words;
    uint64_t mclk_hz;
    uint32_t rate;
    bool pdm;
    bool capture;
    bool lrck_level;
};

static unsigned audio_channels(RK3566AudioState *s, bool receive)
{
    if (s->pdm) {
        return 2 * ctpop32(extract32(R(s, PDM_CTRL0), 27, 4));
    }
    return 2 * (extract32(R(s, receive ? I2S_RXCR : I2S_TXCR), 15, 2) + 1);
}

static unsigned audio_sample_bytes(RK3566AudioState *s, bool receive)
{
    unsigned bits = (R(s, s->pdm ? PDM_CTRL0 :
                      receive ? I2S_RXCR : I2S_TXCR) & 31) + 1;
    return bits <= 8 ? 1 : bits <= 16 ? 2 : 4;
}

static bool audio_tx_enabled(RK3566AudioState *s)
{
    return !s->pdm && (R(s, I2S_XFER) & 1);
}

static bool audio_rx_enabled(RK3566AudioState *s)
{
    return s->pdm ? (R(s, PDM_SYSCONFIG) & BIT(2)) &&
                    (R(s, PDM_CLK_CTRL) & BIT(5)) :
                    (R(s, I2S_XFER) & BIT(1));
}

static void audio_update(RK3566AudioState *s)
{
    if (s->pdm) {
        bool request = (R(s, PDM_DMA_CTRL) & BIT(8)) &&
            s->rx_count >= ((R(s, PDM_DMA_CTRL) & 0x7f) + 1);
        qemu_set_irq(s->dma_busy[0], 1);
        qemu_set_irq(s->dma_busy[1], !request);
        qemu_set_irq(s->irq, (R(s, PDM_INT_ST) & R(s, PDM_INT_EN)) != 0);
        return;
    }
    uint32_t dma = R(s, I2S_DMACR);
    uint32_t control = R(s, I2S_INTCR);
    unsigned tx_lanes = audio_channels(s, false) / 2;
    unsigned rx_lanes = audio_channels(s, true) / 2;
    bool tx_request = (dma & BIT(8)) &&
        s->tx_count <= (dma & 31) * tx_lanes;
    bool rx_request = (dma & BIT(24)) &&
        s->rx_count >= (extract32(dma, 16, 5) + 1) * rx_lanes;
    uint32_t status = R(s, I2S_INTSR) & (BIT(1) | BIT(17));
    if (s->tx_count <= (extract32(control, 4, 5) + 1) * tx_lanes) {
        status |= BIT(0);
    }
    if (s->rx_count >= (extract32(control, 20, 5) + 1) * rx_lanes) {
        status |= BIT(16);
    }
    R(s, I2S_INTSR) = status;
    qemu_set_irq(s->dma_busy[0], !tx_request);
    qemu_set_irq(s->dma_busy[1], !rx_request);
    qemu_set_irq(s->irq, (status & control & 0x30003) != 0);
}

static void audio_output(void *opaque, int available)
{
    RK3566AudioState *s = opaque;
    size_t bytes = MIN(available, s->output_count);
    size_t sent = audio_be_write(s->audio_be, s->output, s->output_pcm, bytes);
    memmove(s->output_pcm, s->output_pcm + sent, s->output_count - sent);
    s->output_count -= sent;
}

static void audio_input(void *opaque, int available)
{
    RK3566AudioState *s = opaque;
    size_t bytes = MIN(available, PCM_BYTES - s->input_count);
    s->input_count += audio_be_read(s->audio_be, s->input,
                                   s->input_pcm + s->input_count, bytes);
}

static uint32_t audio_rate(RK3566AudioState *s)
{
    uint64_t hz = clock_get_hz(s->mclk);
    if (s->pdm) {
        unsigned numerator = R(s, PDM_CTRL1) >> 16;
        unsigned denominator = R(s, PDM_CTRL1) & 0xffff;
        unsigned cic = R(s, PDM_CLK_CTRL) & 3;
        unsigned selector = extract32(R(s, PDM_CTRL0), 5, 3);
        static const unsigned decimation[] = { 512, 256, 128, 128, 64, 32 };
        if (!denominator || selector >= ARRAY_SIZE(decimation)) {
            return 0;
        }
        /* The RK3568/RV1126 path uses a fractional PDM clock and CIC filter. */
        return hz * numerator / denominator * (1u << cic) /
               decimation[selector];
    }
    unsigned divisor = (R(s, I2S_CLKDIV) & 0xff) + 1;
    unsigned frame = (R(s, I2S_CKR) & 0xff) + 1;
    return hz / divisor / frame;
}

static void audio_configure(RK3566AudioState *s)
{
    bool tx = audio_tx_enabled(s);
    bool rx = audio_rx_enabled(s);
    s->rate = audio_rate(s);
    if (s->audio_be && s->rate && s->rate <= 384000) {
        struct audsettings settings = { .freq = s->rate, .big_endian = false };
        if (!s->pdm) {
            unsigned bytes = audio_sample_bytes(s, false);
            settings.nchannels = audio_channels(s, false);
            settings.fmt = bytes == 1 ? AUDIO_FORMAT_S8 :
                           bytes == 2 ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_S32;
            s->output = audio_be_open_out(s->audio_be, s->output, "rk3566-i2s",
                                          s, audio_output, &settings);
            audio_be_set_active_out(s->audio_be, s->output, tx);
        }
        if (s->capture && audio_channels(s, true)) {
            unsigned bytes = audio_sample_bytes(s, true);
            settings.nchannels = audio_channels(s, true);
            settings.fmt = bytes == 1 ? AUDIO_FORMAT_S8 :
                           bytes == 2 ? AUDIO_FORMAT_S16 : AUDIO_FORMAT_S32;
            s->input = audio_be_open_in(s->audio_be, s->input, "rk3566-input",
                                        s, audio_input, &settings);
            audio_be_set_active_in(s->audio_be, s->input, rx);
        }
    }
    timer_del(s->timer);
    if ((tx || rx) && s->rate && s->rate <= 384000) {
        timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     NANOSECONDS_PER_SECOND / (2 * s->rate));
    }
    audio_update(s);
}

static void audio_receive_word(RK3566AudioState *s, uint32_t value)
{
    if (s->rx_count == FIFO_WORDS) {
        if (!s->pdm) {
            R(s, I2S_INTSR) |= BIT(17);
        }
        return;
    }
    s->rx[(s->rx_head + s->rx_count++) % FIFO_WORDS] = value;
    s->received_words++;
}

static void audio_tick(void *opaque)
{
    RK3566AudioState *s = opaque;
    bool tx = audio_tx_enabled(s);
    bool rx = audio_rx_enabled(s);
    s->lrck_level = !s->lrck_level;
    qemu_set_irq(s->lrck, s->lrck_level);
    if (s->lrck_level) {
        bool loopback = !s->pdm && (R(s, I2S_XFER) & (BIT(2) | BIT(3)));
        s->tx_partial_bytes += audio_channels(s, false) *
                               audio_sample_bytes(s, false);
        s->rx_partial_bytes += audio_channels(s, true) *
                               audio_sample_bytes(s, true);
        unsigned tx_words = s->tx_partial_bytes / 4;
        unsigned rx_words = s->rx_partial_bytes / 4;
        s->tx_partial_bytes %= 4;
        s->rx_partial_bytes %= 4;
        if (tx) {
            for (unsigned i = 0; i < tx_words; i++) {
                if (!s->tx_count) {
                    R(s, I2S_INTSR) |= BIT(1);
                    break;
                }
                uint32_t word = s->tx[s->tx_head++ % FIFO_WORDS];
                s->tx_count--;
                s->transmitted_words++;
                if (s->output && s->output_count <= PCM_BYTES - 4) {
                    stl_le_p(s->output_pcm + s->output_count, word);
                    s->output_count += 4;
                }
                if (loopback && rx) {
                    audio_receive_word(s, word);
                }
            }
        }
        if (rx && !loopback) {
            unsigned bytes = MIN(rx_words * 4, s->input_count) & ~3u;
            for (unsigned i = 0; i < bytes; i += 4) {
                audio_receive_word(s, ldl_le_p(s->input_pcm + i));
            }
            memmove(s->input_pcm, s->input_pcm + bytes, s->input_count - bytes);
            s->input_count -= bytes;
        }
    }
    audio_update(s);
    if ((tx || rx) && s->rate) {
        timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     NANOSECONDS_PER_SECOND / (2 * s->rate));
    }
}

static uint64_t audio_read(void *opaque, hwaddr address, unsigned size)
{
    RK3566AudioState *s = opaque;
    if (address >= sizeof(s->regs)) {
        return 0;
    }
    if (address == (s->pdm ? PDM_RXFIFO_DATA : I2S_RXDR)) {
        uint32_t value = 0;
        if (s->rx_count) {
            value = s->rx[s->rx_head++ % FIFO_WORDS];
            s->rx_count--;
        }
        audio_update(s);
        return value;
    }
    if (s->pdm && address == PDM_FIFO_CTRL) {
        return s->rx_count;
    }
    if (!s->pdm && (address == I2S_TXFIFOLR || address == I2S_RXFIFOLR)) {
        bool receive = address == I2S_RXFIFOLR;
        unsigned count = receive ? s->rx_count : s->tx_count;
        unsigned lanes = audio_channels(s, receive) / 2;
        uint32_t value = 0;
        for (unsigned lane = 0; lane < lanes; lane++) {
            value |= MIN((count + lanes - 1 - lane) / lanes, 32) << (6 * lane);
        }
        return value;
    }
    return R(s, address);
}

static void audio_write(void *opaque, hwaddr address, uint64_t value,
                        unsigned size)
{
    RK3566AudioState *s = opaque;
    if (address >= sizeof(s->regs)) {
        qemu_log_mask(LOG_UNIMP, "%s: register 0x%" HWADDR_PRIx "\n",
                      object_get_typename(OBJECT(s)), address);
        return;
    }
    if (s->pdm) {
        if (address == PDM_SYSCONFIG && (value & 1)) {
            s->rx_head = s->rx_count = 0;
            value &= ~1;
        } else if (address == PDM_INT_CLR) {
            R(s, PDM_INT_ST) &= ~value;
            value = 0;
        } else if (address == PDM_INT_ST || address == PDM_RXFIFO_DATA ||
                   address == PDM_FIFO_CTRL) {
            return;
        }
    } else {
        if (address == I2S_TXDR) {
            if (s->tx_count < 32 * audio_channels(s, false) / 2) {
                s->tx[(s->tx_head + s->tx_count++) % FIFO_WORDS] = value;
            }
            audio_update(s);
            return;
        }
        if (address == I2S_CLR) {
            if (value & 1) {
                s->tx_head = s->tx_count = 0;
            }
            if (value & 2) {
                s->rx_head = s->rx_count = 0;
            }
            value = 0;
        } else if (address == I2S_INTCR) {
            if (value & BIT(2)) {
                R(s, I2S_INTSR) &= ~BIT(1);
            }
            if (value & BIT(18)) {
                R(s, I2S_INTSR) &= ~BIT(17);
            }
            value &= ~(BIT(2) | BIT(18));
        } else if (address == I2S_INTSR || address == I2S_TXFIFOLR ||
                   address == I2S_RXFIFOLR || address == I2S_RXDR) {
            return;
        }
    }
    R(s, address) = value;
    if ((s->pdm && address <= PDM_CLK_CTRL) ||
        (!s->pdm && (address == I2S_TXCR || address == I2S_RXCR ||
                     address == I2S_CKR || address == I2S_XFER ||
                     address == I2S_CLKDIV))) {
        audio_configure(s);
    } else {
        audio_update(s);
    }
}

static const MemoryRegionOps audio_ops = {
    .read = audio_read, .write = audio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void audio_clock_update(void *opaque, ClockEvent event)
{
    RK3566AudioState *s = opaque;
    if (s->timer) {
        audio_configure(s);
    }
}

static void audio_reset(DeviceState *device)
{
    RK3566AudioState *s = RK3566_AUDIO(device);
    memset(s->regs, 0, sizeof(s->regs));
    s->tx_head = s->tx_count = s->rx_head = s->rx_count = 0;
    s->input_count = s->output_count = 0;
    s->tx_partial_bytes = s->rx_partial_bytes = 0;
    s->transmitted_words = s->received_words = 0;
    s->lrck_level = false;
    qemu_set_irq(s->lrck, 0);
    if (s->pdm) {
        R(s, PDM_CTRL0) = 0x78000017;
        R(s, PDM_CTRL1) = 0x0bb8ea60;
        R(s, PDM_CLK_CTRL) = 0x0000e401;
        R(s, PDM_DMA_CTRL) = 0x1f;
    } else {
        R(s, I2S_TXCR) = 0x7200000f;
        R(s, I2S_RXCR) = 0x01c8000f;
        R(s, I2S_CKR) = 0x1f1f;
        R(s, I2S_DMACR) = 0x001f0000;
        R(s, I2S_INTCR) = 0x01f00000;
        R(s, I2S_TDM_TXCR) = R(s, I2S_TDM_RXCR) = 0x3eff;
        R(s, I2S_CLKDIV) = 0x0707;
    }
    audio_configure(s);
}

static void audio_realize(DeviceState *device, Error **errp)
{
    RK3566AudioState *s = RK3566_AUDIO(device);
    if (!clock_has_source(s->mclk)) {
        clock_set_hz(s->mclk, s->mclk_hz);
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &audio_ops, s,
                          object_get_typename(OBJECT(s)), 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qdev_init_gpio_out_named(device, s->dma_busy, "dma-busy", 2);
    qdev_init_gpio_out_named(device, &s->lrck, "lrck", 1);
    audio_reset(device);
}

static void audio_init(Object *object)
{
    RK3566AudioState *s = RK3566_AUDIO(object);
    s->pdm = object_dynamic_cast(object, TYPE_RK3566_PDM) != NULL;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, audio_tick, s);
    s->mclk = qdev_init_clock_in(DEVICE(s), "mclk", audio_clock_update,
                                s, ClockUpdate);
}

static void audio_finalize(Object *object)
{
    RK3566AudioState *s = RK3566_AUDIO(object);
    timer_free(s->timer);
    if (s->output) {
        audio_be_close_out(s->audio_be, s->output);
    }
    if (s->input) {
        audio_be_close_in(s->audio_be, s->input);
    }
}

static int audio_post_load(void *opaque, int version)
{
    RK3566AudioState *s = opaque;
    if (s->tx_count > FIFO_WORDS || s->rx_count > FIFO_WORDS ||
        s->input_count > PCM_BYTES || s->output_count > PCM_BYTES ||
        s->tx_partial_bytes > 3 || s->rx_partial_bytes > 3) {
        return -EINVAL;
    }
    audio_configure(s);
    return 0;
}

static const VMStateDescription audio_vmstate = {
    .name = TYPE_RK3566_AUDIO, .version_id = 1, .minimum_version_id = 1,
    .post_load = audio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3566AudioState, 0x100 / 4),
        VMSTATE_UINT32_ARRAY(tx, RK3566AudioState, FIFO_WORDS),
        VMSTATE_UINT32_ARRAY(rx, RK3566AudioState, FIFO_WORDS),
        VMSTATE_UINT32(tx_head, RK3566AudioState),
        VMSTATE_UINT32(tx_count, RK3566AudioState),
        VMSTATE_UINT32(rx_head, RK3566AudioState),
        VMSTATE_UINT32(rx_count, RK3566AudioState),
        VMSTATE_UINT32(tx_partial_bytes, RK3566AudioState),
        VMSTATE_UINT32(rx_partial_bytes, RK3566AudioState),
        VMSTATE_UINT8_ARRAY(input_pcm, RK3566AudioState, PCM_BYTES),
        VMSTATE_UINT8_ARRAY(output_pcm, RK3566AudioState, PCM_BYTES),
        VMSTATE_UINT32(input_count, RK3566AudioState),
        VMSTATE_UINT32(output_count, RK3566AudioState),
        VMSTATE_BOOL(lrck_level, RK3566AudioState),
        VMSTATE_UINT64(transmitted_words, RK3566AudioState),
        VMSTATE_UINT64(received_words, RK3566AudioState),
        VMSTATE_CLOCK(mclk, RK3566AudioState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(RK3566AudioState, audio_be),
    DEFINE_PROP_BOOL("capture", RK3566AudioState, capture, false),
    DEFINE_PROP_UINT64("mclk-hz", RK3566AudioState, mclk_hz, 24000000),
};

static void audio_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    device->realize = audio_realize;
    device->vmsd = &audio_vmstate;
    device_class_set_legacy_reset(device, audio_reset);
    device_class_set_props(device, audio_properties);
}

static const TypeInfo audio_types[] = {
    { .name = TYPE_RK3566_AUDIO, .parent = TYPE_SYS_BUS_DEVICE,
      .instance_size = sizeof(RK3566AudioState), .abstract = true,
      .instance_init = audio_init, .instance_finalize = audio_finalize,
      .class_init = audio_class_init },
    { .name = TYPE_RK3566_I2S, .parent = TYPE_RK3566_AUDIO },
    { .name = TYPE_RK3566_PDM, .parent = TYPE_RK3566_AUDIO },
};

DEFINE_TYPES(audio_types)

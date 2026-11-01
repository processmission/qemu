/*
 * RK3566 ISP
 *
 * RKISP 2.1 capture from external UYVY frames, including the main and
 * self paths, shadowed register pairs and the DMA write-out.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#define TYPE_RK3566_ISP "rk3566-isp"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566ISPState, RK3566_ISP)
#define REG(s, address) ((s)->regs[(address) / 4])

enum {
    ISP_CTRL = 0x400, ISP_H_SIZE = 0x59c, ISP_V_SIZE = 0x5a0,
    ISP_IMSC = 0x5bc, ISP_RIS = 0x5c0, ISP_MIS = 0x5c4,
    ISP_ICR = 0x5c8, ISP_ISR = 0x5cc, ISP_FRAME_COUNT = 0x644,
    MI_CTRL = 0x1400, MI_INIT = 0x1404, MI_CTRL_SHD = 0x1474,
    MI_IMSC = 0x14f8, MI_RIS = 0x14fc, MI_MIS = 0x1500,
    MI_ICR = 0x1504, MI_ISR = 0x1508,
};

#define ISP_REG_WINDOW 0x10000
#define ISP_REG_BANK_SIZE 0x1600
#define ISP_NUM_REGS (ISP_REG_BANK_SIZE / 4)

/* ISP 3A measurement blocks (ISP v2.1 register layout). */
#define ISP_ISP3A_IMSC        0x05d0
#define ISP_ISP3A_RIS         0x05d4
#define ISP_ISP3A_MIS         0x05d8
#define ISP_ISP3A_ICR         0x05dc

#define ISP_RAWAE_BIG_CTRL    0x0000
#define ISP_RAWAE_BIG_WND1_SUMR 0x0030
#define ISP_RAWAE_BIG_RO_MEAN 0x0080
#define ISP_RAWAE_BIG1_BASE   0x4400
#define ISP_RAWAE_BIG2_BASE   0x4600
#define ISP_RAWAE_BIG3_BASE   0x4700

#define ISP_RAWHIST_BIG_CTRL  0x0000
#define ISP_RAWHIST_BIG_RO_BIN 0x0080
#define ISP_RAWHIST_BIG1_BASE 0x4800
#define ISP_RAWHIST_BIG2_BASE 0x4a00
#define ISP_RAWHIST_BIG3_BASE 0x4b00

#define ISP_RAWAF_BASE        0x4d00
#define ISP_RAWAF_CTRL        0x0000
#define ISP_RAWAF_INT_LINE    0x0014
#define ISP_RAWAF_SUM_A       0x0024
#define ISP_RAWAF_SUM_B       0x0028
#define ISP_RAWAF_LUM_A       0x002c
#define ISP_RAWAF_LUM_B       0x0030
#define ISP_RAWAF_INT_STATE   0x0058
#define ISP_RAWAF_RAM_DATA    0x005c

#define ISP_RAWAWB_BASE       0x5000
#define ISP_RAWAWB_CTRL       0x0000
#define ISP_RAWAWB_Y_HIST01   0x0620
#define ISP_RAWAWB_SUM_RGAIN_NOR_0 0x0340
#define ISP_RAWAWB_SUM_BGAIN_NOR_0 0x0348
#define ISP_RAWAWB_WP_NUM_NOR_0    0x034c
#define ISP_RAWAWB_SUM_RGAIN_BIG_0 0x0350
#define ISP_RAWAWB_SUM_BGAIN_BIG_0 0x0358
#define ISP_RAWAWB_WP_NUM_BIG_0    0x035c
#define ISP_RAWAWB_RAM_DATA_BASE   0x0700

#define ISP_CTRL_SWS_CFG      0x001c
#define ISP_SW_3A_DDR_WRITE_EN BIT(24)
#define ISP_MI_DBR_WR_SIZE    0x1964
#define ISP_MI_SWS_3A_WR_BASE 0x197c
#define ISP_STATS_DDR_SIZE    0x3500
#define ISP_ONE_3A_BLS_BASE   0x3000
#define ISP2X_FRAME_BIT       BIT(1)
#define ISP_3A_MEAS_DONE      BIT(31)
#define ISP_3A_AE_BLOCKS      3
#define ISP_3A_AE_WINDOWS     4
#define ISP_3A_AE_MEANS       225
#define ISP_3A_HIST_BINS      256
#define ISP_3A_AWB_SUMS       7
#define ISP_3A_AWB_HIST_BINS  8
#define ISP_3A_AWB_RAM        225
#define ISP_3A_AF_LINES       15
#define ISP_3A_AF_COLUMNS     15

#define ISP_3A_RIS_RAWAE_BIG  BIT(0)
#define ISP_3A_RIS_RAWHIST_BIG BIT(4)
#define ISP_3A_RIS_RAWAF       BIT(10)
#define ISP_3A_RIS_RAWAWB      BIT(11)
#define ISP_3A_RIS_ALL         (ISP_3A_RIS_RAWAE_BIG | \
                                ISP_3A_RIS_RAWHIST_BIG | \
                                ISP_3A_RIS_RAWAF | ISP_3A_RIS_RAWAWB)

struct RK3566ISPState {
    SysBusDevice parent_obj;
    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[ISP_NUM_REGS];
    MemoryRegion *dma;
    AddressSpace dma_as;
    CharFrontend camera;
    qemu_irq irq[3];
    QEMUTimer *frame_end;
    uint32_t regs[ISP_REG_WINDOW / 4];
    uint32_t active[ISP_REG_BANK_SIZE / 4];
    uint32_t width, height;
    uint32_t input_size, input_count;
    uint64_t input_frames;
    uint32_t pixel_rate_hz;
    uint8_t *input, *frame;
    bool streaming, link_enabled, busy;
    bool synth;
    QEMUTimer *synth_timer;
    bool stats_ready;
    uint32_t stat_ae_sum[ISP_3A_AE_BLOCKS][ISP_3A_AE_WINDOWS][3];
    uint32_t stat_ae_mean[ISP_3A_AE_BLOCKS][ISP_3A_AE_MEANS];
    uint32_t stat_hist[ISP_3A_AE_BLOCKS][ISP_3A_HIST_BINS];
    uint32_t stat_awb_yhist[ISP_3A_AWB_HIST_BINS];
    uint32_t stat_awb_sum_r_nor[ISP_3A_AWB_SUMS];
    uint32_t stat_awb_sum_b_nor[ISP_3A_AWB_SUMS];
    uint32_t stat_awb_wp_nor[ISP_3A_AWB_SUMS];
    uint32_t stat_awb_sum_r_big[ISP_3A_AWB_SUMS];
    uint32_t stat_awb_sum_b_big[ISP_3A_AWB_SUMS];
    uint32_t stat_awb_wp_big[ISP_3A_AWB_SUMS];
    uint32_t stat_awb_ram[ISP_3A_AWB_RAM * 2];
    uint32_t stat_af_sum[2];
    uint32_t stat_af_lum[2];
    uint32_t stat_af_ram[ISP_3A_AF_LINES * ISP_3A_AF_COLUMNS];
    unsigned ae_mean_cursor[ISP_3A_AE_BLOCKS];
    unsigned hist_cursor[ISP_3A_AE_BLOCKS];
    unsigned awb_ram_cursor;
    unsigned af_ram_cursor;
    uint8_t *stats_ddr;
};

static void isp_3a_reset(RK3566ISPState *s)
{
    s->stats_ready = false;
    REG(s, ISP_ISP3A_RIS) = 0;
    memset(s->stat_ae_sum, 0, sizeof(s->stat_ae_sum));
    memset(s->stat_ae_mean, 0, sizeof(s->stat_ae_mean));
    memset(s->stat_hist, 0, sizeof(s->stat_hist));
    memset(s->stat_awb_yhist, 0, sizeof(s->stat_awb_yhist));
    memset(s->stat_awb_sum_r_nor, 0, sizeof(s->stat_awb_sum_r_nor));
    memset(s->stat_awb_sum_b_nor, 0, sizeof(s->stat_awb_sum_b_nor));
    memset(s->stat_awb_wp_nor, 0, sizeof(s->stat_awb_wp_nor));
    memset(s->stat_awb_sum_r_big, 0, sizeof(s->stat_awb_sum_r_big));
    memset(s->stat_awb_sum_b_big, 0, sizeof(s->stat_awb_sum_b_big));
    memset(s->stat_awb_wp_big, 0, sizeof(s->stat_awb_wp_big));
    memset(s->stat_af_sum, 0, sizeof(s->stat_af_sum));
    memset(s->stat_af_lum, 0, sizeof(s->stat_af_lum));
    memset(s->stat_af_ram, 0, sizeof(s->stat_af_ram));
    memset(s->ae_mean_cursor, 0, sizeof(s->ae_mean_cursor));
    memset(s->hist_cursor, 0, sizeof(s->hist_cursor));
    s->awb_ram_cursor = 0;
    s->af_ram_cursor = 0;
}

static uint32_t isp_3a_pack_mean(unsigned y, int du, int dv)
{
    unsigned g = MIN(y, 0xfff);
    int r = (int)y + (dv * 4) / 8;
    int b = (int)y - (du * 4) / 8;
    r = CLAMP(r, 0, 0x3ff);
    b = CLAMP(b, 0, 0x3ff);
    return (g & 0xfff) | ((b & 0x3ff) << 12) | ((r & 0x3ff) << 22);
}

static void isp_3a_measure(RK3566ISPState *s)
{
    const uint8_t *frame = s->frame;
    uint32_t hist[ISP_3A_HIST_BINS] = {};
    uint64_t sum_y = 0;
    unsigned samples = 0;
    int sum_u = 0, sum_v = 0;
    unsigned step = 8;

    for (unsigned y = 0; y < s->height; y += 2) {
        for (unsigned x = 0; x < s->width; x += step) {
            unsigned offset = (y * s->width + x) * 2;
            unsigned luma = frame[offset + 1];
            int u = (int)frame[offset] - 128;
            int v = (int)frame[offset + 2] - 128;

            hist[luma]++;
            sum_y += luma;
            sum_u += u;
            sum_v += v;
            samples++;
        }
    }
    if (!samples) {
        return;
    }

    unsigned mean_y = sum_y / samples;
    int mean_u = sum_u / (int)samples;
    int mean_v = sum_v / (int)samples;
    unsigned half_w = MAX(s->width / 2, 1);
    unsigned half_h = MAX(s->height / 2, 1);
    unsigned win_count = MAX(half_w * half_h / (step * 2), 1);

    for (unsigned blk = 0; blk < ISP_3A_AE_BLOCKS; blk++) {
        for (unsigned win = 0; win < ISP_3A_AE_WINDOWS; win++) {
            unsigned lum = CLAMP(mean_y + (int)win * 4 - 6, 0, 0xff);

            s->stat_ae_sum[blk][win][0] = lum * win_count * 3 / 4;
            s->stat_ae_sum[blk][win][1] = lum * win_count;
            s->stat_ae_sum[blk][win][2] = lum * win_count * 5 / 8;
        }
        for (unsigned i = 0; i < ISP_3A_AE_MEANS; i++) {
            unsigned luma = CLAMP((int)mean_y + ((int)(i % 15) - 7) * 2,
                                  0, 0xff);
            s->stat_ae_mean[blk][i] = isp_3a_pack_mean(luma, mean_u, mean_v);
        }
        for (unsigned i = 0; i < ISP_3A_HIST_BINS; i++) {
            s->stat_hist[blk][i] = hist[i] * (blk + 1);
        }
    }

    for (unsigned i = 0; i < ISP_3A_AWB_HIST_BINS; i++) {
        unsigned lo = i * ISP_3A_HIST_BINS / ISP_3A_AWB_HIST_BINS;
        unsigned hi = (i + 1) * ISP_3A_HIST_BINS / ISP_3A_AWB_HIST_BINS;
        unsigned total = 0;

        for (unsigned b = lo; b < hi; b++) {
            total += hist[b];
        }
        s->stat_awb_yhist[i] = total;
    }
    for (unsigned i = 0; i < ISP_3A_AWB_SUMS; i++) {
        unsigned wcount = MAX(samples / ISP_3A_AWB_SUMS, 1);
        unsigned r = CLAMP((int)mean_y + mean_v / 2, 1, 0x3ffff);
        unsigned b = CLAMP((int)mean_y - mean_u / 2, 1, 0x3ffff);

        s->stat_awb_sum_r_nor[i] = r * wcount;
        s->stat_awb_sum_b_nor[i] = b * wcount;
        s->stat_awb_wp_nor[i] = wcount;
        s->stat_awb_sum_r_big[i] = r * wcount * 2;
        s->stat_awb_sum_b_big[i] = b * wcount * 2;
        s->stat_awb_wp_big[i] = wcount * 2;
    }
    for (unsigned i = 0; i < ISP_3A_AWB_RAM; i++) {
        unsigned b = CLAMP((int)mean_y - mean_u, 0, 0x3ffff);
        unsigned g = CLAMP((int)mean_y, 0, 0x3ffff);
        unsigned r = CLAMP((int)mean_y + mean_v, 0, 0x3ffff);

        s->stat_awb_ram[2 * i] = (b & 0x3ffff) | ((g & 0x3fff) << 18);
        s->stat_awb_ram[2 * i + 1] = ((g >> 14) & 0xf) |
            ((r & 0x3ffff) << 4) | ((i & 0x3ff) << 22);
    }

    s->stat_af_sum[0] = sum_y / (samples ?: 1);
    s->stat_af_sum[1] = s->stat_af_sum[0] * 3 / 4;
    s->stat_af_lum[0] = mean_y * samples;
    s->stat_af_lum[1] = s->stat_af_lum[0] * 3 / 4;
    for (unsigned i = 0; i < ARRAY_SIZE(s->stat_af_ram); i++) {
        unsigned line = i / ISP_3A_AF_COLUMNS;
        unsigned col = i % ISP_3A_AF_COLUMNS;

        s->stat_af_ram[i] = (mean_y + line + col) * (samples / 256 + 1);
    }

    memset(s->ae_mean_cursor, 0, sizeof(s->ae_mean_cursor));
    memset(s->hist_cursor, 0, sizeof(s->hist_cursor));
    s->awb_ram_cursor = 0;
    s->af_ram_cursor = 0;
    REG(s, ISP_ISP3A_RIS) |= ISP_3A_RIS_ALL;
    s->stats_ready = true;
}

static void isp_3a_store_words(uint8_t *buf, hwaddr offset,
                                const uint32_t *values, unsigned count)
{
    memcpy(buf + offset, values, count * sizeof(uint32_t));
}

static void isp_3a_write_ddr(RK3566ISPState *s)
{
    uint8_t *buf = s->stats_ddr;
    hwaddr base = REG(s, ISP_MI_SWS_3A_WR_BASE);

    if (!buf || !base || !(REG(s, ISP_CTRL_SWS_CFG) & ISP_SW_3A_DDR_WRITE_EN)) {
        return;
    }
    memset(buf, 0, ISP_STATS_DDR_SIZE);
    for (unsigned i = 0; i < ISP_3A_AE_BLOCKS; i++) {
        isp_3a_store_words(buf, 0x0000 + i * 0x390, s->stat_ae_mean[i],
                           ISP_3A_AE_MEANS);
        isp_3a_store_words(buf, 0x0c00 + i * 0x800, s->stat_hist[i],
                           ISP_3A_HIST_BINS);
    }
    isp_3a_store_words(buf, 0x0ab0, s->stat_ae_mean[0], 25);
    isp_3a_store_words(buf, 0x1000, s->stat_hist[0], ISP_3A_HIST_BINS);
    isp_3a_store_words(buf, 0x1c00, s->stat_af_ram, ARRAY_SIZE(s->stat_af_ram));
    isp_3a_store_words(buf, 0x2000, s->stat_awb_ram,
                       ARRAY_SIZE(s->stat_awb_ram));
    for (unsigned i = 0; i < ISP_3A_AWB_SUMS; i++) {
        hwaddr offset = 0x2710 + i * 0x20;

        isp_3a_store_words(buf, offset + 0x00, &s->stat_awb_sum_r_nor[i], 1);
        isp_3a_store_words(buf, offset + 0x04, &s->stat_awb_sum_b_nor[i], 1);
        isp_3a_store_words(buf, offset + 0x08, &s->stat_awb_wp_nor[i], 1);
        isp_3a_store_words(buf, offset + 0x10, &s->stat_awb_sum_r_big[i], 1);
        isp_3a_store_words(buf, offset + 0x14, &s->stat_awb_sum_b_big[i], 1);
        isp_3a_store_words(buf, offset + 0x18, &s->stat_awb_wp_big[i], 1);
    }
    isp_3a_store_words(buf, 0x2710 + 0xe0, s->stat_awb_yhist,
                       ISP_3A_AWB_HIST_BINS);
    address_space_write(&s->dma_as, base, MEMTXATTRS_UNSPECIFIED, buf,
                        ISP_STATS_DDR_SIZE);
}

static bool isp_3a_ae_block(hwaddr address, unsigned *blk, hwaddr *base)
{
    static const hwaddr bases[ISP_3A_AE_BLOCKS] = {
        ISP_RAWAE_BIG1_BASE, ISP_RAWAE_BIG2_BASE, ISP_RAWAE_BIG3_BASE,
    };

    for (unsigned i = 0; i < ISP_3A_AE_BLOCKS; i++) {
        if (address >= bases[i] && address < bases[i] + 0x100) {
            *blk = i;
            *base = bases[i];
            return true;
        }
    }
    return false;
}

static bool isp_3a_hist_block(hwaddr address, unsigned *blk, hwaddr *base)
{
    static const hwaddr bases[ISP_3A_AE_BLOCKS] = {
        ISP_RAWHIST_BIG1_BASE, ISP_RAWHIST_BIG2_BASE, ISP_RAWHIST_BIG3_BASE,
    };

    for (unsigned i = 0; i < ISP_3A_AE_BLOCKS; i++) {
        if (address >= bases[i] && address < bases[i] + 0x100) {
            *blk = i;
            *base = bases[i];
            return true;
        }
    }
    return false;
}

static uint64_t isp_3a_read(RK3566ISPState *s, hwaddr address)
{
    unsigned blk;
    hwaddr base;
    hwaddr off;

    if (address >= ISP_RAWAWB_BASE && address < ISP_RAWAWB_BASE + 0x800) {
        off = address - ISP_RAWAWB_BASE;
        if (off == ISP_RAWAWB_CTRL) {
            return (s->regs[address / 4] & ~ISP_3A_MEAS_DONE) |
                   (s->stats_ready ? ISP_3A_MEAS_DONE : 0);
        }
        if (off == ISP_RAWAWB_Y_HIST01 + 0 || off == ISP_RAWAWB_Y_HIST01 + 4 ||
            off == ISP_RAWAWB_Y_HIST01 + 8 || off == ISP_RAWAWB_Y_HIST01 + 12) {
            unsigned idx = (off - ISP_RAWAWB_Y_HIST01) / 4;

            return s->stat_awb_yhist[2 * idx] |
                   (s->stat_awb_yhist[2 * idx + 1] << 16);
        }
        if (off >= ISP_RAWAWB_SUM_RGAIN_NOR_0 &&
            off < ISP_RAWAWB_SUM_RGAIN_NOR_0 + ISP_3A_AWB_SUMS * 0x30) {
            unsigned idx = (off - ISP_RAWAWB_SUM_RGAIN_NOR_0) / 0x30;
            unsigned delta = (off - ISP_RAWAWB_SUM_RGAIN_NOR_0) % 0x30;

            switch (delta) {
            case 0x00:
                return s->stat_awb_sum_r_nor[idx];
            case 0x08:
                return s->stat_awb_sum_b_nor[idx];
            case 0x0c:
                return s->stat_awb_wp_nor[idx];
            case 0x10:
                return s->stat_awb_sum_r_big[idx];
            case 0x18:
                return s->stat_awb_sum_b_big[idx];
            case 0x1c:
                return s->stat_awb_wp_big[idx];
            }
        }
        if (off == ISP_RAWAWB_RAM_DATA_BASE) {
            uint32_t value = s->stat_awb_ram[s->awb_ram_cursor];

            s->awb_ram_cursor = (s->awb_ram_cursor + 1) %
                                ARRAY_SIZE(s->stat_awb_ram);
            return value;
        }
        return s->regs[address / 4];
    }

    if (address >= ISP_RAWAF_BASE && address < ISP_RAWAF_BASE + 0x100) {
        off = address - ISP_RAWAF_BASE;
        switch (off) {
        case ISP_RAWAF_CTRL:
            return (s->regs[address / 4] & ~ISP_3A_MEAS_DONE) |
                   (s->stats_ready ? ISP_3A_MEAS_DONE : 0);
        case ISP_RAWAF_SUM_A:
            return s->stat_af_sum[0];
        case ISP_RAWAF_SUM_B:
            return s->stat_af_sum[1];
        case ISP_RAWAF_LUM_A:
            return s->stat_af_lum[0];
        case ISP_RAWAF_LUM_B:
            return s->stat_af_lum[1];
        case ISP_RAWAF_INT_LINE:
        case ISP_RAWAF_INT_STATE:
            return 0;
        case ISP_RAWAF_RAM_DATA: {
            unsigned idx = s->af_ram_cursor++ % ARRAY_SIZE(s->stat_af_ram);

            return s->stat_af_ram[idx];
        }
        default:
            return s->regs[address / 4];
        }
    }

    if (isp_3a_ae_block(address, &blk, &base)) {
        off = address - base;
        if (off == ISP_RAWAE_BIG_CTRL) {
            return (s->regs[address / 4] & ~ISP_3A_MEAS_DONE) |
                   (s->stats_ready ? ISP_3A_MEAS_DONE : 0);
        }
        if (off >= ISP_RAWAE_BIG_WND1_SUMR &&
            off < ISP_RAWAE_BIG_WND1_SUMR + 3 * ISP_3A_AE_WINDOWS * 4) {
            unsigned idx = (off - ISP_RAWAE_BIG_WND1_SUMR) / 4;

            return s->stat_ae_sum[blk][idx % ISP_3A_AE_WINDOWS][idx /
                ISP_3A_AE_WINDOWS];
        }
        if (off == ISP_RAWAE_BIG_RO_MEAN) {
            unsigned idx = s->ae_mean_cursor[blk]++ % ISP_3A_AE_MEANS;

            return s->stat_ae_mean[blk][idx];
        }
        return s->regs[address / 4];
    }

    if (isp_3a_hist_block(address, &blk, &base)) {
        off = address - base;
        if (off == ISP_RAWHIST_BIG_CTRL) {
            return (s->regs[address / 4] & ~ISP_3A_MEAS_DONE) |
                   (s->stats_ready ? ISP_3A_MEAS_DONE : 0);
        }
        if (off == ISP_RAWHIST_BIG_RO_BIN) {
            unsigned idx = s->hist_cursor[blk]++ % ISP_3A_HIST_BINS;

            return s->stat_hist[blk][idx];
        }
        return s->regs[address / 4];
    }

    if (address >= ISP_ONE_3A_BLS_BASE &&
        address < ISP_ONE_3A_BLS_BASE + 0x100) {
        return 0;
    }
    return s->regs[address / 4];
}

static void isp_3a_write(RK3566ISPState *s, hwaddr address, uint64_t value)
{
    unsigned blk;
    hwaddr base;
    hwaddr off;

    if (address == ISP_ISP3A_ICR) {
        REG(s, ISP_ISP3A_RIS) &= ~(uint32_t)value;
        return;
    }
    s->regs[address / 4] = value & ~ISP_3A_MEAS_DONE;
    if (address >= ISP_RAWAWB_BASE && address < ISP_RAWAWB_BASE + 0x800) {
        off = address - ISP_RAWAWB_BASE;
        if ((off == ISP_RAWAWB_CTRL) && (value & ISP_3A_MEAS_DONE)) {
            s->stats_ready = false;
            REG(s, ISP_ISP3A_RIS) &= ~ISP_3A_RIS_RAWAWB;
        }
        return;
    }
    if (address >= ISP_RAWAF_BASE && address < ISP_RAWAF_BASE + 0x100) {
        off = address - ISP_RAWAF_BASE;
        if ((off == ISP_RAWAF_CTRL) && (value & ISP_3A_MEAS_DONE)) {
            REG(s, ISP_ISP3A_RIS) &= ~ISP_3A_RIS_RAWAF;
        }
        return;
    }
    if (isp_3a_ae_block(address, &blk, &base)) {
        off = address - base;
        if ((off == ISP_RAWAE_BIG_CTRL) && (value & ISP_3A_MEAS_DONE)) {
            REG(s, ISP_ISP3A_RIS) &= ~ISP_3A_RIS_RAWAE_BIG;
        }
        return;
    }
    if (isp_3a_hist_block(address, &blk, &base)) {
        off = address - base;
        if ((off == ISP_RAWHIST_BIG_CTRL) && (value & ISP_3A_MEAS_DONE)) {
            REG(s, ISP_ISP3A_RIS) &= ~ISP_3A_RIS_RAWHIST_BIG;
        }
        return;
    }
}

static void isp_update_irq(RK3566ISPState *s)
{
    REG(s, ISP_MIS) = REG(s, ISP_RIS) & REG(s, ISP_IMSC);
    REG(s, MI_MIS) = REG(s, MI_RIS) & REG(s, MI_IMSC);
    REG(s, ISP_ISP3A_MIS) = REG(s, ISP_ISP3A_RIS) & REG(s, ISP_ISP3A_IMSC);
    qemu_set_irq(s->irq[0], 0);
    qemu_set_irq(s->irq[1], REG(s, MI_MIS) != 0);
    qemu_set_irq(s->irq[2], REG(s, ISP_MIS) != 0 ||
                                REG(s, ISP_ISP3A_MIS) != 0);
}

static void isp_latch(RK3566ISPState *s)
{
    static const uint16_t pairs[][2] = {
        {0x1408, 0x1478}, {0x140c, 0x147c}, {0x1410, 0x1480},
        {0x1418, 0x1484}, {0x141c, 0x1488}, {0x1420, 0x148c},
        {0x1424, 0x1490}, {0x142c, 0x1494}, {0x1430, 0x1498},
        {0x1434, 0x149c}, {0x143c, 0x14a0}, {0x1440, 0x14a4},
        {0x1444, 0x14a8}, {0x1450, 0x14b0}, {0x1454, 0x14b4},
        {0x1458, 0x14b8}, {0x1460, 0x14bc}, {0x1464, 0x14c0},
        {0x1468, 0x14c4}, {0x0594, 0x05ac}, {0x0598, 0x05b0},
        {0x059c, 0x05b4}, {0x05a0, 0x05b8},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(pairs); i++) {
        REG(s, pairs[i][1]) = REG(s, pairs[i][0]);
    }
    REG(s, MI_CTRL_SHD) = (REG(s, MI_CTRL) & 0xffff) |
                          ((REG(s, MI_CTRL) & 15) << 16);
    for (unsigned base = 0xc00; base <= 0x1000; base += 0x400) {
        for (unsigned offset = 0; offset <= 0x24; offset += 4) {
            REG(s, base + offset + 0x30) = REG(s, base + offset);
        }
    }
    memcpy(s->active, s->regs, sizeof(s->active));
}

static unsigned isp_scaled_size(unsigned input, unsigned factor,
                                bool enabled, bool enlarge)
{
    if (!enabled || input < 2 || !factor) {
        return input;
    }
    if (enlarge) {
        return ((uint64_t)(input - 1) * 65536 + factor / 2) / factor + 1;
    }
    return ((uint64_t)(input - 1) * (factor - 1) + 65535) / 65536 + 1;
}

static enum AVPixelFormat isp_output_format(RK3566ISPState *s, bool selfpath)
{
    uint32_t control = s->active[MI_CTRL / 4];
    unsigned packing = extract32(control, selfpath ? 24 : 22, 2);
    bool swap = s->active[0x1548 / 4] & BIT(selfpath ? 1 : 0);
    unsigned chroma;
    if (selfpath) {
        chroma = extract32(control, 28, 3);
        if (chroma == 0) {
            return AV_PIX_FMT_GRAY8;
        }
        if (chroma == 4) {
            return AV_PIX_FMT_RGB565LE;
        }
        if (chroma == 7) {
            return AV_PIX_FMT_BGR0;
        }
    } else {
        unsigned y_size = s->active[0x140c / 4];
        unsigned cb_size = s->active[0x1420 / 4];
        chroma = cb_size && cb_size * (packing == 1 ? 2 : 4) == y_size ? 1 : 2;
    }
    if (packing == 2 && chroma == 2 && !swap) {
        return AV_PIX_FMT_UYVY422;
    }
    if (packing == 1) {
        if (chroma == 1) {
            return swap ? AV_PIX_FMT_NV21 : AV_PIX_FMT_NV12;
        }
        if (chroma == 2 && !swap) {
            return AV_PIX_FMT_NV16;
        }
    }
    if (packing == 0) {
        return chroma == 1 ? AV_PIX_FMT_YUV420P :
               chroma == 2 ? AV_PIX_FMT_YUV422P :
               chroma == 3 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_NONE;
    }
    return AV_PIX_FMT_NONE;
}

static bool isp_capture_path(RK3566ISPState *s, bool selfpath)
{
    enum AVPixelFormat format = isp_output_format(s, selfpath);
    const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(format);
    uint8_t *output[4] = {};
    int output_stride[4] = {};
    struct SwsContext *scaler = NULL;
    unsigned width, height;
    unsigned source_width = s->width, source_height = s->height;
    unsigned source_x = 0, source_y = 0;
    unsigned base = selfpath ? 0x143c : 0x1408;
    unsigned sizes[3] = {}, addresses[3];
    unsigned offset[] = {0, 0x14, 0x24};
    unsigned guest_stride;
    bool result = false;
    const char *failure = "image allocation or luma stride";

    if (format == AV_PIX_FMT_NONE || !description) {
        return false;
    }
    if (selfpath) {
        width = s->active[0x1514 / 4];
        height = s->active[0x1518 / 4];
    } else {
        unsigned crop = s->active[0x880 / 4] & 3;
        unsigned input_width = s->active[(crop ? 0x88c : ISP_H_SIZE) / 4];
        unsigned input_height = s->active[(crop ? 0x890 : ISP_V_SIZE) / 4];
        uint32_t resize = s->active[0xc00 / 4];
        if (crop) {
            source_x = s->active[0x884 / 4];
            source_y = s->active[0x888 / 4];
            source_width = input_width;
            source_height = input_height;
        }
        width = isp_scaled_size(input_width, s->active[0xc04 / 4],
                                 resize & BIT(0), resize & BIT(4));
        height = isp_scaled_size(input_height, s->active[0xc10 / 4],
                                  resize & BIT(2), resize & BIT(6));
        if (crop && ((source_x & 1) || !input_width || !input_height ||
                     (uint64_t)source_x + input_width > s->width ||
                     (uint64_t)source_y + input_height > s->height)) {
            failure = "crop window";
            goto out;
        }
    }
    if (!width || !height || width > 8192 || height > 8192) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        addresses[i] = s->active[(base + offset[i]) / 4];
        sizes[i] = s->active[(base + offset[i] + 4) / 4];
    }
    guest_stride = sizes[0] / height;
    if (guest_stride < av_image_get_linesize(format, width, 0) ||
        av_image_alloc(output, output_stride, width, height, format, 16) < 0) {
        goto out;
    }
    scaler = sws_getContext(source_width, source_height, AV_PIX_FMT_UYVY422,
                            width, height, format, SWS_BILINEAR,
                            NULL, NULL, NULL);
    if (!scaler) {
        failure = "scaler configuration";
        goto out;
    }
    size_t input_offset = ((size_t)source_y * s->width + source_x) * 2;
    const uint8_t *input[] = { s->frame + input_offset, NULL, NULL, NULL };
    int input_stride[] = { s->width * 2, 0, 0, 0 };
    if (sws_scale(scaler, input, input_stride, 0, source_height,
                   output, output_stride) != height) {
        failure = "image conversion";
        goto out;
    }
    unsigned planes = av_pix_fmt_count_planes(format);
    for (unsigned plane = 0; plane < planes; plane++) {
        unsigned rows = height;
        unsigned row_bytes = av_image_get_linesize(format, width, plane);
        unsigned stride = guest_stride;
        if (plane) {
            rows = AV_CEIL_RSHIFT((int)height, description->log2_chroma_h);
            stride = sizes[plane] / rows;
        }
        if (stride < row_bytes || (uint64_t)stride * rows > sizes[plane]) {
            failure = "plane size or stride";
            goto out;
        }
        for (unsigned row = 0; row < rows; row++) {
            if (dma_memory_write(&s->dma_as,
                                  (dma_addr_t)addresses[plane] +
                                  (uint64_t)row * stride,
                                  output[plane] + row * output_stride[plane],
                                  row_bytes,
                                  MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                failure = "DMA write";
                goto out;
            }
        }
    }
    REG(s, MI_RIS) |= BIT(selfpath ? 1 : 0);
    result = true;
out:
    if (!result) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3566-isp: %s path=%u format=%s %ux%u "
                      "sizes=%u,%u,%u\n", failure, selfpath,
                      av_get_pix_fmt_name(format), width, height,
                      sizes[0], sizes[1], sizes[2]);
    }
    sws_freeContext(scaler);
    av_freep(&output[0]);
    return result;
}

static void isp_frame_end(void *opaque)
{
    RK3566ISPState *s = opaque;
    bool good = true;
    uint32_t control = s->active[MI_CTRL / 4];
    for (unsigned path = 0; path < 2; path++) {
        if (control & BIT(path)) {
            good &= isp_capture_path(s, path != 0);
        }
    }
    s->busy = false;
    if (good) {
        REG(s, ISP_RIS) |= ISP2X_FRAME_BIT;
        REG(s, ISP_FRAME_COUNT)++;
        isp_3a_measure(s);
        isp_3a_write_ddr(s);
    } else {
        REG(s, ISP_RIS) |= BIT(3);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3566-isp: invalid capture format, dimensions or DMA buffer\n");
    }
    isp_latch(s);
    isp_update_irq(s);
}

static int isp_can_receive(void *opaque)
{
    RK3566ISPState *s = opaque;
    return MIN(65536, s->input_size - s->input_count);
}

static void isp_submit_frame(RK3566ISPState *s)
{
    if (!s->streaming || !s->link_enabled ||
        !(REG(s, ISP_CTRL) & 1) || s->busy) {
        return;
    }
    uint8_t *swap = s->frame;
    s->frame = s->input;
    s->input = swap;
    s->busy = true;
    REG(s, ISP_RIS) |= BIT(6);
    isp_update_irq(s);
    unsigned raw_width = REG(s, 0x410) ?: s->width;
    unsigned raw_height = REG(s, 0x414) ?: s->height;
    if (raw_width > 8192 || raw_height > 8192) {
        s->busy = false;
        REG(s, ISP_RIS) |= BIT(3);
        isp_update_irq(s);
        return;
    }
    uint64_t frame_ns = muldiv64((uint64_t)raw_width * raw_height,
                                 NANOSECONDS_PER_SECOND, s->pixel_rate_hz);
    timer_mod_ns(s->frame_end, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 MAX(frame_ns, 1));
}

static void isp_synth_generate(RK3566ISPState *s)
{
    uint8_t *buf = s->input;
    uint64_t tick = s->input_frames;
    unsigned w = s->width;
    unsigned h = s->height;

    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x + 1 < w; x += 2) {
            unsigned bar = (x * 8) / w;
            int luma = 40 + bar * 24;
            int u = 96 + (int)(bar % 4) * 6;
            int v = 160 - (int)(bar % 4) * 6;
            unsigned offset = (y * w + x) * 2;

            if ((y + tick * 4) % 72 < 10) {
                luma = 220;
            }
            buf[offset] = CLAMP(u, 0, 255);
            buf[offset + 1] = CLAMP(luma + (x & 3), 0, 255);
            buf[offset + 2] = CLAMP(v, 0, 255);
            buf[offset + 3] = CLAMP(luma + ((x + 1) & 3), 0, 255);
        }
    }
}

static void isp_synth_tick(void *opaque)
{
    RK3566ISPState *s = opaque;
    bool enabled = s->synth && s->streaming && s->link_enabled &&
                   (REG(s, ISP_CTRL) & 1);

    if (!enabled) {
        return;
    }
    if (!s->busy) {
        isp_synth_generate(s);
        isp_submit_frame(s);
    }
    timer_mod_ns(s->synth_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 NANOSECONDS_PER_SECOND / 30);
}

static void isp_synth_update(RK3566ISPState *s)
{
    bool enabled = s->synth && s->streaming && s->link_enabled &&
                   (REG(s, ISP_CTRL) & 1);

    if (enabled && !timer_pending(s->synth_timer)) {
        timer_mod_ns(s->synth_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    } else if (!enabled && timer_pending(s->synth_timer)) {
        timer_del(s->synth_timer);
    }
}

static bool isp_3a_addr(hwaddr address)
{
    return (address >= ISP_ONE_3A_BLS_BASE &&
            address < ISP_ONE_3A_BLS_BASE + 0x100) ||
           (address >= ISP_RAWAE_BIG1_BASE &&
            address < ISP_RAWAF_BASE + 0x100) ||
           (address >= ISP_RAWAWB_BASE &&
            address < ISP_RAWAWB_BASE + 0x800) ||
           (address >= ISP_ISP3A_IMSC && address <= ISP_ISP3A_ICR);
}

static void isp_receive(void *opaque, const uint8_t *data, int size)
{
    RK3566ISPState *s = opaque;
    memcpy(s->input + s->input_count, data, size);
    s->input_count += size;
    if (s->input_count != s->input_size) {
        return;
    }
    s->input_count = 0;
    s->input_frames++;
    isp_submit_frame(s);
}

static void isp_sensor_stream(void *opaque, int line, int level)
{
    RK3566ISPState *s = RK3566_ISP(opaque);

    s->streaming = level;
    isp_synth_update(s);
}

static void isp_link_enable(void *opaque, int line, int level)
{
    RK3566ISPState *s = RK3566_ISP(opaque);

    s->link_enabled = level;
    isp_synth_update(s);
}

static void isp_camera_event(void *opaque, QEMUChrEvent event)
{
    if (event == CHR_EVENT_CLOSED || event == CHR_EVENT_OPENED) {
        RK3566_ISP(opaque)->input_count = 0;
    }
}

static void isp_sys_reset_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    if (!val) {
        return;
    }
    timer_del(s->frame_end);
    s->busy = false;
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->active, 0, sizeof(s->active));
}

static uint64_t isp_ctrl_pre_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);
    uint32_t old = *(uint32_t *)reg->data;

    if (!(val & 1) && (old & 1)) {
        REG(s, ISP_RIS) |= BIT(0);
        timer_del(s->frame_end);
        s->busy = false;
    }
    return val;
}

static void isp_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    *(uint32_t *)reg->data = val & ~(BIT(9) | BIT(10));
    if (val & (BIT(9) | BIT(10))) {
        isp_latch(s);
    }
    isp_synth_update(s);
}

static void isp_ris_clear_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);
    hwaddr status = reg->access->addr == ISP_ICR ? ISP_RIS : MI_RIS;

    REG(s, status) &= ~(uint32_t)val;
}

static void isp_ris_set_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);
    hwaddr status = reg->access->addr == ISP_ISR ? ISP_RIS : MI_RIS;

    REG(s, status) |= (uint32_t)val;
}

static uint64_t isp_write_only_post_read(RegisterInfo *reg, uint64_t val)
{
    return 0;
}

static void isp_mi_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    if (!s->busy) {
        isp_latch(s);
    }
}

static void isp_mi_init_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566ISPState *s = RK3566_ISP(reg->opaque);

    if (val & BIT(4)) {
        isp_latch(s);
    }
}

#define ISP_REG(off) { .name = "ISP_REG_" #off, .addr = (off) }

static const RegisterAccessInfo isp_regs_info[] = {
    { .name = "SYS_RESET", .addr = 0x014,
      .post_write = isp_sys_reset_post_write },
    { .name = "ISP_CTRL", .addr = ISP_CTRL,
      .pre_write = isp_ctrl_pre_write,
      .post_write = isp_ctrl_post_write },
    { .name = "ISP_RIS", .addr = ISP_RIS, .ro = UINT32_MAX },
    { .name = "ISP_MIS", .addr = ISP_MIS, .ro = UINT32_MAX },
    { .name = "ISP_ICR", .addr = ISP_ICR,
      .post_write = isp_ris_clear_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "ISP_ISR", .addr = ISP_ISR,
      .post_write = isp_ris_set_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "ISP_FRAME_COUNT", .addr = ISP_FRAME_COUNT, .ro = UINT32_MAX },
    { .name = "MI_CTRL", .addr = MI_CTRL,
      .post_write = isp_mi_ctrl_post_write },
    { .name = "MI_INIT", .addr = MI_INIT,
      .post_write = isp_mi_init_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "MI_RIS", .addr = MI_RIS, .ro = UINT32_MAX },
    { .name = "MI_MIS", .addr = MI_MIS, .ro = UINT32_MAX },
    { .name = "MI_ICR", .addr = MI_ICR,
      .post_write = isp_ris_clear_post_write,
      .post_read = isp_write_only_post_read },
    { .name = "MI_ISR", .addr = MI_ISR,
      .post_write = isp_ris_set_post_write,
      .post_read = isp_write_only_post_read },
    /* Capture configuration, shadow pairs and shadow write-back ranges. */
    ISP_REG(0x0410), ISP_REG(0x0414), ISP_REG(0x0594), ISP_REG(0x0598),
    ISP_REG(0x059c), ISP_REG(0x05a0), ISP_REG(0x05ac), ISP_REG(0x05b0),
    ISP_REG(0x05b4), ISP_REG(0x05b8), ISP_REG(0x05bc),
    ISP_REG(0x0880), ISP_REG(0x0884), ISP_REG(0x0888), ISP_REG(0x088c),
    ISP_REG(0x0890),
    ISP_REG(0x0c00), ISP_REG(0x0c04), ISP_REG(0x0c08), ISP_REG(0x0c0c),
    ISP_REG(0x0c10), ISP_REG(0x0c14), ISP_REG(0x0c18), ISP_REG(0x0c1c),
    ISP_REG(0x0c20), ISP_REG(0x0c24), ISP_REG(0x0c30), ISP_REG(0x0c34),
    ISP_REG(0x0c38), ISP_REG(0x0c3c), ISP_REG(0x0c40), ISP_REG(0x0c44),
    ISP_REG(0x0c48), ISP_REG(0x0c4c), ISP_REG(0x0c50), ISP_REG(0x0c54),
    ISP_REG(0x1000), ISP_REG(0x1004), ISP_REG(0x1008), ISP_REG(0x100c),
    ISP_REG(0x1010), ISP_REG(0x1014), ISP_REG(0x1018), ISP_REG(0x101c),
    ISP_REG(0x1020), ISP_REG(0x1024), ISP_REG(0x1030), ISP_REG(0x1034),
    ISP_REG(0x1038), ISP_REG(0x103c), ISP_REG(0x1040), ISP_REG(0x1044),
    ISP_REG(0x1048), ISP_REG(0x104c), ISP_REG(0x1050), ISP_REG(0x1054),
    ISP_REG(0x1408), ISP_REG(0x140c), ISP_REG(0x1410), ISP_REG(0x1418),
    ISP_REG(0x141c), ISP_REG(0x1420), ISP_REG(0x1424), ISP_REG(0x142c),
    ISP_REG(0x1430), ISP_REG(0x1434), ISP_REG(0x143c), ISP_REG(0x1440),
    ISP_REG(0x1444), ISP_REG(0x1450), ISP_REG(0x1454), ISP_REG(0x1458),
    ISP_REG(0x1460), ISP_REG(0x1464), ISP_REG(0x1468),
    ISP_REG(0x1474), ISP_REG(0x1478), ISP_REG(0x147c), ISP_REG(0x1480),
    ISP_REG(0x1484), ISP_REG(0x1488), ISP_REG(0x148c), ISP_REG(0x1490),
    ISP_REG(0x1494), ISP_REG(0x1498), ISP_REG(0x149c), ISP_REG(0x14a0),
    ISP_REG(0x14a4), ISP_REG(0x14a8), ISP_REG(0x14b0), ISP_REG(0x14b4),
    ISP_REG(0x14b8), ISP_REG(0x14bc), ISP_REG(0x14c0), ISP_REG(0x14c4),
    ISP_REG(0x14f8), ISP_REG(0x1514), ISP_REG(0x1518), ISP_REG(0x1548),
};

#undef ISP_REG

static RegisterInfo *isp_find_register(RegisterInfoArray *reg_array,
                                       hwaddr offset)
{
    for (unsigned i = 0; i < reg_array->num_elements; i++) {
        RegisterInfo *reg = reg_array->r[i];

        if (reg->access->addr == offset) {
            return reg;
        }
    }
    return NULL;
}

static uint64_t isp_read(void *opaque, hwaddr address, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566ISPState *s = RK3566_ISP(register_array_get_owner(reg_array));

    if (isp_3a_addr(address)) {
        return isp_3a_read(s, address);
    }
    if (isp_find_register(reg_array, address)) {
        return register_read_memory(reg_array, address, size);
    }
    return s->regs[address / 4];
}

static void isp_write(void *opaque, hwaddr address, uint64_t value,
                      unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566ISPState *s = RK3566_ISP(register_array_get_owner(reg_array));

    if (isp_3a_addr(address)) {
        isp_3a_write(s, address, value);
        isp_update_irq(s);
        return;
    }
    if (isp_find_register(reg_array, address)) {
        register_write_memory(reg_array, address, value, size);
    } else {
        s->regs[address / 4] = value;
    }
    switch (address) {
    case ISP_RIS:
    case ISP_MIS:
    case MI_RIS:
    case MI_MIS:
    case ISP_FRAME_COUNT:
        return;
    default:
        isp_update_irq(s);
    }
}

static const MemoryRegionOps isp_ops = {
    .read = isp_read,
    .write = isp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void isp_reset(DeviceState *device)
{
    RK3566ISPState *s = RK3566_ISP(device);

    timer_del(s->frame_end);
    timer_del(s->synth_timer);
    isp_3a_reset(s);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->active, 0, sizeof(s->active));
    for (unsigned i = 0; i < ARRAY_SIZE(s->regs_info); i++) {
        register_reset(&s->regs_info[i]);
    }
    s->input_count = 0;
    s->input_frames = 0;
    s->busy = false;
    isp_update_irq(s);
}

static void isp_realize(DeviceState *device, Error **errp)
{
    RK3566ISPState *s = RK3566_ISP(device);
    if (!s->dma || !s->pixel_rate_hz || !s->width ||
        !s->height || (s->width & 1) ||
        s->width > 4096 || s->height > 4096) {
        error_setg(errp, "rk3566-isp requires dma memory "
                   "and even input width <=4096");
        return;
    }
    address_space_init(&s->dma_as, s->dma, "rk3566-isp-dma");
    s->input_size = s->width * s->height * 2;
    s->input = g_malloc(s->input_size);
    s->frame = g_malloc(s->input_size);
    s->stats_ddr = g_malloc0(ISP_STATS_DDR_SIZE);
    qemu_chr_fe_set_handlers(&s->camera, isp_can_receive, isp_receive,
                             isp_camera_event, NULL, s, NULL, true);
    s->synth = qemu_chr_fe_get_driver(&s->camera) == NULL;
}

static void isp_init(Object *object)
{
    RK3566ISPState *s = RK3566_ISP(object);

    s->frame_end = timer_new_ns(QEMU_CLOCK_VIRTUAL, isp_frame_end, s);
    s->synth_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, isp_synth_tick, s);
    s->reg_array = register_init_block32(DEVICE(object), isp_regs_info,
                                         ARRAY_SIZE(isp_regs_info),
                                         s->regs_info, s->regs,
                                         &isp_ops, false, ISP_REG_WINDOW);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->reg_array->mem);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(s), isp_sensor_stream, "sensor-stream", 1);
    qdev_init_gpio_in_named(DEVICE(s), isp_link_enable, "link-enabled", 1);
    object_property_add_uint64_ptr(object, "input-frames", &s->input_frames,
                                   OBJ_PROP_FLAG_READ);
}

static void isp_finalize(Object *object)
{
    RK3566ISPState *s = RK3566_ISP(object);
    timer_free(s->frame_end);
    timer_free(s->synth_timer);
    if (s->dma_as.root) {
        address_space_destroy(&s->dma_as);
    }
    g_free(s->input);
    g_free(s->frame);
    g_free(s->stats_ddr);
}

static const VMStateDescription isp_vmstate = {
    .name = TYPE_RK3566_ISP, .unmigratable = true,
};

static const Property isp_properties[] = {
    DEFINE_PROP_CHR("chardev", RK3566ISPState, camera),
    DEFINE_PROP_LINK("dma", RK3566ISPState, dma,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT32("input-width", RK3566ISPState, width, 640),
    DEFINE_PROP_UINT32("input-height", RK3566ISPState, height, 360),
    DEFINE_PROP_UINT32("pixel-rate-hz", RK3566ISPState,
                       pixel_rate_hz, 118800000),
};

static void isp_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(class);
    device->realize = isp_realize;
    device->vmsd = &isp_vmstate;
    device_class_set_legacy_reset(device, isp_reset);
    device_class_set_props(device, isp_properties);
}

static const TypeInfo isp_types[] = {
    { .name = TYPE_RK3566_ISP, .parent = TYPE_SYS_BUS_DEVICE,
      .instance_size = sizeof(RK3566ISPState), .instance_init = isp_init,
      .instance_finalize = isp_finalize, .class_init = isp_class_init },
};

DEFINE_TYPES(isp_types)

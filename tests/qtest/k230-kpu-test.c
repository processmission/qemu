/*
 * QTest testcase for K230 KPU/GNNE block
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define K230_FAKE_KPU_OUTPUT_BASE 0x10090000
#define K230_FAKE_KPU_OUTPUT_SIZE 0x00100000
#define K230_KPU_CFG_BASE         0x80400000
#define K230_PLIC_BASE            0xf00000000ULL

#define K230_PLIC_ENABLE_S        0x2080
#define K230_PLIC_CONTEXT_S       0x201000
#define K230_PLIC_THRESHOLD       0x00
#define K230_PLIC_CLAIM           0x04

#define K230_GNNE_COMMAND_START   0x100
#define K230_GNNE_COMMAND_END     0x104
#define K230_GNNE_COMMAND_HI      0x108
#define K230_GNNE_CONTROL         0x128
#define K230_GNNE_CLEAR           K230_GNNE_CONTROL
#define K230_GNNE_STATUS          0x130
#define K230_GNNE_SETUP           0x190

#define K230_GNNE_COMMAND_TEST    0x01000000
#define K230_GNNE_COMMAND_BASE_OFFSET 0x003a6000
#define K230_GNNE_RUNTIME_RDATA_BASE 0x10000020
#define K230_GNNE_RUNTIME_FUNCTION_COMMAND 0x1032b020
#define K230_GNNE_RUNTIME_ARG_TABLE 0x80000000
#define K230_GNNE_RUNTIME_DIRECT_SOURCE \
    (K230_GNNE_RUNTIME_RDATA_BASE + 0x500000)
#define K230_GNNE_RUNTIME_DIRECT_OUTPUT \
    (K230_GNNE_RUNTIME_RDATA_BASE + 0x501000)
#define K230_GNNE_RUNTIME_DDR_BASE 0x3c000000
#define K230_GNNE_RDATA_ALIAS_BASE 0xfc000000
#define K230_GNNE_RDATA_FALLBACK_BASE 0x10000000
#define K230_GNNE_DONE            0x0000000400000004ULL
#define K230_GNNE_START           0x0000000900000009ULL
#define K230_GNNE_DELAY_NS        (100 * 1000)
#define K230_GNNE_IRQ             189
#define K230_KPU_PAGE_SIZE        4096

#define K230_KPU_OUTPUT_END \
    (K230_FAKE_KPU_OUTPUT_BASE + K230_FAKE_KPU_OUTPUT_SIZE)
#define K230_KPU_OUTPUT_TEST0     K230_FAKE_KPU_OUTPUT_BASE
#define K230_KPU_OUTPUT_TEST1     (K230_FAKE_KPU_OUTPUT_BASE + 0x3000)
#define K230_KPU_OUTPUT_LAST \
    (K230_KPU_OUTPUT_END - K230_KPU_PAGE_SIZE)
#define K230_KPU_OUTPUT_UNREFERENCED \
    (K230_FAKE_KPU_OUTPUT_BASE + 0x7000)
#define K230_KPU_OUTSIDE_LOW \
    (K230_FAKE_KPU_OUTPUT_BASE - K230_KPU_PAGE_SIZE)
#define K230_KPU_OUTSIDE_HIGH     K230_KPU_OUTPUT_END
#define K230_GNNE_SYNTH_LANE_WIDTH 24
#define K230_GNNE_SYNTH_GLB_BASE \
    (K230_GNNE_COMMAND_TEST - K230_GNNE_COMMAND_BASE_OFFSET)
#define K230_GNNE_SYNTH_OUTPUT    (K230_GNNE_SYNTH_GLB_BASE + 0x180)
#define K230_GNNE_SYNTH_LOAD_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x240)
#define K230_GNNE_SYNTH_WEIGHT_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x300)
#define K230_GNNE_SYNTH_STORE_SOURCE (K230_GNNE_SYNTH_GLB_BASE + 0x340)
#define K230_GNNE_SYNTH_ROUNDTRIP_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x380)
#define K230_GNNE_SYNTH_MFU_SOURCE (K230_GNNE_SYNTH_GLB_BASE + 0x400)
#define K230_GNNE_SYNTH_MFU_ARG (K230_GNNE_SYNTH_GLB_BASE + 0x420)
#define K230_GNNE_SYNTH_MFU_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x440)
#define K230_GNNE_SYNTH_MFU_ADD_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x460)
#define K230_GNNE_SYNTH_MFU_FP16_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x480)
#define K230_GNNE_SYNTH_MFU_ROUNDTRIP_OUTPUT \
    (K230_GNNE_SYNTH_GLB_BASE + 0x4a0)
#define K230_GNNE_SYNTH_CONV_INPUT (K230_GNNE_SYNTH_GLB_BASE + 0x500)
#define K230_GNNE_SYNTH_CONV_WEIGHT (K230_GNNE_SYNTH_GLB_BASE + 0x520)
#define K230_GNNE_SYNTH_CONV_WEIGHT_ZP (K230_GNNE_SYNTH_GLB_BASE + 0x590)
#define K230_GNNE_SYNTH_CONV_ACT0 (K230_GNNE_SYNTH_GLB_BASE + 0x5a0)
#define K230_GNNE_SYNTH_CONV_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x5c0)
#define K230_GNNE_SYNTH_PDP0_INPUT (K230_GNNE_SYNTH_GLB_BASE + 0x600)
#define K230_GNNE_SYNTH_PDP0_WEIGHT (K230_GNNE_SYNTH_GLB_BASE + 0x620)
#define K230_GNNE_SYNTH_PDP0_WEIGHT_ZP (K230_GNNE_SYNTH_GLB_BASE + 0x680)
#define K230_GNNE_SYNTH_PDP0_ACT0 (K230_GNNE_SYNTH_GLB_BASE + 0x690)
#define K230_GNNE_SYNTH_PDP0_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x6c0)
#define K230_GNNE_SYNTH_PSUM_INPUT (K230_GNNE_SYNTH_GLB_BASE + 0x700)
#define K230_GNNE_SYNTH_PSUM_WEIGHT0 (K230_GNNE_SYNTH_GLB_BASE + 0x720)
#define K230_GNNE_SYNTH_PSUM_WEIGHT1 (K230_GNNE_SYNTH_GLB_BASE + 0x740)
#define K230_GNNE_SYNTH_PSUM_WEIGHT_ZP (K230_GNNE_SYNTH_GLB_BASE + 0x760)
#define K230_GNNE_SYNTH_PSUM_ACT0 (K230_GNNE_SYNTH_GLB_BASE + 0x770)
#define K230_GNNE_SYNTH_PSUM_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x790)
#define K230_GNNE_SYNTH_PDP1_INPUT (K230_GNNE_SYNTH_GLB_BASE + 0x800)
#define K230_GNNE_SYNTH_PDP1_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x840)
#define K230_GNNE_SYNTH_CLASS_INPUT (K230_GNNE_SYNTH_GLB_BASE + 0x900)
#define K230_GNNE_SYNTH_CLASS_WEIGHT0 (K230_GNNE_SYNTH_GLB_BASE + 0x940)
#define K230_GNNE_SYNTH_CLASS_WEIGHT1 (K230_GNNE_SYNTH_GLB_BASE + 0xc40)
#define K230_GNNE_SYNTH_CLASS_WEIGHT_ZP (K230_GNNE_SYNTH_GLB_BASE + 0xf40)
#define K230_GNNE_SYNTH_CLASS_ACT0 (K230_GNNE_SYNTH_GLB_BASE + 0xf80)
#define K230_GNNE_SYNTH_CLASS_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x1180)
#define K230_GNNE_SYNTH_DECONV_INPUT_U8 (K230_GNNE_SYNTH_GLB_BASE + 0x1200)
#define K230_GNNE_SYNTH_DECONV_INPUT_I8 (K230_GNNE_SYNTH_GLB_BASE + 0x1220)
#define K230_GNNE_SYNTH_DECONV_ZERO_INPUT (K230_GNNE_SYNTH_GLB_BASE + 0x1240)
#define K230_GNNE_SYNTH_DECONV_WEIGHT (K230_GNNE_SYNTH_GLB_BASE + 0x1260)
#define K230_GNNE_SYNTH_DECONV_ZERO_WEIGHT (K230_GNNE_SYNTH_GLB_BASE + 0x12a0)
#define K230_GNNE_SYNTH_DECONV_WEIGHT_ZP (K230_GNNE_SYNTH_GLB_BASE + 0x12e0)
#define K230_GNNE_SYNTH_DECONV_ACT0 (K230_GNNE_SYNTH_GLB_BASE + 0x1300)
#define K230_GNNE_SYNTH_DECONV_OUTPUT (K230_GNNE_SYNTH_GLB_BASE + 0x1320)
#define K230_GNNE_SYNTH_DECONV_MULTI_INPUT (K230_GNNE_SYNTH_GLB_BASE + 0x1340)
#define K230_GNNE_SYNTH_DECONV_MULTI_WEIGHT (K230_GNNE_SYNTH_GLB_BASE + 0x1360)
#define K230_GNNE_SYNTH_DECONV_MULTI_WEIGHT_ZP \
    (K230_GNNE_SYNTH_GLB_BASE + 0x13c0)
#define K230_GNNE_SYNTH_DECONV_MULTI_ACT0 (K230_GNNE_SYNTH_GLB_BASE + 0x13e0)
#define K230_GNNE_SYNTH_DECONV_MULTI_OUTPUT \
    (K230_GNNE_SYNTH_GLB_BASE + 0x1400)
#define K230_GNNE_SYNTH_TRANSPOSE_INPUT \
    (K230_GNNE_SYNTH_GLB_BASE + 0x1500)
#define K230_GNNE_SYNTH_TRANSPOSE_OUTPUT \
    (K230_GNNE_SYNTH_GLB_BASE + 0x1600)
#define K230_GNNE_SYNTH_MFU_SEG_ARG \
    (K230_GNNE_SYNTH_GLB_BASE + 0x1700)
#define K230_GNNE_SYNTH_MFU_SEG_OUTPUT \
    (K230_GNNE_SYNTH_GLB_BASE + 0x1780)
#define K230_GNNE_SYNTH_SOURCE    0x02000000

#define GNNE_FIELD(value, shift)   ((uint32_t)(value) << (shift))
#define GNNE_LUI(rd, imm) \
    (0x02 | GNNE_FIELD(rd, 7) | GNNE_FIELD(imm, 12))
#define GNNE_LW(rd, rs, offset) \
    (0x06 | GNNE_FIELD(rd, 7) | GNNE_FIELD(rs, 12) | \
     GNNE_FIELD((offset) & 0xfff, 20))
#define GNNE_ADDI(rd, rs, imm) \
    (0x0e | GNNE_FIELD(rd, 7) | GNNE_FIELD(rs, 12) | \
     GNNE_FIELD((imm) & 0xfff, 20))
#define GNNE_MMU_CONF(rstart, rdepth, id) \
    (0x44 | GNNE_FIELD(rstart, 7) | GNNE_FIELD(rdepth, 12) | \
     GNNE_FIELD(id, 17))
#define GNNE_SS_PACK_SHAPE(rn, rc, rh, rw, rss) \
    (0x40 | GNNE_FIELD(rn, 7) | GNNE_FIELD(rc, 12) | \
     GNNE_FIELD(rh, 17) | GNNE_FIELD(rw, 22) | GNNE_FIELD(rss, 27))
#define GNNE_SS_PACK_STRIDE(rn, rc, rh, rss) \
    (0x42 | GNNE_FIELD(rn, 7) | GNNE_FIELD(rc, 12) | \
     GNNE_FIELD(rh, 17) | GNNE_FIELD(rss, 27))
#define GNNE_L2_LOAD_CONF(rstride_d, rstride_s, l2_dt, ddr_dt) \
    (0x46 | GNNE_FIELD(rstride_d, 7) | GNNE_FIELD(rstride_s, 10) | \
     GNNE_FIELD(l2_dt, 13) | GNNE_FIELD(ddr_dt, 15))
#define GNNE_L2_LOAD_W_CONF(rlen_c, rlen_d, l2_dt, ddr_dt, decomp) \
    (0x48 | GNNE_FIELD(rlen_c, 7) | GNNE_FIELD(rlen_d, 12) | \
     GNNE_FIELD(l2_dt, 17) | GNNE_FIELD(ddr_dt, 19) | \
     GNNE_FIELD(decomp, 22))
#define GNNE_L2_STORE_CONF(rstride_d, rstride_s, l2_dt, ddr_dt) \
    (0x4a | GNNE_FIELD(rstride_d, 7) | GNNE_FIELD(rstride_s, 10) | \
     GNNE_FIELD(l2_dt, 13) | GNNE_FIELD(ddr_dt, 15))
#define GNNE_L2_LOAD(raddr_d, raddr_s, rshape) \
    (0x4c | GNNE_FIELD(raddr_d, 7) | GNNE_FIELD(raddr_s, 12) | \
     GNNE_FIELD(rshape, 17))
#define GNNE_L2_STORE(raddr_d, raddr_s, rshape) \
    (0x4e | GNNE_FIELD(raddr_d, 7) | GNNE_FIELD(raddr_s, 12) | \
     GNNE_FIELD(rshape, 17))
#define GNNE_L2_LOAD_W(raddr_d, raddr_s, rvalid_c_num) \
    (0x57 | GNNE_FIELD(raddr_d, 7) | GNNE_FIELD(raddr_s, 12) | \
     GNNE_FIELD(rvalid_c_num, 17))
#define GNNE_AI2D_COMPUTE() 0x51
#define GNNE_MFU_PDP1_CONF1(stride_w, stride_h, rstride_s, funct2, rstride_d) \
    (0x62 | GNNE_FIELD(0x01, 7) | GNNE_FIELD(stride_w, 12) | \
     GNNE_FIELD(stride_h, 17) | GNNE_FIELD(rstride_s, 22) | \
     GNNE_FIELD(funct2, 25) | GNNE_FIELD(rstride_d, 27))
#define GNNE_MFU_PDP1_CONF2(rcount_w, rcount_h, rpe_h, rpe_last_h) \
    (0x62 | GNNE_FIELD(0x02, 7) | GNNE_FIELD(rcount_w, 12) | \
     GNNE_FIELD(rcount_h, 17) | GNNE_FIELD(rpe_h, 22) | \
     GNNE_FIELD(rpe_last_h, 27))
#define GNNE_MFU_PDP1_CONF3(rpe_channels, rpe_last_channels, rpad_value, \
                            sspad) \
    (0x62 | GNNE_FIELD(0x03, 7) | GNNE_FIELD(rpe_channels, 12) | \
     GNNE_FIELD(rpe_last_channels, 17) | GNNE_FIELD(rpad_value, 22) | \
     GNNE_FIELD(sspad, 27))
#define GNNE_MFU_PDP1_CONF4(rwindow_w, rwindow_h, rscale, enable_h2c, \
                            enable_bw) \
    (0x62 | GNNE_FIELD(0x04, 7) | GNNE_FIELD(rwindow_w, 12) | \
     GNNE_FIELD(rwindow_h, 17) | GNNE_FIELD(rscale, 22) | \
     GNNE_FIELD(enable_h2c, 27) | GNNE_FIELD(enable_bw, 28))
#define GNNE_MFU_PDP1_CONF_DEQ(rscale, rbias, quant_type, rshift) \
    (0x62 | GNNE_FIELD(0x06, 7) | GNNE_FIELD(rscale, 12) | \
     GNNE_FIELD(rbias, 17) | GNNE_FIELD(quant_type, 22) | \
     GNNE_FIELD(rshift, 24))
#define GNNE_MFU_PDP1_CONF_QUANT(rscale, rbias, quant_type, rshift) \
    (0x62 | GNNE_FIELD(0x07, 7) | GNNE_FIELD(rscale, 12) | \
     GNNE_FIELD(rbias, 17) | GNNE_FIELD(quant_type, 22) | \
     GNNE_FIELD(rshift, 24))
#define GNNE_MFU_PDP1_COMPUTE(raddr_d, raddr_s, rshape) \
    (0x6a | GNNE_FIELD(raddr_d, 7) | GNNE_FIELD(raddr_s, 12) | \
     GNNE_FIELD(rshape, 17))
#define GNNE_MFU_ACT1_CONF_STRIDE(rstride_s1, rstride_s2, rstride_d1) \
    (0x62 | GNNE_FIELD(0x08, 7) | GNNE_FIELD(rstride_s1, 12) | \
     GNNE_FIELD(rstride_s2, 15) | GNNE_FIELD(rstride_d1, 18))
#define GNNE_MFU_ACT1_CONF_SRC1(rslice, rright_repeats, rslice_repeats, sid, \
                                slice_loc) \
    (0x62 | GNNE_FIELD(0x09, 7) | GNNE_FIELD(rslice, 12) | \
     GNNE_FIELD(rright_repeats, 17) | GNNE_FIELD(rslice_repeats, 22) | \
     GNNE_FIELD(sid, 27) | GNNE_FIELD(slice_loc, 28))
#define GNNE_MFU_ACT1_CONF_SRC2(rleft_repeats, rshape, sid, source_type) \
    (0x62 | GNNE_FIELD(0x0a, 7) | GNNE_FIELD(rleft_repeats, 12) | \
     GNNE_FIELD(rshape, 17) | GNNE_FIELD(sid, 20) | \
     GNNE_FIELD(source_type, 21))
#define GNNE_MFU_ACT1_CONF_DEST(rlen, rshape) \
    (0x62 | GNNE_FIELD(0x0b, 7) | GNNE_FIELD(rlen, 12) | \
     GNNE_FIELD(rshape, 17))
#define GNNE_MFU_ACT1_CONF_DEQ(rscale, rbias, quant_type, sid, rshift) \
    (0x62 | GNNE_FIELD(0x0c, 7) | GNNE_FIELD(rscale, 12) | \
     GNNE_FIELD(rbias, 17) | GNNE_FIELD(quant_type, 22) | \
     GNNE_FIELD(sid, 24) | GNNE_FIELD(rshift, 25))
#define GNNE_MFU_ACT1_CONF_QUANT(quant_type, rshift) \
    (0x62 | GNNE_FIELD(0x0d, 7) | GNNE_FIELD(quant_type, 12) | \
     GNNE_FIELD(rshift, 14))
#define GNNE_MFU_ACT1_CONF(funct4, is_by_channel, is_16_segments) \
    (0x62 | GNNE_FIELD(0x0e, 7) | GNNE_FIELD(funct4, 12) | \
     GNNE_FIELD(is_by_channel, 16) | GNNE_FIELD(is_16_segments, 17))
#define GNNE_MFU_ACT1_COMPUTE(raddr_d1, raddr_s1, raddr_s2, raddr_arg) \
    (0x72 | GNNE_FIELD(raddr_d1, 7) | GNNE_FIELD(raddr_s1, 12) | \
     GNNE_FIELD(raddr_s2, 17) | GNNE_FIELD(raddr_arg, 22))
#define GNNE_MFU_TRANSPOSE_CONF(rstride_d, rstride_s, l2_dt, permute) \
    (0x62 | GNNE_FIELD(rstride_d, 12) | GNNE_FIELD(rstride_s, 15) | \
     GNNE_FIELD(l2_dt, 18) | GNNE_FIELD(permute, 20))
#define GNNE_MFU_TRANSPOSE(raddr_d, raddr_s, rshape) \
    (0x68 | GNNE_FIELD(raddr_d, 7) | GNNE_FIELD(raddr_s, 12) | \
     GNNE_FIELD(rshape, 17))
#define GNNE_DM_LOAD_L1_CONF(tcu, pu, rstride_s, datatype, l1_type) \
    (0x50 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(0, 13) | GNNE_FIELD(rstride_s, 17) | \
     GNNE_FIELD(datatype, 20) | GNNE_FIELD(l1_type, 22))
#define GNNE_DM_LOAD_W_CONF(tcu, pu, kernel_h, kernel_w, rstride_oc) \
    (0x50 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(1, 13) | GNNE_FIELD(kernel_h, 17) | \
     GNNE_FIELD(kernel_w, 22) | GNNE_FIELD(rstride_oc, 27))
#define GNNE_DM_LOAD_W_CONF_DEQ(tcu, pu, quant_type) \
    (0x50 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(2, 13) | GNNE_FIELD(quant_type, 17))
#define GNNE_DM_LOAD_W_CONF2(tcu, pu, rgroups, rgoc) \
    (0x50 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(5, 13) | GNNE_FIELD(rgroups, 17) | \
     GNNE_FIELD(rgoc, 22))
#define GNNE_DM_LOAD_L1(tcu, pu, raddr_s, rhtoc_window, rshape, l1_type) \
    (0x52 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(raddr_s, 13) | GNNE_FIELD(rhtoc_window, 18) | \
     GNNE_FIELD(rshape, 23) | GNNE_FIELD(l1_type, 26))
#define GNNE_DM_LOAD_W(tcu, pu, raddr_s, raddr_bw, r_iochannels, dest_type) \
    (0x54 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(raddr_s, 13) | GNNE_FIELD(raddr_bw, 18) | \
     GNNE_FIELD(r_iochannels, 23) | GNNE_FIELD(dest_type, 26))
#define GNNE_DM_LOAD_ACT0(tcu, pu, raddr_s, rlen, dest_channel, by_channel) \
    (0x56 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(raddr_s, 13) | GNNE_FIELD(rlen, 18) | \
     GNNE_FIELD(dest_channel, 23) | GNNE_FIELD(by_channel, 24))
#define GNNE_DM_STORE_OF(tcu, pu, raddr_d, rshape, src_channel) \
    (0x58 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(raddr_d, 13) | GNNE_FIELD(rshape, 18) | \
     GNNE_FIELD(src_channel, 21))
#define GNNE_PU_FETCHIF_CONF1(tcu, pu, stride_w, stride_h, rstride_s) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(0, 13) | GNNE_FIELD(stride_w, 17) | \
     GNNE_FIELD(stride_h, 22) | GNNE_FIELD(rstride_s, 27))
#define GNNE_PU_FETCHIF_CONF2(tcu, pu, rgic, rgic_last) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(1, 13) | GNNE_FIELD(rgic, 17) | \
     GNNE_FIELD(rgic_last, 22))
#define GNNE_PU_FETCHIF_CONF3(tcu, pu, raddr_s, rgroups, rshape) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(2, 13) | GNNE_FIELD(raddr_s, 17) | \
     GNNE_FIELD(rgroups, 22) | GNNE_FIELD(rshape, 27))
#define GNNE_PU_FETCHIF_CONF4(tcu, pu, rpad_value, sspad) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(3, 13) | GNNE_FIELD(rpad_value, 17) | \
     GNNE_FIELD(sspad, 27))
#define GNNE_PU_FETCHIF_CONF_DEQ(tcu, pu, ric, rbx, quant_type) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(4, 13) | GNNE_FIELD(ric, 17) | GNNE_FIELD(rbx, 22) | \
     GNNE_FIELD(quant_type, 27))
#define GNNE_PU_W_CONF(tcu, pu, kernel_h, kernel_w) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(5, 13) | GNNE_FIELD(kernel_h, 17) | \
     GNNE_FIELD(kernel_w, 22))
#define GNNE_PU_OF_CONF1(tcu, pu, rgoc, rgoc_last, rstride_d) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(6, 13) | GNNE_FIELD(rgoc, 17) | \
     GNNE_FIELD(rgoc_last, 22) | GNNE_FIELD(rstride_d, 27))
#define GNNE_PU_OF_CONF2(tcu, pu, raddr_d, rshape_d) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(7, 13) | GNNE_FIELD(raddr_d, 17) | \
     GNNE_FIELD(rshape_d, 27))
#define GNNE_PU_COMPUTE_CONF(tcu, pu, load_psum, clr_psum, dest_target, \
                             release_if, mode) \
    (0x5a | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(8, 13) | GNNE_FIELD(load_psum, 17) | \
     GNNE_FIELD(clr_psum, 18) | GNNE_FIELD(dest_target, 19) | \
     GNNE_FIELD(release_if, 20) | GNNE_FIELD(mode, 21))
#define GNNE_ACT0_SRC1_CONF(tcu, pu, channel, rshape, rshift_bits) \
    (0x60 | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(channel, 16) | GNNE_FIELD(rshape, 17) | \
     GNNE_FIELD(rshift_bits, 20))
#define GNNE_ACT0_COMPUTE(raddr_d, tcu, channel, target, datatype, by_channel) \
    (0x74 | GNNE_FIELD(raddr_d, 12) | GNNE_FIELD(tcu, 17) | \
     GNNE_FIELD(channel, 20) | GNNE_FIELD(target, 21) | \
     GNNE_FIELD(datatype, 23) | GNNE_FIELD(by_channel, 25))
#define GNNE_PU_COMPUTE(tcu, of_shift_mode) \
    (0x4d | GNNE_FIELD(tcu, 7) | GNNE_FIELD(of_shift_mode, 10))
#define GNNE_PDP0_MODE_CONF(tcu, pu, mode) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(0, 13) | GNNE_FIELD(mode, 17))
#define GNNE_PDP0_FETCHIF_CONF1(tcu, pu, stride_w, stride_h) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(1, 13) | GNNE_FIELD(stride_w, 17) | \
     GNNE_FIELD(stride_h, 22))
#define GNNE_PDP0_FETCHIF_CONF2(tcu, pu, rgic, rgic_last) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(2, 13) | GNNE_FIELD(rgic, 17) | \
     GNNE_FIELD(rgic_last, 22))
#define GNNE_PDP0_FETCHIF_CONF3(tcu, pu, rshape) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(3, 13) | GNNE_FIELD(rshape, 27))
#define GNNE_PDP0_FETCHIF_CONF4(tcu, pu, rpad_value, sspad) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(4, 13) | GNNE_FIELD(rpad_value, 17) | \
     GNNE_FIELD(sspad, 22))
#define GNNE_PDP0_FETCHIF_CONF_DEQ(tcu, pu, rbx, quant_type) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(5, 13) | GNNE_FIELD(rbx, 17) | \
     GNNE_FIELD(quant_type, 22))
#define GNNE_PDP0_W_CONF(tcu, pu, kernel_h, kernel_w) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(6, 13) | GNNE_FIELD(kernel_h, 17) | \
     GNNE_FIELD(kernel_w, 22))
#define GNNE_PDP0_OF_CONF(tcu, pu, rstride_d, rshape_d) \
    (0x5e | GNNE_FIELD(tcu, 7) | GNNE_FIELD(pu, 10) | \
     GNNE_FIELD(7, 13) | GNNE_FIELD(rstride_d, 17) | \
     GNNE_FIELD(rshape_d, 20))
#define GNNE_PDP0_COMPUTE(tcu, raddr_s) \
    (0x4f | GNNE_FIELD(tcu, 7) | GNNE_FIELD(raddr_s, 10))

static void k230_plic_enable_irq(QTestState *qts, unsigned int irq)
{
    uint32_t enable;
    uint64_t enable_addr;

    qtest_writel(qts, K230_PLIC_BASE + irq * 4, 1);
    enable_addr = K230_PLIC_BASE + K230_PLIC_ENABLE_S + (irq / 32) * 4;
    enable = qtest_readl(qts, enable_addr);
    qtest_writel(qts, enable_addr, enable | (1u << (irq % 32)));
    qtest_writel(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                 K230_PLIC_THRESHOLD, 0);
}

static uint32_t k230_plic_claim(QTestState *qts)
{
    return qtest_readl(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                       K230_PLIC_CLAIM);
}

static void k230_plic_complete(QTestState *qts, unsigned int irq)
{
    qtest_writel(qts, K230_PLIC_BASE + K230_PLIC_CONTEXT_S +
                 K230_PLIC_CLAIM, irq);
}

static void k230_assert_page_byte(QTestState *qts, uint64_t addr,
                                  uint8_t expected)
{
    static const uint64_t offsets[] = {
        0,
        K230_KPU_PAGE_SIZE / 2,
        K230_KPU_PAGE_SIZE - 32,
    };
    uint8_t data[32];

    for (size_t i = 0; i < G_N_ELEMENTS(offsets); i++) {
        qtest_memread(qts, addr + offsets[i], data, sizeof(data));
        for (size_t j = 0; j < sizeof(data); j++) {
            g_assert_cmphex(data[j], ==, expected);
        }
    }
}

static QTestState *k230_kpu_init(void)
{
    QTestState *qts = qtest_init("-machine k230-canmv");

    k230_plic_enable_irq(qts, K230_GNNE_IRQ);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);

    return qts;
}

static void k230_kpu_set_command_range(QTestState *qts, uint32_t start,
                                       uint32_t end)
{
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_COMMAND_START, start);
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_COMMAND_END, end);
}

static void k230_kpu_write_commands(QTestState *qts, const uint32_t *commands,
                                    size_t command_count)
{
    for (size_t i = 0; i < command_count; i++) {
        qtest_writel(qts, K230_GNNE_COMMAND_TEST + i * sizeof(commands[0]),
                     commands[i]);
    }
}

static void k230_kpu_start(QTestState *qts)
{
    qtest_writeq(qts, K230_KPU_CFG_BASE + K230_GNNE_CONTROL,
                 K230_GNNE_START);
}

static void k230_kpu_run_commands(QTestState *qts, const uint32_t *commands,
                                  size_t command_count)
{
    k230_kpu_write_commands(qts, commands, command_count);
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST,
                               K230_GNNE_COMMAND_TEST +
                               command_count * sizeof(commands[0]));
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);
}

static void k230_kpu_command_u32(uint8_t *commands, size_t *offset,
                                 uint32_t word)
{
    stl_le_p(commands + *offset, word);
    *offset += sizeof(word);
}

static void k230_kpu_command_u16(uint8_t *commands, size_t *offset,
                                 uint16_t word)
{
    stw_le_p(commands + *offset, word);
    *offset += sizeof(word);
}

static uint16_t k230_test_fp16_from_uint(unsigned int value)
{
    unsigned int exp = 0;
    unsigned int norm = value;
    unsigned int frac;

    g_assert_cmpuint(value, >, 0);
    g_assert_cmpuint(value, <, 1024);

    while (norm > 1) {
        norm >>= 1;
        exp++;
    }
    frac = (value << (10 - exp)) & 0x3ff;
    return ((exp + 15) << 10) | frac;
}

static void k230_kpu_run_command_bytes(QTestState *qts,
                                       const uint8_t *commands, size_t size)
{
    qtest_memwrite(qts, K230_GNNE_COMMAND_TEST, commands, size);
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST,
                               K230_GNNE_COMMAND_TEST + size);
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);
}

static void k230_kpu_run_command_bytes_at(QTestState *qts, uint32_t start,
                                          const uint8_t *commands,
                                          size_t size)
{
    qtest_memwrite(qts, start, commands, size);
    k230_kpu_set_command_range(qts, start, start + size);
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);
}

static void k230_kpu_assert_done_irq(QTestState *qts)
{
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, K230_GNNE_DONE);
    g_assert_cmphex(k230_plic_claim(qts), ==, K230_GNNE_IRQ);
}

static void k230_kpu_clear_done_irq(QTestState *qts)
{
    qtest_writeq(qts, K230_KPU_CFG_BASE + K230_GNNE_CLEAR, K230_GNNE_DONE);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    k230_plic_complete(qts, K230_GNNE_IRQ);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
}

static void test_zero_start_does_not_complete(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    k230_kpu_write_commands(qts, commands, G_N_ELEMENTS(commands));
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST,
                               K230_GNNE_COMMAND_TEST + sizeof(commands));
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_COMMAND_HI, 0x12345678);

    qtest_writeq(qts, K230_KPU_CFG_BASE + K230_GNNE_CONTROL, 0);
    qtest_writel(qts, K230_KPU_CFG_BASE + K230_GNNE_SETUP, 0x30d40);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);

    g_assert_cmphex(qtest_readl(qts, K230_KPU_CFG_BASE +
                                K230_GNNE_COMMAND_HI), ==, 0x12345678);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);

    qtest_quit(qts);
}

static void test_delayed_completion(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    k230_kpu_write_commands(qts, commands, G_N_ELEMENTS(commands));
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST,
                               K230_GNNE_COMMAND_TEST + sizeof(commands));
    k230_kpu_start(qts);

    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);

    qtest_clock_step(qts, K230_GNNE_DELAY_NS / 2);
    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    g_assert_cmphex(k230_plic_claim(qts), ==, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);

    qtest_clock_step(qts, K230_GNNE_DELAY_NS - K230_GNNE_DELAY_NS / 2);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_command_completion_pages(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
        K230_KPU_OUTSIDE_LOW | 2,
        K230_KPU_OUTPUT_TEST1 + 0x40,
        (K230_KPU_OUTPUT_TEST0 + 0x80) | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_TEST1, 0x5a, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_UNREFERENCED, 0xc3,
                 K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTSIDE_LOW, 0x3c, K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST1, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_UNREFERENCED, 0xc3);
    k230_assert_page_byte(qts, K230_KPU_OUTSIDE_LOW, 0x3c);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_same_page_addresses_zero_one_page(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 + 0x20,
        K230_KPU_OUTPUT_TEST0 + 0x8f0,
        K230_KPU_OUTPUT_TEST0 + 0xffc,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_TEST0 + K230_KPU_PAGE_SIZE, 0x5a,
                 K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0 + K230_KPU_PAGE_SIZE,
                          0x5a);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_output_window_boundaries(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 + 1,
        K230_KPU_OUTPUT_LAST + 0x80,
        K230_KPU_OUTSIDE_LOW + 2,
        K230_KPU_OUTSIDE_HIGH,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_LAST, 0x5a, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTSIDE_LOW, 0x3c, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTSIDE_HIGH, 0xc3, K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_LAST, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTSIDE_LOW, 0x3c);
    k230_assert_page_byte(qts, K230_KPU_OUTSIDE_HIGH, 0xc3);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_empty_command_range(void)
{
    QTestState *qts = k230_kpu_init();

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);

    k230_kpu_set_command_range(qts, 0, 0);
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_reversed_command_range(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    k230_kpu_write_commands(qts, commands, G_N_ELEMENTS(commands));
    k230_kpu_set_command_range(qts, K230_GNNE_COMMAND_TEST + sizeof(commands),
                               K230_GNNE_COMMAND_TEST);
    k230_kpu_start(qts);
    qtest_clock_step(qts, K230_GNNE_DELAY_NS);

    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0xa5);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_clear_status_allows_second_run(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t first_commands[] = {
        K230_KPU_OUTPUT_TEST0 | 2,
    };
    const uint32_t second_commands[] = {
        K230_KPU_OUTPUT_TEST1 | 2,
    };

    qtest_memset(qts, K230_KPU_OUTPUT_TEST0, 0xa5, K230_KPU_PAGE_SIZE);
    qtest_memset(qts, K230_KPU_OUTPUT_TEST1, 0x5a, K230_KPU_PAGE_SIZE);

    k230_kpu_run_commands(qts, first_commands, G_N_ELEMENTS(first_commands));
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST0, 0);
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST1, 0x5a);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    g_assert_cmphex(qtest_readq(qts, K230_KPU_CFG_BASE + K230_GNNE_STATUS),
                    ==, 0);
    k230_kpu_run_commands(qts, second_commands, G_N_ELEMENTS(second_commands));
    k230_assert_page_byte(qts, K230_KPU_OUTPUT_TEST1, 0);
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_store_copies_parsed_output(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
        0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x180),
        GNNE_ADDI(7, 0, 0x340),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(3, 7, 0),
    };
    uint8_t data[32];

    qtest_memwrite(qts, K230_GNNE_SYNTH_STORE_SOURCE,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_store_converts_fp16_to_fp32(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x00, 0x3c,             /* fp16 1.0 */
        0x00, 0x40,             /* fp16 2.0 */
    };
    const uint8_t expected[] = {
        0x00, 0x00, 0x80, 0x3f, /* fp32 1.0 */
        0x00, 0x00, 0x00, 0x40, /* fp32 2.0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x340),
        GNNE_ADDI(3, 0, 0x180),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, 2),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_STORE_CONF(1, 0, 1, 2),
        GNNE_L2_STORE(3, 2, 0),
    };
    uint8_t data[12];

    qtest_memwrite(qts, K230_GNNE_SYNTH_STORE_SOURCE,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(expected) ? expected[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_copies_to_glb(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };
    const uint32_t commands[] = {
        GNNE_LUI(6, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x240),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_LOAD_CONF(1, 0, 0, 0),
        GNNE_L2_LOAD(3, 6, 0),
    };
    uint8_t data[16];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE, source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_LOAD_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_LOAD_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_uses_packed_strides(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x11, 0x12, 0xa5, 0xa5, 0xa5, 0x21, 0x22, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0x31,
        0x32, 0xa5, 0xa5, 0xa5, 0x41, 0x42, 0xa5, 0xa5,
    };
    const uint8_t expected[] = {
        0x11, 0x12, 0x21, 0x22, 0x31, 0x32, 0x41, 0x42,
    };
    const uint32_t commands[] = {
        GNNE_LUI(6, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(3, 0, 0x240),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 3),
        GNNE_ADDI(11, 0, 5),
        GNNE_MMU_CONF(0, 9, 0),
        GNNE_SS_PACK_SHAPE(4, 9, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(9, 10, 11, 0),
        GNNE_SS_PACK_STRIDE(9, 9, 9, 1),
        GNNE_L2_LOAD_CONF(1, 0, 0, 0),
        GNNE_L2_LOAD(3, 6, 0),
    };
    uint8_t data[12];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE, source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_LOAD_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_LOAD_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(expected) ? expected[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_store_uses_packed_strides(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x11, 0x12, 0xa5, 0xa5, 0xa5, 0x21, 0x22, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0x31,
        0x32, 0xa5, 0xa5, 0xa5, 0x41, 0x42, 0xa5, 0xa5,
    };
    const uint8_t expected[] = {
        0x11, 0x12, 0x21, 0x22, 0x31, 0x32, 0x41, 0x42,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x340),
        GNNE_ADDI(3, 0, 0x180),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 3),
        GNNE_ADDI(11, 0, 5),
        GNNE_MMU_CONF(0, 9, 0),
        GNNE_SS_PACK_SHAPE(4, 9, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(9, 10, 11, 0),
        GNNE_SS_PACK_STRIDE(9, 9, 9, 1),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(3, 2, 0),
    };
    uint8_t data[12];

    qtest_memwrite(qts, K230_GNNE_SYNTH_STORE_SOURCE,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(expected) ? expected[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_store_reads_high_mmu0_glb(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x6a, 0x6b, 0x6c, 0x6d,
    };
    const uint32_t commands[] = {
        GNNE_LUI(2, 0x12),
        GNNE_ADDI(3, 0, 0x180),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_MMU_CONF(0, 4, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(3, 2, 0),
    };
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_GLB_BASE + 0x12000,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_nonzero_bank_uses_raw_offset(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t expected[] = {
        0x31, 0x32, 0x33, 0x34,
    };
    const uint8_t poison[] = {
        0xa1, 0xa2, 0xa3, 0xa4,
    };
    const uint32_t commands[] = {
        GNNE_LUI(2, 0x10000),
        GNNE_ADDI(2, 2, 0x500),
        GNNE_LUI(3, 0x10000),
        GNNE_ADDI(3, 3, 0x700),
        GNNE_ADDI(4, 0, 0x180),
        GNNE_ADDI(5, 0, 0x184),
        GNNE_ADDI(6, 0, 1),
        GNNE_ADDI(7, 0, sizeof(expected)),
        GNNE_LUI(8, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_LUI(9, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(9, 9, 0x100),
        GNNE_ADDI(10, 0, 0),
        GNNE_ADDI(11, 0, 0x20),
        GNNE_ADDI(12, 0, 0x10),
        GNNE_MMU_CONF(10, 11, 0),
        GNNE_MMU_CONF(10, 11, 1),
        GNNE_SS_PACK_SHAPE(6, 6, 6, 7, 0),
        GNNE_SS_PACK_STRIDE(7, 7, 7, 0),
        GNNE_SS_PACK_STRIDE(7, 7, 7, 1),
        GNNE_L2_LOAD_CONF(1, 0, 0, 0),
        GNNE_L2_LOAD(3, 9, 0),
        GNNE_MMU_CONF(12, 11, 1),
        GNNE_L2_LOAD(2, 8, 0),
        GNNE_MMU_CONF(10, 11, 1),
        GNNE_L2_LOAD(3, 9, 0),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(4, 2, 0),
        GNNE_MMU_CONF(12, 11, 1),
        GNNE_L2_STORE(5, 2, 0),
    };
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE, expected,
                   sizeof(expected));
    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE + 0x100, poison,
                   sizeof(poison));
    qtest_memset(qts, K230_GNNE_SYNTH_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(expected); i++) {
        g_assert_cmphex(data[i], ==, expected[i]);
        g_assert_cmphex(data[i + sizeof(expected)], ==, expected[i]);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_bank_write_updates_logical_glb(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t expected[] = {
        0x41, 0x42, 0x43, 0x44,
    };
    const uint8_t poison[] = {
        0xa1, 0xa2, 0xa3, 0xa4,
    };
    const uint32_t commands[] = {
        GNNE_LUI(2, 0x10000),
        GNNE_ADDI(3, 0, 0x200),
        GNNE_ADDI(4, 0, 0x180),
        GNNE_ADDI(6, 0, 1),
        GNNE_ADDI(7, 0, sizeof(expected)),
        GNNE_LUI(8, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(10, 0, 0x10),
        GNNE_ADDI(11, 0, 0x20),
        GNNE_MMU_CONF(0, 11, 0),
        GNNE_MMU_CONF(10, 11, 1),
        GNNE_SS_PACK_SHAPE(6, 6, 6, 7, 0),
        GNNE_SS_PACK_STRIDE(7, 7, 7, 0),
        GNNE_SS_PACK_STRIDE(7, 7, 7, 1),
        GNNE_L2_LOAD_CONF(1, 0, 0, 0),
        GNNE_L2_LOAD(2, 8, 0),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(4, 3, 0),
    };
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE, expected,
                   sizeof(expected));
    qtest_memwrite(qts, K230_GNNE_SYNTH_GLB_BASE + 0x200, poison,
                   sizeof(poison));
    qtest_memset(qts, K230_GNNE_SYNTH_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(expected) ? expected[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_converts_fp32_to_fp16(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x00, 0x00, 0x80, 0x3f, /* fp32 1.0 */
        0x00, 0x00, 0x00, 0x40, /* fp32 2.0 */
        0x00, 0x00, 0xc0, 0x7f, /* fp32 qNaN */
    };
    const uint8_t expected[] = {
        0x00, 0x3c,             /* fp16 1.0 */
        0x00, 0x40,             /* fp16 2.0 */
        0xff, 0x7b,             /* fp16 max finite */
    };
    const uint32_t commands[] = {
        GNNE_LUI(6, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x240),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, 3),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_LOAD_CONF(1, 0, 1, 2),
        GNNE_L2_LOAD(3, 6, 0),
    };
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE, source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_LOAD_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_LOAD_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(expected) ? expected[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_then_store_roundtrip(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    };
    const uint32_t commands[] = {
        GNNE_LUI(6, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(6, 6, 0x200),
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x240),
        GNNE_ADDI(7, 0, 0x380),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_LOAD_CONF(1, 0, 0, 0),
        GNNE_L2_LOAD(3, 6, 0),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(7, 3, 0),
    };
    uint8_t data[16];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE + 0x200,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_ROUNDTRIP_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_ROUNDTRIP_OUTPUT, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_ai2d_compute_preserves_command_stream(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8,
    };
    uint8_t commands[128];
    uint8_t data[16];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 0, 0x380));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 0x340));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(5, 0, sizeof(source)));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 5, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 5, 5, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_STORE_CONF(1, 0, 0, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_AI2D_COMPUTE());
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_STORE(3, 7, 0));

    qtest_memwrite(qts, K230_GNNE_SYNTH_STORE_SOURCE,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_ROUNDTRIP_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_ROUNDTRIP_OUTPUT, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_uses_lane_layout(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23,
    };
    const uint32_t commands[] = {
        GNNE_LUI(6, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(6, 6, 0x100),
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x300),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_ADDI(8, 0, 3),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_L2_LOAD_W_CONF(5, 5, 0, 0, 0),
        GNNE_L2_LOAD_W(3, 6, 8),
    };
    uint8_t data[32];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE + 0x100,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_WEIGHT_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_WEIGHT_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        uint8_t expected = 0xa5;

        if (i < 4) {
            expected = source[i];
        } else if (i >= 24 && i < 28) {
            expected = source[i - 20];
        }
        g_assert_cmphex(data[i], ==, expected);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_conf_latches_rlen(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x30, 0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x43,
    };
    const uint32_t commands[] = {
        GNNE_LUI(6, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(6, 6, 0x120),
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x300),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_ADDI(8, 0, 3),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_L2_LOAD_W_CONF(5, 5, 0, 0, 0),
        GNNE_ADDI(5, 6, 0),
        GNNE_L2_LOAD_W(3, 5, 8),
    };
    uint8_t data[32];

    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE + 0x120,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_WEIGHT_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_WEIGHT_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        uint8_t expected = 0xa5;

        if (i < 4) {
            expected = source[i];
        } else if (i >= 24 && i < 28) {
            expected = source[i - 20];
        }
        g_assert_cmphex(data[i], ==, expected);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_translates_low_source(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t raw_source[] = {
        0x11, 0x12, 0x13, 0x14,
    };
    const uint8_t translated_source[] = {
        0x91, 0x92, 0x93, 0x94,
    };
    uint8_t commands[128];
    uint8_t data[16];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x20));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 0, 0x180));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 4));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 3));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(2, 7, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_W_CONF(5, 5, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD_W(3, 6, 8));

    qtest_memwrite(qts, K230_GNNE_RUNTIME_DDR_BASE + 0x100,
                   raw_source, sizeof(raw_source));
    qtest_memwrite(qts, K230_GNNE_RUNTIME_DDR_BASE + 0x500,
                   translated_source, sizeof(translated_source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x580, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x580, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(translated_source) ?
                        translated_source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_rebases_function_source(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t raw_source[] = {
        0x21, 0x22, 0x23, 0x24,
    };
    const uint8_t rebased_source[] = {
        0xa1, 0xa2, 0xa3, 0xa4,
    };
    uint8_t commands[128];
    uint8_t data[16];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 0x8));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 4));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(6, 0x103));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 6, 0xb07));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 3));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(2, 7, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_W_CONF(5, 5, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD_W(3, 6, 8));

    qtest_memwrite(qts, K230_GNNE_RUNTIME_DDR_BASE + 0x102b07,
                   raw_source, sizeof(raw_source));
    qtest_memwrite(qts, K230_GNNE_RUNTIME_DDR_BASE + 0x102d07,
                   rebased_source, sizeof(rebased_source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(rebased_source) ?
                        rebased_source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_rebases_absolute_rdata_source(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t source = K230_GNNE_RUNTIME_RDATA_BASE + 0x1000;
    const uint8_t raw_source[] = {
        0x00, 0x00, 0x00, 0x00,
    };
    const uint8_t rebased_source[] = {
        0xb1, 0xb2, 0xb3, 0xb4,
    };
    uint8_t commands[128];
    uint8_t data[16];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 0x8));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 4));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_LUI(6, source >> 12));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(6, 6, source & 0xfff));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 3));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(2, 7, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_W_CONF(5, 5, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD_W(3, 6, 8));

    qtest_memwrite(qts, source, raw_source, sizeof(raw_source));
    qtest_memwrite(qts, source + 0x200,
                   rebased_source, sizeof(rebased_source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(rebased_source) ?
                        rebased_source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_keeps_absolute_rdata_source(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t source = K230_GNNE_RUNTIME_RDATA_BASE + 0x3000;
    const uint8_t raw_source[] = {
        0x41, 0x42, 0x43, 0x44,
    };
    const uint8_t rebased_source[] = {
        0xc1, 0xc2, 0xc3, 0xc4,
    };
    uint8_t commands[128];
    uint8_t data[16];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 0x8));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 4));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_LUI(6, source >> 12));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(6, 6, source & 0xfff));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 3));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(2, 7, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_W_CONF(5, 5, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD_W(3, 6, 8));

    qtest_memwrite(qts, source, raw_source, sizeof(raw_source));
    qtest_memwrite(qts, source + 0x200,
                   rebased_source, sizeof(rebased_source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(raw_source) ? raw_source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_synthesizes_function_arg(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t poison[] = {
        0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8,
        0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
    };
    const uint8_t expected[] = {
        0x00, 0x00, 0x00, 0x38, 0x00, 0x38, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xfc, 0x00, 0x7c,
    };
    uint8_t commands[128];
    uint8_t data[32];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 0x8));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 7));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(6, 0x0a));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 6, 0xcde));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 23));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(2, 7, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_W_CONF(5, 5, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD_W(3, 6, 8));

    qtest_memwrite(qts, K230_GNNE_RUNTIME_DDR_BASE + 0x9cde,
                   poison, sizeof(poison));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, data,
                  sizeof(data));

    g_assert_cmpmem(data, sizeof(expected), expected, sizeof(expected));
    for (size_t i = sizeof(expected); i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void k230_l2_load_w_rdata_arg_case(uint32_t source,
                                          const uint8_t *expected,
                                          size_t expected_size)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t poison[] = {
        0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8,
        0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
    };
    uint8_t commands[128];
    uint8_t data[32];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 0x8));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 7));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(6, source >> 12));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(6, 6, source & 0xfff));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 23));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(2, 7, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_W_CONF(5, 5, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD_W(3, 6, 8));

    qtest_memwrite(qts, source, poison, sizeof(poison));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x8000, data,
                  sizeof(data));

    g_assert_cmpmem(data, expected_size, expected, expected_size);
    for (size_t i = expected_size; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_w_synthesizes_rdata_function_args(void)
{
    const uint8_t identity[] = {
        0x00, 0x00, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xfc, 0x00, 0x7c,
    };
    const uint8_t half[] = {
        0x00, 0x00, 0x00, 0x38, 0x00, 0x38, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xfc, 0x00, 0x7c,
    };
    const uint8_t bbox_scale[] = {
        0x00, 0x00, 0x1c, 0x68, 0x1c, 0x68, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xe7, 0xff, 0x67,
    };

    k230_l2_load_w_rdata_arg_case(K230_GNNE_RUNTIME_RDATA_BASE,
                                  identity, sizeof(identity));
    k230_l2_load_w_rdata_arg_case(K230_GNNE_RUNTIME_RDATA_BASE + 0x20de,
                                  half, sizeof(half));
    k230_l2_load_w_rdata_arg_case(K230_GNNE_RUNTIME_RDATA_BASE + 0x210b,
                                  bbox_scale, sizeof(bbox_scale));
}

static void test_l2_load_w_uses_rdata_fallback_base(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x6a,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 1),
        GNNE_ADDI(4, 0, 0x180),
        GNNE_ADDI(5, 0, 0),
        GNNE_ADDI(7, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_L2_LOAD_W_CONF(3, 3, 0, 0, 0),
        GNNE_LW(5, 0, 8),
        GNNE_ADDI(5, 5, 0x120),
        GNNE_L2_LOAD_W(4, 5, 7),
    };
    uint8_t alias[4];
    uint8_t data[8];

    stl_le_p(alias, K230_GNNE_RDATA_ALIAS_BASE);
    qtest_memwrite(qts, K230_GNNE_RDATA_FALLBACK_BASE + 8,
                   alias, sizeof(alias));
    qtest_memwrite(qts, K230_GNNE_RDATA_FALLBACK_BASE + 0x120,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, source[0]);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_runtime_function_command_uses_runtime_base(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x71, 0x72, 0x73, 0x74,
    };
    uint8_t commands[128];
    uint8_t data[8];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 0, 0x180));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(5, 0, sizeof(source)));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 5, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_STORE_CONF(0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_STORE(3, 2, 0));

    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x100,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_runtime_rdata_shadow_survives_glb_mutation(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x6a,
    };
    const uint8_t poisoned_source[] = {
        0x99,
    };
    const uint32_t warm_commands[] = {
        GNNE_ADDI(1, 0, 0),
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 1),
        GNNE_ADDI(4, 0, 0x180),
        GNNE_ADDI(5, 0, 0),
        GNNE_ADDI(7, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_L2_LOAD_W_CONF(3, 3, 0, 0, 0),
        GNNE_LW(5, 0, 8),
        GNNE_ADDI(5, 5, 0x120),
        GNNE_L2_LOAD_W(4, 5, 7),
    };
    uint8_t alias[4];
    uint8_t data[8];

    stl_le_p(alias, K230_GNNE_RDATA_ALIAS_BASE);
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 8,
                   alias, sizeof(alias));
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x120,
                   source, sizeof(source));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  (const uint8_t *)warm_commands,
                                  sizeof(warm_commands));
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    stl_le_p(alias, 0);
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 8,
                   alias, sizeof(alias));
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x120,
                   poisoned_source, sizeof(poisoned_source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  (const uint8_t *)commands,
                                  sizeof(commands));
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, data,
                  sizeof(data));

    g_assert_cmphex(data[0], ==, source[0]);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_load_uses_rdata_prefix_shadow(void)
{
    QTestState *qts = k230_kpu_init();
    const uint32_t source_addr = K230_GNNE_RUNTIME_RDATA_BASE + 0x20;
    const uint8_t source[] = {
        0x11, 0x12, 0x13, 0x14,
    };
    const uint8_t poisoned_source[] = {
        0x91, 0x92, 0x93, 0x94,
    };
    const uint32_t warm_commands[] = {
        GNNE_ADDI(1, 0, 0),
    };
    uint8_t commands[160];
    uint8_t data[8];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x180));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_LUI(3, source_addr >> 12));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(3, 3, source_addr & 0xfff));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(5, 0, sizeof(source)));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 5, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 5, 5, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_CONF(1, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD(2, 3, 0));

    qtest_memwrite(qts, source_addr, source, sizeof(source));
    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  (const uint8_t *)warm_commands,
                                  sizeof(warm_commands));
    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_memwrite(qts, source_addr, poisoned_source,
                   sizeof(poisoned_source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, data,
                  sizeof(data));

    g_assert_cmpmem(data, sizeof(source), source, sizeof(source));
    for (size_t i = sizeof(source); i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_runtime_arg_table_drives_direct_io(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x41, 0x42, 0x43, 0x44,
    };
    uint8_t commands[160];
    uint8_t table[16] = {};
    uint8_t data[8];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x200));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(5, 0, sizeof(source)));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 4, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LW(6, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 5, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 5, 5, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_LOAD_CONF(1, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_LOAD(2, 6, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LW(7, 0, 4));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_STORE_CONF(1, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_STORE(7, 2, 0));

    stl_le_p(table, K230_GNNE_RUNTIME_DIRECT_SOURCE);
    stl_le_p(table + 4, K230_GNNE_RUNTIME_DIRECT_OUTPUT);
    stl_le_p(table + 8, K230_GNNE_RUNTIME_RDATA_BASE);
    qtest_memwrite(qts, K230_GNNE_RUNTIME_ARG_TABLE, table, sizeof(table));
    qtest_memwrite(qts, K230_GNNE_RUNTIME_DIRECT_SOURCE,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_RUNTIME_DIRECT_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  commands, command_size);
    qtest_memread(qts, K230_GNNE_RUNTIME_DIRECT_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_store_accepts_rdata_alias_destination(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x51, 0x52, 0x53, 0x54,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_LW(3, 0, 8),
        GNNE_ADDI(3, 3, 0x180),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(3, 2, 0),
    };
    uint8_t alias[4];
    uint8_t data[8];

    stl_le_p(alias, K230_GNNE_RDATA_ALIAS_BASE);
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 8,
                   alias, sizeof(alias));
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x100,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  (const uint8_t *)commands,
                                  sizeof(commands));
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_store_runtime_mirrors_to_ddr_source(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x61, 0x62, 0x63, 0x64,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_LW(3, 0, 8),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(5, 5, 5, 1),
        GNNE_L2_STORE_CONF(1, 0, 0, 0),
        GNNE_L2_STORE(3, 2, 0),
        GNNE_LUI(6, 0x3c000),
        GNNE_ADDI(7, 0, 0x180),
        GNNE_L2_LOAD_CONF(1, 0, 0, 0),
        GNNE_L2_LOAD(7, 6, 0),
    };
    uint8_t alias[4];
    uint8_t ddr[sizeof(source)];
    uint8_t data[8];

    stl_le_p(alias, K230_GNNE_RDATA_ALIAS_BASE);
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 8,
                   alias, sizeof(alias));
    qtest_memwrite(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x100,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE, 0xa5,
                 sizeof(data));
    qtest_memset(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, 0xa5,
                 sizeof(data));
    qtest_memset(qts, K230_GNNE_RUNTIME_DDR_BASE, 0,
                 sizeof(source));

    k230_kpu_run_command_bytes_at(qts, K230_GNNE_RUNTIME_FUNCTION_COMMAND,
                                  (const uint8_t *)commands,
                                  sizeof(commands));
    qtest_memread(qts, K230_GNNE_RUNTIME_DDR_BASE, ddr, sizeof(ddr));
    qtest_memread(qts, K230_GNNE_RUNTIME_RDATA_BASE + 0x180, data,
                  sizeof(data));

    g_assert_cmpmem(ddr, sizeof(ddr), source, sizeof(source));
    g_assert_cmpmem(data, sizeof(source), source, sizeof(source));
    for (size_t i = sizeof(source); i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_l2_store_conf_latches_stride(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x11, 0x12, 0x13, 0x14,
    };
    uint8_t commands[160];
    uint8_t data[8];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 0, 0x180));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 2));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 0, 4));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(4, 5, 4, 5, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(5, 4, 5, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_L2_STORE_CONF(1, 1, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(7, 7, 7, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_L2_STORE(3, 2, 0));

    qtest_memwrite(qts, K230_GNNE_SYNTH_GLB_BASE + 0x100,
                   source, sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_GLB_BASE + 0x180, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_GLB_BASE + 0x180, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_identity_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        1, 2, 200, 255,
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0x00, 0x5c,             /* upper 256 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_ADDI(6, 0, 0x440),
        GNNE_ADDI(7, 0, 0x420),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_LUI(10, 0x4),
        GNNE_ADDI(10, 10, -0x400),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEST(5, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 1, 0, 0),
        GNNE_MFU_ACT1_CONF_QUANT(1, 0),
        GNNE_MFU_ACT1_CONF(0, 1, 0),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 0, 7),
    };
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source,
                   sizeof(source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_conf_dest_latches_rlen(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        4, 5, 6, 7,
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0x00, 0x5c,             /* upper 256 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_ADDI(6, 0, 0x440),
        GNNE_ADDI(7, 0, 0x420),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_LUI(10, 0x4),
        GNNE_ADDI(10, 10, -0x400),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEST(5, 0),
        GNNE_LUI(5, 0x5000),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 1, 0, 0),
        GNNE_MFU_ACT1_CONF_QUANT(1, 0),
        GNNE_MFU_ACT1_CONF(0, 1, 0),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 0, 7),
    };
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source,
                   sizeof(source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_add_clip_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0, 1, 254, 255,
    };
    const uint8_t expected[] = {
        1, 2, 255, 255,
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x20, 0x38,             /* negative bias 0.515625 */
        0x20, 0x38,             /* positive bias 0.515625 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_ADDI(6, 0, 0x460),
        GNNE_ADDI(7, 0, 0x420),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_LUI(10, 0x4),
        GNNE_ADDI(10, 10, -0x400),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEST(5, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 1, 0, 0),
        GNNE_MFU_ACT1_CONF_QUANT(1, 0),
        GNNE_MFU_ACT1_CONF(0, 1, 0),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 0, 7),
    };
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source,
                   sizeof(source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_ADD_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_ADD_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(expected) ? expected[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_fp16_roundtrip(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0, 1, 254, 255,
    };
    const uint8_t expected_fp16[] = {
        0x00, 0x00,             /* 0 */
        0x00, 0x3c,             /* 1 */
        0xf0, 0x5b,             /* 254 */
        0xf8, 0x5b,             /* 255 */
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_ADDI(6, 0, 0x480),
        GNNE_ADDI(7, 0, 0x420),
        GNNE_ADDI(8, 0, 0x4a0),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, sizeof(source)),
        GNNE_LUI(10, 0x4),
        GNNE_ADDI(10, 10, -0x400),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEST(5, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 1, 0, 0),
        GNNE_MFU_ACT1_CONF_QUANT(0, 0),
        GNNE_MFU_ACT1_CONF(0, 1, 0),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 0, 7),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 0, 0, 0),
        GNNE_MFU_ACT1_CONF_QUANT(1, 0),
        GNNE_MFU_ACT1_COMPUTE(8, 6, 0, 7),
    };
    uint8_t fp16_data[sizeof(expected_fp16)];
    uint8_t data[8];

    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source,
                   sizeof(source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_FP16_OUTPUT, 0xa5,
                 sizeof(fp16_data));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_ROUNDTRIP_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_FP16_OUTPUT, fp16_data,
                  sizeof(fp16_data));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_ROUNDTRIP_OUTPUT, data,
                  sizeof(data));

    for (size_t i = 0; i < sizeof(expected_fp16); i++) {
        g_assert_cmphex(fp16_data[i], ==, expected_fp16[i]);
    }
    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(source) ? source[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_mul_fp16_two_l2_sources(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source1[] = {
        0x00, 0x40,             /* 2 */
        0x00, 0x42,             /* 3 */
    };
    const uint8_t source2[] = {
        0x00, 0x44,             /* 4 */
        0x00, 0x45,             /* 5 */
    };
    const uint8_t expected[] = {
        0x00, 0x48,             /* 8 */
        0x80, 0x4b,             /* 15 */
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0x00, 0x5c,             /* upper 256 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_ADDI(4, 0, 0x480),
        GNNE_ADDI(6, 0, 0x4a0),
        GNNE_ADDI(7, 0, 0x420),
        GNNE_ADDI(5, 0, 2),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 9, 0),
        GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0),
        GNNE_MFU_ACT1_CONF_SRC1(5, 10, 10, 0, 1),
        GNNE_MFU_ACT1_CONF_SRC1(5, 10, 10, 1, 1),
        GNNE_MFU_ACT1_CONF_SRC2(10, 0, 0, 0),
        GNNE_MFU_ACT1_CONF_SRC2(10, 0, 1, 0),
        GNNE_MFU_ACT1_CONF_DEST(5, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 0, 1, 0),
        GNNE_MFU_ACT1_CONF_QUANT(0, 0),
        GNNE_MFU_ACT1_CONF(1, 0, 0),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 4, 7),
    };
    uint8_t data[sizeof(expected)];

    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source1,
                   sizeof(source1));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_FP16_OUTPUT, source2,
                   sizeof(source2));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_ROUNDTRIP_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_ROUNDTRIP_OUTPUT, data,
                  sizeof(data));

    g_assert_cmpmem(data, sizeof(data), expected, sizeof(expected));

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_raddr_s2_zero_is_unary(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source1[] = {
        0x00, 0x40,             /* 2 */
        0x00, 0x42,             /* 3 */
    };
    const uint8_t zero_source[] = {
        0x00, 0x44,             /* 4 */
        0x00, 0x45,             /* 5 */
    };
    const uint8_t expected[] = {
        0x00, 0x40,             /* 2 */
        0x00, 0x42,             /* 3 */
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0x00, 0x5c,             /* upper 256 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_ADDI(6, 0, 0x4a0),
        GNNE_ADDI(7, 0, 0x420),
        GNNE_ADDI(5, 0, 2),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 9, 0),
        GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0),
        GNNE_MFU_ACT1_CONF_SRC1(5, 10, 10, 0, 1),
        GNNE_MFU_ACT1_CONF_SRC1(5, 10, 10, 1, 1),
        GNNE_MFU_ACT1_CONF_SRC2(10, 0, 0, 0),
        GNNE_MFU_ACT1_CONF_SRC2(10, 0, 1, 0),
        GNNE_MFU_ACT1_CONF_DEST(5, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 0, 1, 0),
        GNNE_MFU_ACT1_CONF_QUANT(0, 0),
        GNNE_MFU_ACT1_CONF(1, 0, 0),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 0, 7),
    };
    uint8_t data[sizeof(expected)];

    qtest_memwrite(qts, K230_GNNE_SYNTH_GLB_BASE, zero_source,
                   sizeof(zero_source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source1,
                   sizeof(source1));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_ROUNDTRIP_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_ROUNDTRIP_OUTPUT, data,
                  sizeof(data));

    g_assert_cmpmem(data, sizeof(data), expected, sizeof(expected));

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_segment_linefit_fp16(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        0x00, 0x00,             /* fp16 0 */
        0x00, 0x3e,             /* fp16 1.5 */
    };
    const uint8_t expected[] = {
        0x00, 0x3c,             /* fp16 1 */
        0x00, 0x45,             /* fp16 5 */
    };
    uint8_t table[98] = {};
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_LUI(6, 1),
        GNNE_ADDI(6, 6, 0x780),
        GNNE_LUI(7, 1),
        GNNE_ADDI(7, 7, 0x700),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 4, 4, 5, 0),
        GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEST(5, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 0, 0, 0),
        GNNE_MFU_ACT1_CONF_QUANT(0, 0),
        GNNE_MFU_ACT1_CONF(0, 0, 1),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 0, 7),
    };
    uint8_t data[8];

    stw_le_p(table + 0, 0x3c00);     /* threshold 1 */
    stw_le_p(table + 2, 0x4000);     /* threshold 2 */
    stw_le_p(table + 30, 0x3c00);    /* segment 0 slope 1 */
    stw_le_p(table + 32, 0x4000);    /* segment 1 slope 2 */
    stw_le_p(table + 62, 0x3c00);    /* segment 0 bias 1 */
    stw_le_p(table + 64, 0x4000);    /* segment 1 bias 2 */
    stw_le_p(table + 94, 0x0000);    /* lower 0 */
    stw_le_p(table + 96, 0x5c00);    /* upper 256 */

    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source,
                   sizeof(source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SEG_ARG, table,
                   sizeof(table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_SEG_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_SEG_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==,
                        i < sizeof(expected) ? expected[i] : 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_packed_strides_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        11, 12, 0xee,
        13, 14, 0xee,
        15, 16, 0xee,
        17, 18, 0xee,
    };
    const uint8_t expected[] = {
        11, 12, 0xa5,
        13, 14, 0xa5,
        15, 16, 0xa5,
        17, 18, 0xa5,
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0x00, 0x5c,             /* upper 256 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x400),
        GNNE_ADDI(6, 0, 0x440),
        GNNE_ADDI(7, 0, 0x420),
        GNNE_ADDI(4, 0, 1),
        GNNE_ADDI(5, 0, 2),
        GNNE_ADDI(8, 0, 4),
        GNNE_ADDI(9, 0, 3),
        GNNE_LUI(10, 0x4),
        GNNE_ADDI(10, 10, -0x400),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(4, 5, 5, 5, 0),
        GNNE_SS_PACK_STRIDE(8, 5, 9, 1),
        GNNE_MFU_ACT1_CONF_STRIDE(1, 0, 1),
        GNNE_MFU_ACT1_CONF_SRC2(0, 0, 0, 0),
        GNNE_MFU_ACT1_CONF_DEST(0, 0),
        GNNE_MFU_ACT1_CONF_DEQ(10, 0, 1, 0, 0),
        GNNE_MFU_ACT1_CONF_QUANT(1, 0),
        GNNE_MFU_ACT1_CONF(0, 0, 0),
        GNNE_MFU_ACT1_COMPUTE(6, 3, 0, 7),
    };
    uint8_t data[sizeof(expected)];

    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, source,
                   sizeof(source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_OUTPUT, data, sizeof(data));

    g_assert_cmpmem(data, sizeof(data), expected, sizeof(expected));

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_pdp1_average_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        2, 4, 0xee, 0xee,
        6, 8, 0xee, 0xee,
        10, 20, 0xdd, 0xdd,
        30, 40, 0xdd, 0xdd,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_LUI(3, 1),
        GNNE_ADDI(3, 3, 0x800),
        GNNE_LUI(4, 1),
        GNNE_ADDI(4, 4, 0x840),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 16),
        GNNE_ADDI(11, 0, 4),
        GNNE_LUI(12, 0x4),
        GNNE_ADDI(12, 12, -0x400),
        GNNE_ADDI(13, 0, 0),
        GNNE_ADDI(14, 0, 2),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(10, 9, 11, 1),
        GNNE_SS_PACK_STRIDE(9, 8, 8, 2),
        GNNE_MFU_PDP1_CONF1(1, 1, 1, 2, 2),
        GNNE_MFU_PDP1_CONF2(0, 0, 0, 0),
        GNNE_MFU_PDP1_CONF3(0, 0, 0, 0),
        GNNE_MFU_PDP1_CONF4(14, 14, 12, 0, 0),
        GNNE_MFU_PDP1_CONF_DEQ(12, 13, 1, 13),
        GNNE_MFU_PDP1_CONF_QUANT(12, 13, 1, 13),
        GNNE_MFU_PDP1_COMPUTE(4, 3, 0),
    };
    uint8_t data[4];

    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP1_INPUT, source,
                   sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 5);
    g_assert_cmphex(data[1], ==, 25);
    for (size_t i = 2; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_pdp1_min_fp16_sum_i16(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        2, 4, 0xee, 0xee,
        6, 8, 0xee, 0xee,
        10, 20, 0xdd, 0xdd,
        30, 40, 0xdd, 0xdd,
    };
    const uint32_t min_commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_LUI(3, 1),
        GNNE_ADDI(3, 3, 0x800),
        GNNE_LUI(4, 1),
        GNNE_ADDI(4, 4, 0x840),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 16),
        GNNE_ADDI(11, 0, 4),
        GNNE_LUI(12, 0x4),
        GNNE_ADDI(12, 12, -0x400),
        GNNE_ADDI(13, 0, 0),
        GNNE_ADDI(14, 0, 2),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(10, 9, 11, 1),
        GNNE_SS_PACK_STRIDE(9, 8, 8, 2),
        GNNE_MFU_PDP1_CONF1(1, 1, 1, 1, 2),
        GNNE_MFU_PDP1_CONF2(0, 0, 0, 0),
        GNNE_MFU_PDP1_CONF3(0, 0, 0, 0),
        GNNE_MFU_PDP1_CONF4(14, 14, 12, 0, 0),
        GNNE_MFU_PDP1_CONF_DEQ(12, 13, 1, 13),
        GNNE_MFU_PDP1_CONF_QUANT(12, 13, 0, 13),
        GNNE_MFU_PDP1_COMPUTE(4, 3, 0),
    };
    const uint32_t sum_commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_LUI(3, 1),
        GNNE_ADDI(3, 3, 0x800),
        GNNE_LUI(4, 1),
        GNNE_ADDI(4, 4, 0x840),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 16),
        GNNE_ADDI(11, 0, 4),
        GNNE_LUI(12, 0x4),
        GNNE_ADDI(12, 12, -0x400),
        GNNE_ADDI(13, 0, 0),
        GNNE_ADDI(14, 0, 2),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(10, 9, 11, 1),
        GNNE_SS_PACK_STRIDE(9, 8, 8, 2),
        GNNE_MFU_PDP1_CONF1(1, 1, 1, 3, 2),
        GNNE_MFU_PDP1_CONF2(0, 0, 0, 0),
        GNNE_MFU_PDP1_CONF3(0, 0, 0, 0),
        GNNE_MFU_PDP1_CONF4(14, 14, 12, 0, 0),
        GNNE_MFU_PDP1_CONF_DEQ(12, 13, 1, 13),
        GNNE_MFU_PDP1_CONF_QUANT(12, 13, 3, 13),
        GNNE_MFU_PDP1_COMPUTE(4, 3, 0),
    };
    const uint8_t expected_min[] = {
        0x00, 0x40,
        0x00, 0x49,
    };
    const uint8_t expected_sum[] = {
        20, 0,
        100, 0,
    };
    uint8_t data[4];

    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP1_INPUT, source,
                   sizeof(source));

    qtest_memset(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, 0xa5, sizeof(data));
    k230_kpu_run_commands(qts, min_commands, G_N_ELEMENTS(min_commands));
    qtest_memread(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, data, sizeof(data));
    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, expected_min[i]);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_memset(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, 0xa5, sizeof(data));
    k230_kpu_run_commands(qts, sum_commands, G_N_ELEMENTS(sum_commands));
    qtest_memread(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, data, sizeof(data));
    for (size_t i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, expected_sum[i]);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_pdp1_sliding_min_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t source[] = {
        1, 2, 3,
        4, 5, 6,
        7, 8, 9,
    };
    const uint8_t expected[] = {
        1, 2,
        4, 5,
        0xa5, 0xa5, 0xa5, 0xa5,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_LUI(3, 1),
        GNNE_ADDI(3, 3, 0x800),
        GNNE_LUI(4, 1),
        GNNE_ADDI(4, 4, 0x840),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 3),
        GNNE_ADDI(10, 0, 3),
        GNNE_ADDI(11, 0, 2),
        GNNE_LUI(12, 0x4),
        GNNE_ADDI(12, 12, -0x400),
        GNNE_ADDI(13, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 10, 1),
        GNNE_SS_PACK_STRIDE(8, 8, 11, 2),
        GNNE_MFU_PDP1_CONF1(1, 1, 1, 1, 2),
        GNNE_MFU_PDP1_CONF2(11, 11, 0, 0),
        GNNE_MFU_PDP1_CONF3(0, 0, 0, 0),
        GNNE_MFU_PDP1_CONF4(11, 11, 12, 0, 0),
        GNNE_MFU_PDP1_CONF_DEQ(12, 13, 1, 13),
        GNNE_MFU_PDP1_CONF_QUANT(12, 13, 1, 13),
        GNNE_MFU_PDP1_COMPUTE(4, 3, 0),
    };
    uint8_t data[sizeof(expected)];

    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP1_INPUT, source,
                   sizeof(source));
    qtest_memset(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_PDP1_OUTPUT, data, sizeof(data));

    g_assert_cmpmem(data, sizeof(data), expected, sizeof(expected));

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_conv2d_act0_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 2,
        3, 4,
    };
    const uint8_t weight_zp[] = {
        3,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 4),
        GNNE_ADDI(11, 0, 2),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(10, 10, 9, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 2, 2, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1),
        GNNE_PU_W_CONF(0, 0, 2, 2),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[80] = {};
    uint8_t data[4];

    weights[0] = 4;
    weights[24] = 5;
    weights[48] = 6;
    weights[72] = 7;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 10);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_dm_store_of_act0_dest(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 2,
        3, 4,
    };
    const uint8_t weight_zp[] = {
        3,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 4),
        GNNE_ADDI(11, 0, 2),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(10, 10, 9, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 2, 2, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1),
        GNNE_PU_W_CONF(0, 0, 2, 2),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_DM_STORE_OF(0, 0, 7, 1, 0),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(0, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[80] = {};
    uint8_t data[4];

    weights[0] = 4;
    weights[24] = 5;
    weights[48] = 6;
    weights[72] = 7;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 10);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_conv2d_i8_input_act0_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        0xfd, 4,
    };
    const uint8_t weight_zp[] = {
        0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_ADDI(11, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 8, 8, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(9, 8, 8, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 9, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 9, 11, 2),
        GNNE_PU_W_CONF(0, 0, 1, 1),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[24] = {};
    uint8_t data[4];

    weights[0] = 2;
    weights[1] = 3;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 6);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_conv2d_packed_if_stride(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 2,
        99, 4,
        5, 6,
        7, 8,
    };
    const uint8_t weight_zp[] = {
        0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_ADDI(11, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 9, 9, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(8, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 9, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 9, 11, 1),
        GNNE_PU_W_CONF(0, 0, 1, 1),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[24] = {};
    uint8_t data[4];

    weights[0] = 1;
    weights[1] = 2;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 11);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_conv2d_packed_of_stride(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 2,
        3, 4,
    };
    const uint8_t weight_zp[] = {
        0, 0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_ADDI(11, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 9, 9, 1),
        GNNE_SS_PACK_STRIDE(8, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(9, 9, 9, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 9),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1),
        GNNE_PU_W_CONF(0, 0, 1, 1),
        GNNE_PU_OF_CONF1(0, 0, 9, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 0),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[48] = {};
    uint8_t data[10];

    weights[0] = 1;
    weights[24] = 10;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 1);
    g_assert_cmphex(data[1], ==, 2);
    g_assert_cmphex(data[2], ==, 3);
    g_assert_cmphex(data[3], ==, 4);
    g_assert_cmphex(data[4], ==, 10);
    g_assert_cmphex(data[5], ==, 20);
    g_assert_cmphex(data[6], ==, 30);
    g_assert_cmphex(data[7], ==, 40);
    for (size_t i = 8; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_of_conf_latches_shape_stride(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 2,
        3, 4,
    };
    const uint8_t weight_zp[] = {
        0, 0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_ADDI(11, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 9, 9, 1),
        GNNE_SS_PACK_STRIDE(8, 9, 9, 0),
        GNNE_SS_PACK_STRIDE(9, 9, 9, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 9),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1),
        GNNE_PU_W_CONF(0, 0, 1, 1),
        GNNE_PU_OF_CONF1(0, 0, 9, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_SS_PACK_SHAPE(10, 10, 10, 10, 1),
        GNNE_SS_PACK_STRIDE(10, 10, 10, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 0),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[48] = {};
    uint8_t data[10];

    weights[0] = 1;
    weights[24] = 10;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 1);
    g_assert_cmphex(data[1], ==, 2);
    g_assert_cmphex(data[2], ==, 3);
    g_assert_cmphex(data[3], ==, 4);
    g_assert_cmphex(data[4], ==, 10);
    g_assert_cmphex(data[5], ==, 20);
    g_assert_cmphex(data[6], ==, 30);
    g_assert_cmphex(data[7], ==, 40);
    for (size_t i = 8; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_psum_accumulates_to_act0(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        7,
    };
    const uint8_t weight0[24] = {
        9,
    };
    const uint8_t weight1[24] = {
        3,
    };
    const uint8_t weight_zp[] = {
        3,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    uint8_t commands[256];
    uint8_t data[4];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 0, 0x700));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 0x720));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 0x760));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 0, 0x770));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 0x790));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(11, 0, 2));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(8, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(8, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_W_CONF(0, 0, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF1(0, 0, 8, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF2(0, 0, 7, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 0, 1, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 0x740));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 1, 0, 1, 1, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));

    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_WEIGHT0, weight0,
                   sizeof(weight0));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_WEIGHT1, weight1,
                   sizeof(weight1));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_PSUM_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_PSUM_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 30);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_uses_fetchif_stride_without_l1(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        5, 7,
    };
    const uint8_t stale_input[] = {
        1, 1,
    };
    const uint8_t weight_zp[] = {
        1,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_ADDI(11, 0, 0),
        GNNE_ADDI(12, 0, 0x124),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 9, 1),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 2),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 3),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 4),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 2, 0),
        GNNE_DM_LOAD_L1(0, 0, 12, 0, 1, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 2, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 3),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 1),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1),
        GNNE_PU_W_CONF(0, 0, 1, 2),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 4),
        GNNE_PU_OF_CONF2(0, 0, 7, 2),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 2, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[48] = {};
    uint8_t data[4];

    weights[0] = 3;
    weights[24] = 4;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT + 0x24,
                   stale_input, sizeof(stale_input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 31);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_zero_fetchif_uses_l1_source(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        5, 7,
    };
    const uint8_t stale_input[] = {
        1, 1,
    };
    const uint8_t weight_zp[] = {
        1,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0),
        GNNE_ADDI(11, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 9, 2),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 2, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 2, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 0, 8, 2),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1),
        GNNE_PU_W_CONF(0, 0, 1, 2),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[48] = {};
    uint8_t data[4];

    weights[0] = 3;
    weights[24] = 4;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_GLB_BASE,
                   stale_input, sizeof(stale_input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 31);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_fetchif_offset_uses_if_staging(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[5] = {
        5, 99, 88, 77, 7,
    };
    const uint8_t global_input[] = {
        1, 1,
    };
    const uint8_t weight_zp[] = {
        0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 4),
        GNNE_ADDI(11, 0, 0),
        GNNE_ADDI(12, 0, 1),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 9, 8, 8, 2),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(8, 10, 8, 3),
        GNNE_DM_LOAD_L1_CONF(0, 0, 3, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 2, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 12, 8, 2),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1),
        GNNE_PU_W_CONF(0, 0, 1, 1),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[24] = {};
    uint8_t data[4];

    weights[0] = 3;
    weights[1] = 4;
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_GLB_BASE + 1,
                   global_input, sizeof(global_input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 21);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_l1_bank_source_uses_raw_offset(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t raw_input[] = {
        5, 7,
    };
    const uint8_t translated_input[] = {
        1, 1,
    };
    const uint8_t weight_zp[] = {
        1,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_LUI(3, 0x10000),
        GNNE_ADDI(3, 3, 0x500),
        GNNE_ADDI(4, 0, 0x520),
        GNNE_ADDI(5, 0, 0x590),
        GNNE_ADDI(6, 0, 0x5a0),
        GNNE_ADDI(7, 0, 0x5c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 0x10),
        GNNE_ADDI(11, 0, 2),
        GNNE_ADDI(15, 0, 0),
        GNNE_LUI(12, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_LUI(13, 0x10000),
        GNNE_ADDI(13, 13, 0x700),
        GNNE_LUI(14, K230_GNNE_SYNTH_SOURCE >> 12),
        GNNE_ADDI(14, 14, 0x100),
        GNNE_MMU_CONF(0, 11, 0),
        GNNE_MMU_CONF(0, 11, 1),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 9, 2),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 0),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 1),
        GNNE_L2_LOAD_CONF(1, 0, 0, 0),
        GNNE_L2_LOAD(3, 12, 2),
        GNNE_L2_LOAD(13, 14, 2),
        GNNE_MMU_CONF(10, 11, 1),
        GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0),
        GNNE_DM_LOAD_L1(0, 0, 3, 0, 2, 0),
        GNNE_DM_LOAD_W_CONF(0, 0, 1, 2, 0),
        GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1),
        GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1),
        GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0),
        GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PU_FETCHIF_CONF3(0, 0, 0, 8, 2),
        GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 15, 1),
        GNNE_PU_W_CONF(0, 0, 1, 2),
        GNNE_PU_OF_CONF1(0, 0, 8, 0, 1),
        GNNE_PU_OF_CONF2(0, 0, 7, 1),
        GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 1, 0),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1),
        GNNE_PU_COMPUTE(0, 0),
    };
    uint8_t weights[48] = {};
    uint8_t data[4];

    weights[0] = 3;
    weights[24] = 4;
    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE, raw_input,
                   sizeof(raw_input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_SOURCE + 0x100,
                   translated_input, sizeof(translated_input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 31);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_l1_i16_repacks_byte_planes(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        5, 9,                   /* channel 0: low byte, high byte */
        7, 11,                  /* channel 1: low byte, high byte */
    };
    const uint8_t weight_zp[] = {
        0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    uint8_t commands[512];
    uint8_t weights[24] = {};
    uint8_t data[4];
    size_t command_size = 0;

    weights[0] = 2;
    weights[1] = 3;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 0, 0x500));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 0x520));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 0x590));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 0, 0x5a0));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 0x5c0));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(9, 0, 2));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(12, 0, 0x200));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(13, 7, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(15, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 9, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 9, 8, 8, 2));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(9, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(8, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1_CONF(0, 0, 0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1(0, 0, 3, 0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 0, 8, 2));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 15, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_W_CONF(0, 0, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF1(0, 0, 8, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF2(0, 0, 7, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 0, 0, 1, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 12, 8, 2));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 15, 2));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_COMPUTE(13, 0, 0, 0, 0, 1));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));

    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_CONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 31);
    g_assert_cmphex(data[1], ==, 51);
    for (size_t i = 2; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_act1_adds_psum_and_l2_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        5,
    };
    const uint8_t weight[24] = {
        4,
    };
    const uint8_t weight_zp[] = {
        1,
    };
    const uint8_t l2_source[] = {
        2,
    };
    const uint8_t act1_table[] = {
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0x00, 0x5c,             /* upper 256 */
    };
    uint8_t commands[512];
    uint8_t data[4];
    size_t command_size = 0;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 0, 0x700));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 0, 0x720));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 0, 0x760));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 0, 0x420));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 0, 0x440));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(9, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(10, 0, 0x790));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(11, 0, 0x400));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(12, 4));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(12, 12, -0x400));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(8, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 9, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_W_CONF(0, 0, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF1(0, 0, 8, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF2(0, 0, 10, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 0, 1, 0, 1, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_STRIDE(0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_SRC1(0, 0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_SRC1(0, 0, 0, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_SRC2(0, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_SRC2(0, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_DEST(8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_DEQ(12, 9, 0, 0, 9));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_DEQ(12, 9, 1, 1, 9));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF_QUANT(1, 9));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_CONF(0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_MFU_ACT1_COMPUTE(7, 10, 11, 6));

    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_WEIGHT0, weight,
                   sizeof(weight));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PSUM_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_SOURCE, l2_source,
                   sizeof(l2_source));
    qtest_memwrite(qts, K230_GNNE_SYNTH_MFU_ARG, act1_table,
                   sizeof(act1_table));
    qtest_memset(qts, K230_GNNE_SYNTH_MFU_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_MFU_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 17);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void k230_run_pu_compute_psum_classifier_stride(unsigned int channels)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1,
    };
    const uint8_t act0_entry[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    uint8_t weight0[32 * K230_GNNE_SYNTH_LANE_WIDTH] = {};
    uint8_t weight1[32 * K230_GNNE_SYNTH_LANE_WIDTH] = {};
    uint8_t weight_zp[32] = {};
    uint8_t act0_table[32 * sizeof(act0_entry)];
    uint8_t commands[256];
    uint8_t data[72];
    size_t command_size = 0;

    g_assert_cmpuint(channels, >, 0);
    g_assert_cmpuint(channels, <=, 32);

    for (size_t i = 0; i < 32; i++) {
        weight0[i * K230_GNNE_SYNTH_LANE_WIDTH] = i + 1;
        weight1[i * K230_GNNE_SYNTH_LANE_WIDTH] = 2 * (i + 1);
        memcpy(act0_table + i * sizeof(act0_entry), act0_entry,
               sizeof(act0_entry));
    }

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 3, 0x900));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(4, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 4, 0x940));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(5, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 5, 0xf40));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(6, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 6, 0xf80));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(7, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 7, 0x180));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ADDI(9, 0, channels));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(11, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 9, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(8, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(9, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1_CONF(0, 0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1(0, 0, 3, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF2(0, 0, 8, 9));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 8, 11, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_W_CONF(0, 0, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF1(0, 0, 9, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF2(0, 0, 7, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_COMPUTE(7, 0, 0, 0, 2, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 0, 1, 0, 1, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(4, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 4, 0xc40));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 1, 0, 1, 1, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));

    qtest_memwrite(qts, K230_GNNE_SYNTH_CLASS_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CLASS_WEIGHT0, weight0,
                   sizeof(weight0));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CLASS_WEIGHT1, weight1,
                   sizeof(weight1));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CLASS_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_CLASS_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_CLASS_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_CLASS_OUTPUT, data, sizeof(data));

    for (size_t i = 0; i < channels; i++) {
        uint8_t expected[2];

        stw_le_p(expected, k230_test_fp16_from_uint(3 * (i + 1)));
        g_assert_cmphex(data[i * 2], ==, expected[0]);
        g_assert_cmphex(data[i * 2 + 1], ==, expected[1]);
    }
    for (size_t i = channels * 2; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_psum_classifier_stride(void)
{
    k230_run_pu_compute_psum_classifier_stride(32);
    k230_run_pu_compute_psum_classifier_stride(9);
}

static void test_pu_compute_deconv_1x1_psum_to_act0(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input_u8[] = {
        5, 6,
    };
    const uint8_t input_i8[] = {
        0xff, 2,
    };
    const uint8_t zero_input[] = {
        0,
    };
    uint8_t weight[K230_GNNE_SYNTH_LANE_WIDTH] = {};
    uint8_t zero_weight[K230_GNNE_SYNTH_LANE_WIDTH] = {};
    const uint8_t weight_zp[] = {
        0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    uint8_t commands[512];
    size_t command_size = 0;
    uint8_t data[2];

    weight[0] = 2;
    weight[1] = 3;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 3, 0x200));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(4, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 4, 0x220));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(5, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 5, 0x260));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(6, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 6, 0x2e0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(7, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 7, 0x300));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(8, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 8, 0x320));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(9, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(10, 0, 2));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(13, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(9, 10, 9, 9, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(9, 9, 9, 9, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(10, 9, 9, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(9, 9, 9, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 5, 6, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 2, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 10, 13, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_W_CONF(0, 0, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF1(0, 0, 8, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF2(0, 0, 13, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 0, 1, 0, 0, 1));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 4, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 10, 13, 2));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 1, 0, 0, 0, 1));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));

    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(11, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(11, 11, 0x240));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(12, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(12, 12, 0x2a0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1_CONF(0, 0, 1, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1(0, 0, 11, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 12, 6, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_ACT0(0, 0, 7, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 11, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 9, 13, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF1(0, 0, 8, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF2(0, 0, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_COMPUTE(8, 0, 0, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 1, 0, 1, 1, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));

    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_INPUT_U8, input_u8,
                   sizeof(input_u8));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_INPUT_I8, input_i8,
                   sizeof(input_i8));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_ZERO_INPUT, zero_input,
                   sizeof(zero_input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_WEIGHT, weight,
                   sizeof(weight));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_ZERO_WEIGHT, zero_weight,
                   sizeof(zero_weight));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_DECONV_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_DECONV_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 32);
    g_assert_cmphex(data[1], ==, 0xa5);

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pu_compute_deconv_1x1_multi_channel(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        4, 5,
    };
    const uint8_t zero_input[] = {
        0, 0,
    };
    uint8_t weights[2 * K230_GNNE_SYNTH_LANE_WIDTH] = {};
    uint8_t zero_weight[2 * K230_GNNE_SYNTH_LANE_WIDTH] = {};
    const uint8_t weight_zp[] = {
        0, 0,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    uint8_t commands[512];
    size_t command_size = 0;
    uint8_t data[2];

    weights[0] = 2;
    weights[1] = 3;
    weights[K230_GNNE_SYNTH_LANE_WIDTH] = 5;
    weights[K230_GNNE_SYNTH_LANE_WIDTH + 1] = 7;

    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(2, 0, 0x100));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(3, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(3, 3, 0x340));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(4, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(4, 4, 0x360));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(5, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(5, 5, 0x3c0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(6, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(6, 6, 0x3e0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(7, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(7, 7, 0x400));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(8, 0, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(9, 0, 2));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(10, 0, 0));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(11, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(11, 11, 0x240));
    k230_kpu_command_u32(commands, &command_size, GNNE_LUI(12, 1));
    k230_kpu_command_u32(commands, &command_size, GNNE_ADDI(12, 12, 0x2a0));
    k230_kpu_command_u32(commands, &command_size, GNNE_MMU_CONF(0, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 9, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_SHAPE(8, 9, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(9, 8, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_SS_PACK_STRIDE(9, 8, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF(0, 0, 1, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF_DEQ(0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W_CONF2(0, 0, 8, 8));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 2, 2, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF2(0, 0, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 3, 8, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF4(0, 0, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF_DEQ(0, 0, 9, 10, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_W_CONF(0, 0, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF1(0, 0, 8, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_OF_CONF2(0, 0, 10, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 0, 1, 0, 0, 1));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_ACT0_COMPUTE(7, 0, 0, 0, 0, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1_CONF(0, 0, 1, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_L1(0, 0, 11, 0, 1, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_DM_LOAD_W(0, 0, 12, 5, 0, 0));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF1(0, 0, 1, 1, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_FETCHIF_CONF3(0, 0, 11, 8, 1));
    k230_kpu_command_u32(commands, &command_size,
                         GNNE_PU_COMPUTE_CONF(0, 0, 1, 0, 1, 1, 0));
    k230_kpu_command_u16(commands, &command_size, GNNE_PU_COMPUTE(0, 0));

    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_MULTI_INPUT, input,
                   sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_ZERO_INPUT, zero_input,
                   sizeof(zero_input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_MULTI_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_ZERO_WEIGHT, zero_weight,
                   sizeof(zero_weight));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_MULTI_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_DECONV_MULTI_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_DECONV_MULTI_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_command_bytes(qts, commands, command_size);
    qtest_memread(qts, K230_GNNE_SYNTH_DECONV_MULTI_OUTPUT, data,
                  sizeof(data));

    g_assert_cmphex(data[0], ==, 23);
    g_assert_cmphex(data[1], ==, 55);

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_mfu_transpose_nwch_i16(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        0x01, 0x10, 0x02, 0x10, 0x03, 0x10,
        0x04, 0x10, 0x05, 0x10, 0x06, 0x10,
    };
    const uint8_t expected[] = {
        0x01, 0x10, 0x04, 0x10,
        0x02, 0x10, 0x05, 0x10,
        0x03, 0x10, 0x06, 0x10,
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_LUI(3, 1),
        GNNE_ADDI(3, 3, 0x500),
        GNNE_LUI(4, 1),
        GNNE_ADDI(4, 4, 0x600),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 3),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 10, 0),
        GNNE_SS_PACK_STRIDE(8, 9, 10, 0),
        GNNE_SS_PACK_STRIDE(10, 8, 9, 1),
        GNNE_MFU_TRANSPOSE_CONF(1, 0, 2, 4),
        GNNE_MFU_TRANSPOSE(4, 3, 0),
    };
    uint8_t data[sizeof(expected)];

    qtest_memwrite(qts, K230_GNNE_SYNTH_TRANSPOSE_INPUT, input,
                   sizeof(input));
    qtest_memset(qts, K230_GNNE_SYNTH_TRANSPOSE_OUTPUT, 0xa5,
                 sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_TRANSPOSE_OUTPUT, data,
                  sizeof(data));

    g_assert_cmpmem(data, sizeof(data), expected, sizeof(expected));

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pdp0_compute_depthwise_act0_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 2,
        3, 4,
    };
    const uint8_t weights[] = {
        4, 5,
        6, 7,
    };
    const uint8_t weight_zp[] = {
        3,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x600),
        GNNE_ADDI(4, 0, 0x620),
        GNNE_ADDI(5, 0, 0x680),
        GNNE_ADDI(6, 0, 0x690),
        GNNE_ADDI(7, 0, 0x6c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 2),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 2),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 1),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 1, 1),
        GNNE_DM_STORE_OF(0, 0, 7, 1, 1),
        GNNE_PDP0_MODE_CONF(0, 0, 0),
        GNNE_PDP0_FETCHIF_CONF1(0, 0, 1, 1),
        GNNE_PDP0_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PDP0_FETCHIF_CONF3(0, 0, 0),
        GNNE_PDP0_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PDP0_FETCHIF_CONF_DEQ(0, 0, 10, 1),
        GNNE_PDP0_W_CONF(0, 0, 2, 2),
        GNNE_PDP0_OF_CONF(0, 0, 2, 1),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(0, 0, 0, 1, 0, 1),
        GNNE_PDP0_COMPUTE(0, 3),
    };
    uint8_t data[4];

    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_PDP0_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_PDP0_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 10);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pdp0_conf_latches_shape_stride(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 2,
        3, 4,
    };
    const uint8_t weights[] = {
        4, 5,
        6, 7,
    };
    const uint8_t weight_zp[] = {
        3,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x600),
        GNNE_ADDI(4, 0, 0x620),
        GNNE_ADDI(5, 0, 0x680),
        GNNE_ADDI(6, 0, 0x690),
        GNNE_ADDI(7, 0, 0x6c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 2),
        GNNE_ADDI(11, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 8, 8, 1),
        GNNE_SS_PACK_STRIDE(8, 8, 8, 2),
        GNNE_DM_LOAD_W(0, 0, 4, 5, 0, 1),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 1, 1),
        GNNE_DM_STORE_OF(0, 0, 7, 1, 1),
        GNNE_PDP0_MODE_CONF(0, 0, 0),
        GNNE_PDP0_FETCHIF_CONF1(0, 0, 1, 1),
        GNNE_PDP0_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PDP0_FETCHIF_CONF3(0, 0, 0),
        GNNE_PDP0_FETCHIF_CONF4(0, 0, 0, 0),
        GNNE_PDP0_FETCHIF_CONF_DEQ(0, 0, 10, 1),
        GNNE_PDP0_W_CONF(0, 0, 2, 2),
        GNNE_PDP0_OF_CONF(0, 0, 2, 1),
        GNNE_SS_PACK_SHAPE(11, 11, 11, 11, 0),
        GNNE_SS_PACK_SHAPE(11, 11, 11, 11, 1),
        GNNE_SS_PACK_STRIDE(11, 11, 11, 2),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(0, 0, 0, 1, 0, 1),
        GNNE_PDP0_COMPUTE(0, 3),
    };
    uint8_t data[4];

    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_WEIGHT, weights,
                   sizeof(weights));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_WEIGHT_ZP, weight_zp,
                   sizeof(weight_zp));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_PDP0_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_PDP0_OUTPUT, data, sizeof(data));

    g_assert_cmphex(data[0], ==, 10);
    for (size_t i = 1; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0xa5);
    }

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

static void test_pdp0_compute_max_act0_u8(void)
{
    QTestState *qts = k230_kpu_init();
    const uint8_t input[] = {
        1, 9, 2,
        4, 3, 8,
        7, 6, 5,
    };
    const uint8_t expected[] = {
        9, 9,
        7, 8,
    };
    const uint8_t act0_table[] = {
        0x00, 0x3c,             /* negative slope 1 */
        0x00, 0x3c,             /* positive slope 1 */
        0x00, 0x00,             /* negative bias 0 */
        0x00, 0x00,             /* positive bias 0 */
        0x00, 0x00,             /* lower 0 */
        0xf8, 0x5b,             /* upper 255 */
        0x00, 0x00,             /* threshold 0 */
    };
    const uint32_t commands[] = {
        GNNE_ADDI(2, 0, 0x100),
        GNNE_ADDI(3, 0, 0x600),
        GNNE_ADDI(6, 0, 0x690),
        GNNE_ADDI(7, 0, 0x6c0),
        GNNE_ADDI(8, 0, 1),
        GNNE_ADDI(9, 0, 2),
        GNNE_ADDI(10, 0, 3),
        GNNE_ADDI(11, 0, 0),
        GNNE_MMU_CONF(0, 2, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 10, 10, 0),
        GNNE_SS_PACK_SHAPE(8, 8, 9, 9, 1),
        GNNE_SS_PACK_STRIDE(8, 8, 9, 2),
        GNNE_DM_LOAD_ACT0(0, 0, 6, 0, 1, 1),
        GNNE_DM_STORE_OF(0, 0, 7, 1, 1),
        GNNE_PDP0_MODE_CONF(0, 0, 2),
        GNNE_PDP0_FETCHIF_CONF1(0, 0, 1, 1),
        GNNE_PDP0_FETCHIF_CONF2(0, 0, 8, 0),
        GNNE_PDP0_FETCHIF_CONF3(0, 0, 0),
        GNNE_PDP0_FETCHIF_CONF4(0, 0, 11, 0),
        GNNE_PDP0_FETCHIF_CONF_DEQ(0, 0, 11, 1),
        GNNE_PDP0_W_CONF(0, 0, 2, 2),
        GNNE_PDP0_OF_CONF(0, 0, 2, 1),
        GNNE_ACT0_SRC1_CONF(0, 0, 0, 1, 0),
        GNNE_ACT0_COMPUTE(0, 0, 0, 1, 0, 1),
        GNNE_PDP0_COMPUTE(0, 3),
    };
    uint8_t data[sizeof(expected)];

    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_INPUT, input, sizeof(input));
    qtest_memwrite(qts, K230_GNNE_SYNTH_PDP0_ACT0, act0_table,
                   sizeof(act0_table));
    qtest_memset(qts, K230_GNNE_SYNTH_PDP0_OUTPUT, 0xa5, sizeof(data));

    k230_kpu_run_commands(qts, commands, G_N_ELEMENTS(commands));
    qtest_memread(qts, K230_GNNE_SYNTH_PDP0_OUTPUT, data, sizeof(data));

    g_assert_cmpmem(data, sizeof(data), expected, sizeof(expected));

    k230_kpu_assert_done_irq(qts);
    k230_kpu_clear_done_irq(qts);

    qtest_quit(qts);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/k230-kpu/zero-start-does-not-complete",
                   test_zero_start_does_not_complete);
    qtest_add_func("/k230-kpu/delayed-completion",
                   test_delayed_completion);
    qtest_add_func("/k230-kpu/command-completion-pages",
                   test_command_completion_pages);
    qtest_add_func("/k230-kpu/same-page-addresses-zero-one-page",
                   test_same_page_addresses_zero_one_page);
    qtest_add_func("/k230-kpu/output-window-boundaries",
                   test_output_window_boundaries);
    qtest_add_func("/k230-kpu/empty-command-range",
                   test_empty_command_range);
    qtest_add_func("/k230-kpu/reversed-command-range",
                   test_reversed_command_range);
    qtest_add_func("/k230-kpu/clear-status-allows-second-run",
                   test_clear_status_allows_second_run);
    qtest_add_func("/k230-kpu/l2-store-copies-parsed-output",
                   test_l2_store_copies_parsed_output);
    qtest_add_func("/k230-kpu/l2-store-converts-fp16-to-fp32",
                   test_l2_store_converts_fp16_to_fp32);
    qtest_add_func("/k230-kpu/l2-load-copies-to-glb",
                   test_l2_load_copies_to_glb);
    qtest_add_func("/k230-kpu/l2-load-uses-packed-strides",
                   test_l2_load_uses_packed_strides);
    qtest_add_func("/k230-kpu/l2-store-uses-packed-strides",
                   test_l2_store_uses_packed_strides);
    qtest_add_func("/k230-kpu/l2-store-reads-high-mmu0-glb",
                   test_l2_store_reads_high_mmu0_glb);
    qtest_add_func("/k230-kpu/l2-nonzero-bank-raw-offset",
                   test_l2_nonzero_bank_uses_raw_offset);
    qtest_add_func("/k230-kpu/l2-bank-write-updates-logical-glb",
                   test_l2_bank_write_updates_logical_glb);
    qtest_add_func("/k230-kpu/l2-load-converts-fp32-to-fp16",
                   test_l2_load_converts_fp32_to_fp16);
    qtest_add_func("/k230-kpu/l2-load-then-store-roundtrip",
                   test_l2_load_then_store_roundtrip);
    qtest_add_func("/k230-kpu/ai2d-compute-preserves-command-stream",
                   test_ai2d_compute_preserves_command_stream);
    qtest_add_func("/k230-kpu/l2-load-w-uses-lane-layout",
                   test_l2_load_w_uses_lane_layout);
    qtest_add_func("/k230-kpu/l2-load-w-conf-latches-rlen",
                   test_l2_load_w_conf_latches_rlen);
    qtest_add_func("/k230-kpu/l2-load-w-translates-low-source",
                   test_l2_load_w_translates_low_source);
    qtest_add_func("/k230-kpu/l2-load-w-rebases-function-source",
                   test_l2_load_w_rebases_function_source);
    qtest_add_func("/k230-kpu/l2-load-w-rebases-absolute-rdata-source",
                   test_l2_load_w_rebases_absolute_rdata_source);
    qtest_add_func("/k230-kpu/l2-load-w-keeps-absolute-rdata-source",
                   test_l2_load_w_keeps_absolute_rdata_source);
    qtest_add_func("/k230-kpu/l2-load-w-synthesizes-function-arg",
                   test_l2_load_w_synthesizes_function_arg);
    qtest_add_func("/k230-kpu/l2-load-w-synthesizes-rdata-function-args",
                   test_l2_load_w_synthesizes_rdata_function_args);
    qtest_add_func("/k230-kpu/l2-load-w-rdata-fallback-base",
                   test_l2_load_w_uses_rdata_fallback_base);
    qtest_add_func("/k230-kpu/runtime-function-command-base",
                   test_runtime_function_command_uses_runtime_base);
    qtest_add_func("/k230-kpu/runtime-rdata-shadow-survives-glb-mutation",
                   test_runtime_rdata_shadow_survives_glb_mutation);
    qtest_add_func("/k230-kpu/l2-load-rdata-prefix-shadow",
                   test_l2_load_uses_rdata_prefix_shadow);
    qtest_add_func("/k230-kpu/runtime-arg-table-direct-io",
                   test_runtime_arg_table_drives_direct_io);
    qtest_add_func("/k230-kpu/l2-store-rdata-alias-destination",
                   test_l2_store_accepts_rdata_alias_destination);
    qtest_add_func("/k230-kpu/l2-store-runtime-ddr-mirror",
                   test_l2_store_runtime_mirrors_to_ddr_source);
    qtest_add_func("/k230-kpu/l2-store-conf-latches-stride",
                   test_l2_store_conf_latches_stride);
    qtest_add_func("/k230-kpu/mfu-act1-identity-u8",
                   test_mfu_act1_identity_u8);
    qtest_add_func("/k230-kpu/mfu-act1-conf-dest-latches-rlen",
                   test_mfu_act1_conf_dest_latches_rlen);
    qtest_add_func("/k230-kpu/mfu-act1-add-clip-u8",
                   test_mfu_act1_add_clip_u8);
    qtest_add_func("/k230-kpu/mfu-act1-fp16-roundtrip",
                   test_mfu_act1_fp16_roundtrip);
    qtest_add_func("/k230-kpu/mfu-act1-mul-fp16-two-l2-sources",
                   test_mfu_act1_mul_fp16_two_l2_sources);
    qtest_add_func("/k230-kpu/mfu-act1-raddr-s2-zero-is-unary",
                   test_mfu_act1_raddr_s2_zero_is_unary);
    qtest_add_func("/k230-kpu/mfu-act1-segment-linefit-fp16",
                   test_mfu_act1_segment_linefit_fp16);
    qtest_add_func("/k230-kpu/mfu-act1-packed-strides-u8",
                   test_mfu_act1_packed_strides_u8);
    qtest_add_func("/k230-kpu/mfu-pdp1-average-u8",
                   test_mfu_pdp1_average_u8);
    qtest_add_func("/k230-kpu/mfu-pdp1-min-fp16-sum-i16",
                   test_mfu_pdp1_min_fp16_sum_i16);
    qtest_add_func("/k230-kpu/mfu-pdp1-sliding-min-u8",
                   test_mfu_pdp1_sliding_min_u8);
    qtest_add_func("/k230-kpu/pu-compute-conv2d-act0-u8",
                   test_pu_compute_conv2d_act0_u8);
    qtest_add_func("/k230-kpu/pu-compute-dm-store-of-act0-dest",
                   test_pu_compute_dm_store_of_act0_dest);
    qtest_add_func("/k230-kpu/pu-compute-conv2d-i8-input-act0-u8",
                   test_pu_compute_conv2d_i8_input_act0_u8);
    qtest_add_func("/k230-kpu/pu-compute-conv2d-packed-if-stride",
                   test_pu_compute_conv2d_packed_if_stride);
    qtest_add_func("/k230-kpu/pu-compute-conv2d-packed-of-stride",
                   test_pu_compute_conv2d_packed_of_stride);
    qtest_add_func("/k230-kpu/pu-compute-of-conf-latches-shape-stride",
                   test_pu_compute_of_conf_latches_shape_stride);
    qtest_add_func("/k230-kpu/pu-compute-psum-accumulates-to-act0",
                   test_pu_compute_psum_accumulates_to_act0);
    qtest_add_func("/k230-kpu/pu-compute-fetchif-stride-without-l1",
                   test_pu_compute_uses_fetchif_stride_without_l1);
    qtest_add_func("/k230-kpu/pu-compute-zero-fetchif-uses-l1-source",
                   test_pu_compute_zero_fetchif_uses_l1_source);
    qtest_add_func("/k230-kpu/pu-compute-fetchif-offset-uses-if-staging",
                   test_pu_compute_fetchif_offset_uses_if_staging);
    qtest_add_func("/k230-kpu/pu-compute-l1-bank-source-raw-offset",
                   test_pu_compute_l1_bank_source_uses_raw_offset);
    qtest_add_func("/k230-kpu/pu-compute-l1-i16-repacks-byte-planes",
                   test_pu_compute_l1_i16_repacks_byte_planes);
    qtest_add_func("/k230-kpu/mfu-act1-adds-psum-and-l2-u8",
                   test_mfu_act1_adds_psum_and_l2_u8);
    qtest_add_func("/k230-kpu/pu-compute-psum-classifier-stride",
                   test_pu_compute_psum_classifier_stride);
    qtest_add_func("/k230-kpu/pu-compute-deconv-1x1-psum-to-act0",
                   test_pu_compute_deconv_1x1_psum_to_act0);
    qtest_add_func("/k230-kpu/pu-compute-deconv-1x1-multi-channel",
                   test_pu_compute_deconv_1x1_multi_channel);
    qtest_add_func("/k230-kpu/mfu-transpose-nwch-i16",
                   test_mfu_transpose_nwch_i16);
    qtest_add_func("/k230-kpu/pdp0-compute-depthwise-act0-u8",
                   test_pdp0_compute_depthwise_act0_u8);
    qtest_add_func("/k230-kpu/pdp0-conf-latches-shape-stride",
                   test_pdp0_conf_latches_shape_stride);
    qtest_add_func("/k230-kpu/pdp0-compute-max-act0-u8",
                   test_pdp0_compute_max_act0_u8);

    return g_test_run();
}

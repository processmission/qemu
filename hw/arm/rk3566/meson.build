# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later

rk3566_ss = ss.source_set()
rk3566_ss.add(files(
  'rk3566-cru.c',
  'rk3566-timer.c',
  'rk3566-otp.c',
  'rk3566-i2c.c',
  'rk3566-tsadc.c',
  'rk3566-pwm.c',
  'rk3566-system.c',
  'lsm6dsv16x.c',
  'rk3566-saradc.c',
  'rk3566-media-audio.c',
  'rk3566-media-npu.c',
  'st25r100.c',
  'vl53l8cx.c',
))

# The camera, RGA2 and VENC models convert image formats with libswscale and
# libavutil, decode H.264 frames with libavcodec and encode them with x264.
# Keep the machine buildable when the optional libraries are not available.
rk3566_libavcodec = dependency('libavcodec', required: false)
rk3566_libavutil = dependency('libavutil', required: false)
rk3566_libswscale = dependency('libswscale', required: false)
rk3566_x264 = dependency('x264', required: false)

if (rk3566_libavcodec.found() and rk3566_libavutil.found() and
    rk3566_libswscale.found() and rk3566_x264.found())
  rk3566_ss.add(files(
    'rk3566-media-rga.c',
  ), rk3566_libavcodec, rk3566_libavutil, rk3566_libswscale, rk3566_x264)
endif

arm_common_ss.add_all(when: 'CONFIG_RK3566', if_true: rk3566_ss)

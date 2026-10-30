# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later

rk3566_ss = ss.source_set()
rk3566_ss.add(files(
  'rk3566-cru.c',
))

arm_common_ss.add_all(when: 'CONFIG_RK3566', if_true: rk3566_ss)

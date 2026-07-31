# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later

mcs51_ss = ss.source_set()
mcs51_ss.add(when: 'CONFIG_STC32G_DSP',
              if_true: files('stc32g_dsp.c'))
mcs51_ss.add(when: 'CONFIG_STC32G_TFPU',
              if_true: files('stc32g_tfpu.c'))

hw_arch += {'mcs51': mcs51_ss}

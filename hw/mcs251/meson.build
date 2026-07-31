# SPDX-License-Identifier: GPL-2.0-or-later

mcs251_ss = ss.source_set()
mcs251_ss.add(when: 'CONFIG_STC32G144K246',
              if_true: files('stc32g.c', 'stc32g144k246.c',
                             'stc32g_timer.c', 'stc32g_uart.c',
                             'stc32g_gpio.c'))

hw_arch += {'mcs251': mcs251_ss}

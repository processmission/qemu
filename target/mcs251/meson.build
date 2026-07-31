# SPDX-License-Identifier: GPL-2.0-or-later

mcs251_ss = ss.source_set()
mcs251_ss.add(files(
  'cpu.c',
  'disas.c',
  'helper.c',
  'translate.c'))

mcs251_common_system_ss = ss.source_set()
mcs251_common_system_ss.add(files(
  'gdbstub.c',
  'machine.c'))

target_arch += {'mcs251': mcs251_ss}
target_common_system_arch += {'mcs251': mcs251_common_system_ss}

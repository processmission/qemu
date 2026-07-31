# Copyright (c) 2026 Process Mission
#
# SPDX-License-Identifier: GPL-2.0-or-later

gen = [
  decodetree.process(
    'mcs51.decode',
    extra_args: ['--static-decode=decode_mcs51', '--insnwidth=16']),
  decodetree.process(
    'mcs251.decode',
    extra_args: ['--static-decode=decode_mcs251', '--insnwidth=16']),
]

mcs51_ss = ss.source_set()
mcs51_ss.add(gen)
mcs51_ss.add(files(
  'cpu.c',
  'decode.c',
  'disas.c',
  'gdbstub.c',
  'helper.c',
  'machine.c',
  'translate.c'))

target_arch += {'mcs51': mcs51_ss}

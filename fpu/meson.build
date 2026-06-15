common_ss.add(when: 'CONFIG_TCG', if_true: files('softfloat.c'))
if not config_all_accel.has_key('CONFIG_TCG')
  system_ss.add(when: 'CONFIG_K230_KPU', if_true: files('softfloat.c'))
endif

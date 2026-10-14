project('mutter-virtual-monitor', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=gnu11'])

executable('vmon', 'vmon.c',
  c_args: ['-Wno-unused-parameter'],
  dependencies: [
    dependency('gio-unix-2.0'),
    dependency('libpipewire-0.3'),
  ],
  install: true)

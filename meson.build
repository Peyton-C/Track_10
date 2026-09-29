project('track10', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

gst = dependency('gstreamer-1.0', version: '>= 1.20')

core_inc = include_directories('core/include')
core = static_library('track10',
  'core/src/player.c',
  'core/src/stemfile.c',
  include_directories: core_inc,
  dependencies: gst,
)
core_dep = declare_dependency(
  link_with: core,
  include_directories: core_inc,
  dependencies: gst,
)

executable('t10-info', 'core/tools/t10-info.c', dependencies: core_dep)

cc = meson.get_compiler('c')
gtk = dependency('gtk4', version: '>= 4.10', required: get_option('gtk'))
if gtk.found()
  subdir('linux')
endif

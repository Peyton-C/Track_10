resources = import('gnome').compile_resources('resources', 'track10.gresource.xml')

executable('track10', 'main.c', resources,
  dependencies: [core_dep, gtk, cc.find_library('m', required: false)],
  install: true,
)

app_id = 'io.github.peyton_c.Track10'

install_data('@0@.desktop'.format(app_id),
  install_dir: get_option('datadir') / 'applications',
)
install_data('../assets/track_10_icon/track_10_icon.png',
  rename: '@0@.png'.format(app_id),
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / '512x512' / 'apps',
)

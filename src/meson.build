project('kibaos-oobe', 'vala', 'c', version: '1.0')

cc = meson.get_compiler('c')
m_dep       = cc.find_library('m', required: true)
threads_dep = dependency('threads')   # needed for GLib.Thread

gtk4_dep    = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1')
gee_dep     = dependency('gee-0.8')

executable(
  'io.kibaos.oobe',
  'main.vala',
  dependencies: [gtk4_dep, adwaita_dep, gee_dep, m_dep, threads_dep],
  install: true
)

executable(
  'io.kibaos.winapps-setup',
  'winapps-setup.vala',
  dependencies: [gtk4_dep, adwaita_dep, m_dep, threads_dep],
  install: true
)

# Stylesheet. main.vala loads <prefix>/share/kibaos-oobe/oobe.css first; if
# it isn't there the installer runs completely unstyled. oobe.css lives in
# the top project dir (one level above this src/ dir).
install_data(
  '../oobe.css',
  install_dir: get_option('datadir') / 'kibaos-oobe'
)

project('osmium', 'c',
          version: '0.1.0',
    meson_version: '>= 0.62.0',
       license: 'LGPL-2.1-or-later',
  default_options: [ 'warning_level=2', 'c_std=gnu11', ],
)

cc = meson.get_compiler('c')
pkg = import('pkgconfig')

gtk_dep = dependency('gtk4', version: '>= 4.14')
curl_dep = dependency('libcurl')
xml_dep = dependency('libxml-2.0')
m_dep = cc.find_library('m', required: false)
libxml_dep = dependency('libxml-2.0')
gdal_dep = dependency('gdal')

add_project_arguments('-fvisibility=hidden', '-Wno-unused-parameter', language: 'c')

osmium_sources = [
  'src/osmium_compass.c',
  'src/osmium_map.c',
  'src/osmium_vector_layer.c',
  'src/osmium_tile_layer.c',
  'src/osmium_wms_layer.c',
  'src/osmium_zoom_control.c',
  'src/vector/vector_data.c',
  'src/tms/tms.c',
  'src/wms/wms.c',
]

osmium_headers = [
  'src/osmium.h',
  'src/osmium_compass.h',
  'src/osmium_map.h',
  'src/osmium_vector_layer.h',
  'src/osmium_tile_layer.h',
  'src/osmium_wms_layer.h',
  'src/osmium_zoom_control.h',
]

osmium_inc = include_directories('src')

libosmium = shared_library('osmium', osmium_sources,
  version: meson.project_version(),
  soversion: '0',
  dependencies: [gtk_dep, curl_dep, m_dep, libxml_dep, gdal_dep],
  include_directories: osmium_inc,
  install: true,
)

install_headers(osmium_headers, subdir: 'osmium')
install_headers('src/wms/wms.h', subdir: 'osmium/wms')

pkg.generate(libosmium,
  name: 'osmium',
  description: 'GTK4 slippy map widget for OpenStreetMap-style tile servers',
  requires: ['gtk4'],
  subdirs: 'osmium',
)

osmium_dep = declare_dependency(
  link_with: libosmium,
  include_directories: osmium_inc,
  dependencies: gtk_dep,
)

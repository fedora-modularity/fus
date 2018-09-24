project('fus', 'c', license : 'GPL-3.0+')

dep_glib = dependency('glib-2.0')
dep_gio = dependency('gio-2.0')
dep_libsolv = dependency('libsolv', version : '>= 0.6.34')
dep_libsolvext = dependency('libsolvext', version : '>= 0.6.34')
dep_modulemd = dependency('modulemd', version : '>= 1.4')

add_project_arguments('-DG_LOG_DOMAIN="fus"', language : 'c')
exe_main = executable('fus', 'fus.c', 'main.c', dependencies : [dep_glib, dep_gio, dep_libsolv, dep_libsolvext, dep_modulemd], install : true)
test('main', exe_main, args : ['@0@/data'.format(meson.current_source_dir())])

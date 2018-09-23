project('fus', 'c', license : 'GPL-3.0+')

dep_glib = dependency('glib-2.0')
dep_gio = dependency('gio-2.0')
dep_libsolv = dependency('libsolv', version : '>= 0.6.34')
dep_libsolvext = dependency('libsolvext', version : '>= 0.6.34')
dep_modulemd = dependency('modulemd', version : '>= 1.4')

add_project_arguments('-DG_LOG_DOMAIN="fus"', language : 'c')
exe_main = executable('fus', 'fus.c', 'main.c', dependencies : [dep_glib, dep_gio, dep_libsolv, dep_libsolvext, dep_modulemd], install : true)
exe_test = executable('tests', 'fus.c', 'tests.c',
    dependencies : [dep_glib, dep_gio, dep_libsolv, dep_libsolvext, dep_modulemd],
    install : false,
    c_args : '-DFUS_TESTING')
test('tests', exe_test,
  env: [
    'G_TEST_SRCDIR=@0@/tests/'.format(meson.current_source_dir()),
    'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
  ],
)

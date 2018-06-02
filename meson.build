project('pungi-solver', 'c')

dep_glib = dependency('glib-2.0')
dep_libsolv = dependency('libsolv', version : '>= 0.6.34')
dep_libsolvext = dependency('libsolvext', version : '>= 0.6.34')
dep_modulemd = dependency('modulemd', version : '>= 1.4')

add_project_arguments('-DG_LOG_DOMAIN="pungi-solver"', language : 'c')
exe_main = executable('main', 'main.c', dependencies : [dep_glib, dep_libsolv, dep_libsolvext, dep_modulemd])
test('main', exe_main)

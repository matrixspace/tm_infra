project('TM kit infra', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=3'])

inc = include_directories('.')
thread_dep = dependency('threads')
common_deps = [thread_dep]
pkg = import('pkgconfig')

if build_machine.system() == 'linux'
    add_global_arguments('-Wno-unknown-pragmas', language: 'cpp')
    add_global_arguments('-Wno-unused-function', language: 'cpp')
    add_global_arguments('-Wno-reorder', language: 'cpp')
    if get_option('buildtype') != 'release'
        add_global_arguments('-fno-omit-frame-pointer', language: 'cpp')
        add_global_arguments('-fno-omit-frame-pointer', language: 'c')
    endif
endif

subdir('tm_kit/infra')
subdir('src')

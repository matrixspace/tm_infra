if get_option('buildtype') == 'debug'
    const_trigger_importer_test_lib = tm_infra_lib_debug
else
    const_trigger_importer_test_lib = tm_infra_lib
endif

const_trigger_importer_test_exe = executable('const_trigger_importer_test', 'ConstTriggerImporterTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: const_trigger_importer_test_lib,
    install: false)
test('const_trigger_importer_test', const_trigger_importer_test_exe, timeout: 30)

const_trigger_importer_bench_exe = executable('const_trigger_importer_bench', 'ConstTriggerImporterBench.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: const_trigger_importer_test_lib,
    install: false)

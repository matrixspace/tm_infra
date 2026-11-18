if get_option('buildtype') == 'debug'
    trigger_importer_test_lib = tm_infra_lib_debug
else
    trigger_importer_test_lib = tm_infra_lib
endif

trigger_importer_test_exe = executable('trigger_importer_test', 'TriggerImporterTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: trigger_importer_test_lib,
    install: false)
test('trigger_importer_test', trigger_importer_test_exe, timeout: 30)

# Throughput benchmark for the real triggerImporter<T,true> node. Not a test:
# run it by hand, pinned to idle cores, to compare the queue container /
# notify strategy before and after optimization.
trigger_importer_bench_exe = executable('trigger_importer_bench', 'TriggerImporterBench.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: trigger_importer_test_lib,
    install: false)

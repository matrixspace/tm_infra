if get_option('buildtype') == 'debug'
    bunched_importer_test_lib = tm_infra_lib_debug
else
    bunched_importer_test_lib = tm_infra_lib
endif

bunched_importer_test_exe = executable('bunched_importer_test', 'BunchedImporterTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: bunched_importer_test_lib,
    install: false)
test('bunched_importer_test', bunched_importer_test_exe, timeout: 30)

bunched_importer_bench_exe = executable('bunched_importer_bench', 'BunchedImporterBench.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: bunched_importer_test_lib,
    install: false)

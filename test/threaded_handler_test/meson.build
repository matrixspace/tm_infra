if get_option('buildtype') == 'debug'
    threaded_handler_test_lib = tm_infra_lib_debug
else
    threaded_handler_test_lib = tm_infra_lib
endif

dedicated_executor_test_exe = executable('dedicated_executor_test', 'DedicatedExecutorTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: threaded_handler_test_lib,
    install: false)
test('dedicated_executor_test', dedicated_executor_test_exe, timeout: 30)

threaded_handler_compatibility_test_exe = executable('threaded_handler_compatibility_test', 'ThreadedHandlerCompatibilityTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: threaded_handler_test_lib,
    install: false)
test('threaded_handler_compatibility_test', threaded_handler_compatibility_test_exe, timeout: 10)

threaded_node_options_test_exe = executable('threaded_node_options_test', 'ThreadedNodeOptionsTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: threaded_handler_test_lib,
    install: false)
test('threaded_node_options_test', threaded_node_options_test_exe, timeout: 30)

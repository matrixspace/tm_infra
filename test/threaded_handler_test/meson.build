if get_option('buildtype') == 'debug'
    threaded_handler_test_lib = tm_infra_lib_debug
else
    threaded_handler_test_lib = tm_infra_lib
endif

threaded_handler_test_exe = executable('threaded_handler_test'
    , ['ThreadedHandlerTest.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: threaded_handler_test_lib
    , install: false
)
test('threaded_handler_test', threaded_handler_test_exe, timeout: 300)

executor_mechanism_test_exe = executable('executor_mechanism_test'
    , ['ExecutorMechanismTest.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: threaded_handler_test_lib
    , install: false
)
test('executor_mechanism_test', executor_mechanism_test_exe, timeout: 30)

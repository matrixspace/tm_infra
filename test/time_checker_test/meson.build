if get_option('buildtype') == 'debug'
    time_checker_test_lib = tm_infra_lib_debug
else
    time_checker_test_lib = tm_infra_lib
endif

time_checker_test_exe = executable('time_checker_test'
    , ['TimeCheckerTest.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: time_checker_test_lib
    , install: false
)
test('time_checker_test', time_checker_test_exe, timeout: 120)

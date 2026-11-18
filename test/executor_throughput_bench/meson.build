if get_option('buildtype') == 'debug'
    executor_throughput_bench_lib = tm_infra_lib_debug
else
    executor_throughput_bench_lib = tm_infra_lib
endif

#benchmark, not a test: run it by hand, pinned to idle cores
executable('executor_throughput_bench'
    , ['ExecutorThroughputBench.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: executor_throughput_bench_lib
    , install: false
)

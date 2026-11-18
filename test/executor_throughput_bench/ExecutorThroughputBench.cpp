#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

//Throughput of the default dedicated executor (DedicatedThreadExecutor<Item>):
//one producer pushes N items, the consumer thread validates that every seq
//arrives exactly once and in order, and the run reports wall/cpu ns per item.
//Measures the mutex-protected incoming queue + consumer-local processing queue
//as a whole (the same machinery every non-customized threaded node uses), so it
//is the microbenchmark for the list->deque / splice->swap / drain / conditional
//notify optimizations of that executor. Not a test: run it by hand, pinned to
//idle cores.

using namespace dev::cd606::tm::infra;

namespace {

    //64-byte trivially-movable payload so the run isolates the queue machinery
    //rather than per-item heap allocation; .seq is the order-check key.
    struct Item {
        uint64_t seq;
        uint64_t a, b, c, d, e, f, g;
    };

    using Exec = DedicatedThreadExecutor<Item>;

    struct Host {
        std::mutex mutex;
        std::condition_variable cond;
        bool released = false;
        std::atomic<bool> entered{false};
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> errors{0};
        uint64_t expected = 0; // consumer-thread only, reset by main while drained

        void executorWaitForStart()
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered.store(true, std::memory_order_release);
            cond.wait(lock, [&] { return released; });
        }
        void release()
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
            cond.notify_one();
        }
        void executorHandle(Item &&x)
        {
            if (x.seq != expected) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            ++expected;
            count.fetch_add(1, std::memory_order_release);
        }
        void executorBeforeBatch(Item const &, std::size_t)
        {
        }
        void executorIdleWork()
        {
        }
    };

    template <class T>
    bool waitFor(std::atomic<T> const &v, T target)
    {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (v.load(std::memory_order_acquire) < target) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    //Pushes `messages` items after a `warmup` drain and returns wall/cpu ns per
    //item. Returns a negative value when delivery or order verification fails.
    struct RunResult {
        double wallNs;
        double cpuNs;
        bool ok;
    };
    RunResult runOnce(int64_t warmup, int64_t messages)
    {
        Host host;
        Exec exec;
        exec.start(&host);
        if (!waitFor(host.entered, true)) {
            return {-1.0, -1.0, false};
        }
        host.release();

        //warmup: fill and drain so the first measured batch is not the cold path
        for (int64_t i = 0; i < warmup; ++i) {
            exec.push(Item{static_cast<uint64_t>(i), 0, 0, 0, 0, 0, 0, 0});
        }
        if (!waitFor(host.count, static_cast<uint64_t>(warmup))) {
            return {-1.0, -1.0, false};
        }

        //drained: reset the order-check baseline for the measured run
        host.expected = 0;
        uint64_t const base = host.count.load(std::memory_order_acquire);

        auto const cpuBegin = std::clock();
        auto const wallBegin = std::chrono::steady_clock::now();
        for (int64_t i = 0; i < messages; ++i) {
            exec.push(Item{static_cast<uint64_t>(i), 0, 0, 0, 0, 0, 0, 0});
        }
        auto const wallEnd = std::chrono::steady_clock::now();
        auto const cpuEnd = std::clock();

        if (!waitFor(host.count, base + static_cast<uint64_t>(messages))) {
            return {-1.0, -1.0, false};
        }
        bool const ok = host.count.load(std::memory_order_acquire) == base + static_cast<uint64_t>(messages)
                        && host.errors.load(std::memory_order_relaxed) == 0;
        double const wallNs = std::chrono::duration<double, std::nano>(wallEnd - wallBegin).count();
        double const cpuNs = 1e9 * static_cast<double>(cpuEnd - cpuBegin) / CLOCKS_PER_SEC;
        return {wallNs / messages, cpuNs / messages, ok};
    }

    double median(std::vector<double> v)
    {
        if (v.empty()) {
            return 0.0;
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
} // namespace

int main(int argc, char **argv)
{
    int64_t const messages = (argc > 1) ? std::atoll(argv[1]) : 5000000;
    int64_t const warmup = (argc > 2) ? std::atoll(argv[2]) : 100000;
    int const runs = (argc > 3) ? std::atoi(argv[3]) : 5;
    std::printf("DedicatedThreadExecutor throughput: single producer -> push -> consumer\n");
    std::printf("messages=%lld warmup=%lld runs=%d\n"
        , static_cast<long long>(messages), static_cast<long long>(warmup), runs);
    if (messages <= 0 || messages > 100000000 || warmup < 0 || warmup > messages || runs < 1) {
        return 2;
    }
    std::vector<double> wallNs, cpuNs;
    bool allOk = true;
    for (int r = 0; r < runs; ++r) {
        auto const res = runOnce(warmup, messages);
        if (!res.ok) {
            allOk = false;
            std::fprintf(stderr, "run %d: delivery/order check failed\n", r);
            continue;
        }
        wallNs.push_back(res.wallNs);
        cpuNs.push_back(res.cpuNs);
        std::printf("run %d: wall=%7.1f ns/item cpu=%7.1f ns/item %8.0f kitem/s\n"
            , r, res.wallNs, res.cpuNs, 1e6 / res.wallNs);
        std::fflush(stdout);
    }
    if (!wallNs.empty()) {
        double const wMin = *std::min_element(wallNs.begin(), wallNs.end());
        double const wMax = *std::max_element(wallNs.begin(), wallNs.end());
        double const cMin = *std::min_element(cpuNs.begin(), cpuNs.end());
        double const cMax = *std::max_element(cpuNs.begin(), cpuNs.end());
        std::printf("median: wall=%7.1f ns/item (range %.1f..%.1f)  cpu=%7.1f ns/item (range %.1f..%.1f)\n"
            , median(wallNs), wMin, wMax, median(cpuNs), cMin, cMax);
    }
    return allOk && !wallNs.empty() ? 0 : 1;
}

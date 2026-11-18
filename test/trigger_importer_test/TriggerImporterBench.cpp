#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

//Throughput of the real triggerImporter<T,true> node: how fast one producer
//can push items through the node's own thread into a counting sink. Used to
//compare the queue container / drain / notify strategy before and after
//optimization (list+splice vs deque+swap, and unconditional vs empty->non-empty
//notify). Not a test: run it by hand, pinned to idle cores, one process per
//measurement, and take the median of several runs (a single run can swing by
//tens of percent on a shared box).

using namespace dev::cd606::tm;

namespace {

    struct TestClockComponent {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        TimePointType now() {
            return std::chrono::system_clock::now();
        }
        TimePointType resolveTime() {
            return std::chrono::system_clock::now();
        }
        TimePointType resolveTime(TimePointType const &) {
            return std::chrono::system_clock::now();
        }
        void log(infra::LogLevel, std::string const &) {}
    };

    using Env = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, TestClockComponent>;
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;

    constexpr int64_t kSentinel = -1;

    struct Sink {
        std::atomic<int64_t> receivedAll {0}; // sentinels included
        std::atomic<int64_t> count {0};       // real items only
        std::atomic<int64_t> bad {0};
        int64_t nextExpected = 0; // node thread only
        void handle(int64_t v) {
            receivedAll.fetch_add(1, std::memory_order_relaxed);
            if (v == kSentinel) {
                return;
            }
            if (v != nextExpected) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            ++nextExpected;
            count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    bool waitFor(int64_t wanted, std::atomic<int64_t> const &counter, std::chrono::seconds timeout) {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (counter.load(std::memory_order_relaxed) != wanted) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    bool waitDrained(std::atomic<int64_t> const &counter) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        int64_t last = counter.load(std::memory_order_relaxed);
        auto stable = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            int64_t cur = counter.load(std::memory_order_relaxed);
            if (cur != last) {
                last = cur;
                stable = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - stable >= std::chrono::milliseconds(50)) {
                return true;
            }
        }
        return false;
    }
} // namespace

int main(int argc, char **argv) {
    int64_t const messages = (argc > 1) ? std::atoll(argv[1]) : 5000000;
    int64_t const warmup = (argc > 2) ? std::atoll(argv[2]) : 100000;
    std::printf("triggerImporter throughput: trigger -> counting sink, single producer\n");
    std::printf("messages=%lld warmup=%lld\n", static_cast<long long>(messages), static_cast<long long>(warmup));
    if (messages <= 0 || messages > 100000000 || warmup < 0 || warmup > messages) {
        return 2;
    }

    Sink sink;
    auto *env = new Env(); //leaked: the importer thread outlives main
    auto *r = new R(env);
    auto imp = M::triggerImporter<int64_t>();
    auto exporter = M::template pureExporter<int64_t>([&sink](int64_t &&v) {
        sink.handle(v);
    });
    r->connect(r->importItem("src", std::get<0>(imp)), r->exporterAsSink("sink", exporter));
    r->finalize();
    auto &fire = std::get<1>(imp);

    //start the importer thread: fire sentinels until one is delivered
    auto const startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sink.receivedAll.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < startDeadline) {
        fire(int64_t(kSentinel));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (sink.receivedAll.load(std::memory_order_relaxed) == 0 || !waitDrained(sink.receivedAll)) {
        std::fprintf(stderr, "triggerImporter thread did not start / queue did not drain\n");
        return 1;
    }

    //warm up: flush the queue and let everything settle
    for (int64_t i = 0; i < warmup; ++i) {
        fire(int64_t(i));
    }
    if (!waitFor(warmup, sink.count, std::chrono::seconds(120))) {
        std::fprintf(stderr, "warmup did not flush (%lld != %lld)\n"
            , static_cast<long long>(sink.count.load(std::memory_order_relaxed)), static_cast<long long>(warmup));
        return 1;
    }
    sink.nextExpected = 0;
    int64_t const base = sink.count.load(std::memory_order_relaxed);

    auto const cpuBegin = std::clock();
    auto const wallBegin = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < messages; ++i) {
        fire(int64_t(i));
    }
    bool const delivered = waitFor(base + messages, sink.count, std::chrono::seconds(120));
    auto const wallEnd = std::chrono::steady_clock::now();
    auto const cpuEnd = std::clock();

    double const wallNs = std::chrono::duration<double, std::nano>(wallEnd - wallBegin).count();
    double const cpuNs = 1e9 * static_cast<double>(cpuEnd - cpuBegin) / CLOCKS_PER_SEC;
    std::printf("pushes=%8lld got=%8lld bad=%8lld  wall=%7.1f ns/item cpu=%7.1f ns/item %8.0f kitem/s\n"
        , static_cast<long long>(messages)
        , static_cast<long long>(sink.count.load(std::memory_order_relaxed) - base)
        , static_cast<long long>(sink.bad.load(std::memory_order_relaxed))
        , wallNs / messages, cpuNs / messages, static_cast<double>(messages) / (wallNs / 1e6));
    std::fflush(stdout);
    if (!delivered) {
        std::fprintf(stderr, "did not deliver exact expected items (got=%lld)\n"
            , static_cast<long long>(sink.count.load(std::memory_order_relaxed) - base));
        return 1;
    }
    if (sink.bad.load(std::memory_order_relaxed) != 0) {
        std::fprintf(stderr, "order check failed (%lld bad)\n", static_cast<long long>(sink.bad.load(std::memory_order_relaxed)));
        return 1;
    }
    //graph leaked on purpose: the importer thread outlives main
    return 0;
}

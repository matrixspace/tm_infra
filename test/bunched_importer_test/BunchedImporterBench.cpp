#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

//Throughput of the production bunchedImporter<T> fed by triggerImporter<T>:
//the trigger publishes single items, the bunch node aggregates them into
//vector<T> batches. Measures wall/cpu ns per pushed item with the real
//production nodes. Not a test: run it by hand, pinned to idle cores (same
//advice as the other throughput benches). The sink counts the aggregated items
//and verifies they arrive in order with no loss/duplication. Columns: wall/cpu
//ns per pushed item, thousands of items/s.

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

    struct Payload { int64_t seq; };

    //Counts the aggregated items and checks that values arrive strictly in
    //order with no gaps/duplicates. handle() runs in the bunch node's thread.
    struct VectorSink final : public M::template AbstractExporter<std::vector<Payload>> {
        std::atomic<int64_t> items{0};
        std::atomic<int64_t> bad{0};
        std::atomic<int64_t> nextExpected{0};
        virtual void start(Env *) override final {}
        virtual void handle(typename M::template InnerData<std::vector<Payload>> &&d) override final {
            for (auto const &v : d.timedData.value) {
                items.fetch_add(1, std::memory_order_relaxed);
                if (v.seq >= 0) {
                    int64_t const expected = nextExpected.load(std::memory_order_relaxed);
                    if (v.seq != expected) {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    }
                    nextExpected.store(v.seq + 1, std::memory_order_relaxed);
                }
            }
        }
    };

    //Wait until the count stops changing, so any in-flight items that were
    //still buffered in the importer have been published.
    template <class F>
    int64_t settle(F const &readCount) {
        int64_t prev = readCount();
        for (int i = 0; i < 1000; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            int64_t cur = readCount();
            if (cur == prev) {
                return cur;
            }
            prev = cur;
        }
        return prev;
    }
} // namespace

int main(int argc, char **argv) {
    int64_t const messages = (argc > 1) ? std::atoll(argv[1]) : 5000000;
    int64_t const warmup = (argc > 2) ? std::atoll(argv[2]) : 100000;
    std::printf("bunchedImporter throughput: triggerImporter<T> -> bunchedImporter<T> -> vector sink\n");
    std::printf("messages=%lld warmup=%lld\n", static_cast<long long>(messages), static_cast<long long>(warmup));
    if (messages <= 0 || messages > 100000000 || warmup < 0 || warmup > messages) {
        return 2;
    }

    auto *env = new Env(); //leaked: node threads outlive main
    auto *r = new R(env);
    auto *sink = new VectorSink();
    auto underlying = M::triggerImporter<Payload>();
    auto bunched = M::bunchedImporter<Payload>(std::get<0>(underlying));
    auto sinkHolder = M::exporter(sink);
    r->connect(r->importItem("bunched", bunched), r->exporterAsSink("sink", sinkHolder));
    r->finalize();
    auto &fire = std::get<1>(underlying);

    //start both threads, then flush any in-flight items before baselining
    for (int tries = 0; tries < 1000 && sink->items.load() == 0; ++tries) {
        fire(Payload{-1});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (sink->items.load() == 0) {
        std::fprintf(stderr, "bunchedImporter thread never started\n");
        return 1;
    }
    int64_t const base0 = settle([&] { return sink->items.load(); });

    //warm-up volume, drained before the measured run
    for (int64_t i = 0; i < warmup; ++i) {
        fire(Payload{-1});
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (sink->items.load() - base0 < warmup && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (sink->items.load() - base0 != warmup) {
        std::fprintf(stderr, "bunchedImporter warmup did not flush (%lld != %lld)\n"
            , static_cast<long long>(sink->items.load() - base0), static_cast<long long>(warmup));
        return 1;
    }
    int64_t const base = settle([&] { return sink->items.load(); });
    sink->nextExpected.store(0, std::memory_order_relaxed);

    auto const cpuBegin = std::clock();
    auto const wallBegin = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < messages; ++i) {
        fire(Payload{i});
    }
    auto const runDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (sink->items.load() - base < messages && std::chrono::steady_clock::now() < runDeadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    auto const wallEnd = std::chrono::steady_clock::now();
    auto const cpuEnd = std::clock();

    int64_t const got = sink->items.load() - base;
    int64_t const bad = sink->bad.load(std::memory_order_relaxed);
    bool const ok = got == messages && bad == 0;

    double const wallNs = std::chrono::duration<double, std::nano>(wallEnd - wallBegin).count();
    double const cpuNs = 1e9 * static_cast<double>(cpuEnd - cpuBegin) / CLOCKS_PER_SEC;
    std::printf("pushes=%8lld got=%8lld bad=%8lld  wall=%7.0f ns/item cpu=%7.0f ns/item %8.0f kitem/s\n"
        , static_cast<long long>(messages), static_cast<long long>(got), static_cast<long long>(bad)
        , wallNs / messages, cpuNs / messages, static_cast<double>(messages) / (wallNs / 1e6));
    std::fflush(stdout);
    if (!ok) {
        std::fprintf(stderr, "bunchedImporter did not deliver exact expected items (got=%lld bad=%lld)\n"
            , static_cast<long long>(got), static_cast<long long>(bad));
    }
    return ok ? 0 : 1;
}

#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

//Throughput of the self-threaded constTriggerImporter<T>: the trigger function
//publishes a copy of the same T per call and the run thread drains the
//accumulated count in one batch. Measures wall/cpu ns per trigger with the real
//production node. Not a test: run it by hand, pinned to idle cores (same advice
//as the other throughput benches). Columns: wall/cpu ns per trigger, thousands
//of triggers/s.

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

    struct Payload { int64_t value; };

    //Wait until the count stops changing, so any in-flight triggers that were
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
    std::printf("constTriggerImporter throughput: trigger -> counting sink\n");
    std::printf("messages=%lld warmup=%lld\n", static_cast<long long>(messages), static_cast<long long>(warmup));
    if (messages <= 0 || messages > 100000000 || warmup < 0 || warmup > messages) {
        return 2;
    }

    auto *env = new Env(); //leaked: the importer thread outlives main
    auto *r = new R(env);

    std::atomic<int64_t> received{0};
    auto trigger = M::constTriggerImporter<Payload>(Payload{42});
    auto sink = M::template simpleExporter<Payload>([&](typename M::template InnerData<Payload> &&) {
        received.fetch_add(1, std::memory_order_relaxed);
    });

    r->connect(r->importItem("ct", std::get<0>(trigger)), r->exporterAsSink("sink", sink));
    r->finalize();
    auto &fire = std::get<1>(trigger);

    //start the thread, then flush any in-flight triggers before baselining
    for (int tries = 0; tries < 1000 && received.load() == 0; ++tries) {
        fire();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (received.load() == 0) {
        std::fprintf(stderr, "constTriggerImporter thread never started\n");
        return 1;
    }
    int64_t const base0 = settle([&] { return received.load(); });

    //warm-up volume, drained before the measured run
    for (int64_t i = 0; i < warmup; ++i) {
        fire();
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (received.load() - base0 < warmup && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (received.load() - base0 != warmup) {
        std::fprintf(stderr, "constTriggerImporter warmup did not flush (%lld != %lld)\n"
            , static_cast<long long>(received.load() - base0), static_cast<long long>(warmup));
        return 1;
    }
    int64_t const base = settle([&] { return received.load(); });

    auto const cpuBegin = std::clock();
    auto const wallBegin = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < messages; ++i) {
        fire();
    }
    auto const runDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (received.load() - base < messages && std::chrono::steady_clock::now() < runDeadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    auto const wallEnd = std::chrono::steady_clock::now();
    auto const cpuEnd = std::clock();

    int64_t const got = received.load() - base;
    bool const ok = got == messages;

    double const wallNs = std::chrono::duration<double, std::nano>(wallEnd - wallBegin).count();
    double const cpuNs = 1e9 * static_cast<double>(cpuEnd - cpuBegin) / CLOCKS_PER_SEC;
    std::printf("triggers=%8lld got=%8lld  wall=%7.0f ns/item cpu=%7.0f ns/item %8.0f ktrigger/s\n"
        , static_cast<long long>(messages), static_cast<long long>(got)
        , wallNs / messages, cpuNs / messages, static_cast<double>(messages) / (wallNs / 1e6));
    std::fflush(stdout);
    if (!ok) {
        std::fprintf(stderr, "constTriggerImporter did not deliver exact expected items (got=%lld)\n", static_cast<long long>(got));
    }
    return ok ? 0 : 1;
}

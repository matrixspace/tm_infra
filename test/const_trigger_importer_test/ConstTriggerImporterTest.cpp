#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

//Functional test for the self-threaded constTriggerImporter<T>: each trigger
//call publishes a fresh copy of the same T and the run thread drains the
//accumulated count in one batch (so rapid triggers may be coalesced). This
//verifies, through a real graph (importer -> counting sink), that N trigger
//calls deliver exactly N items, each equal to the original value, with no loss
//or duplication.

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

    int g_failures = 0;
    void check(bool cond, std::string const &what) {
        if (!cond) {
            ++g_failures;
            std::printf("FAIL: %s\n", what.c_str());
        } else {
            std::printf("ok:   %s\n", what.c_str());
        }
    }

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

int main() {
    constexpr int64_t kN = 200000;
    auto *env = new Env(); //leaked: the importer thread outlives main
    auto *r = new R(env);

    std::atomic<int64_t> received{0};
    std::atomic<int64_t> bad{0};
    auto trigger = M::constTriggerImporter<Payload>(Payload{42});
    auto sink = M::template simpleExporter<Payload>([&](typename M::template InnerData<Payload> &&d) {
        if (d.timedData.value.value != 42) {
            bad.fetch_add(1, std::memory_order_relaxed);
        }
        received.fetch_add(1, std::memory_order_relaxed);
    });

    r->connect(r->importItem("ct", std::get<0>(trigger)), r->exporterAsSink("sink", sink));
    r->finalize();
    auto &fire = std::get<1>(trigger);

    //constTriggerImporter sets its running flag in start() before creating
    //its thread, so fire() right after finalize() is delivered; the retry is
    //kept as a warm-up guard.
    for (int tries = 0; tries < 1000 && received.load() == 0; ++tries) {
        fire();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(received.load() > 0, "constTriggerImporter thread started and delivered a warm-up item");

    int64_t const base = settle([&] { return received.load(); });
    for (int64_t i = 0; i < kN; ++i) {
        fire();
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (received.load() - base < kN && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    check(received.load() - base == kN, "constTriggerImporter delivered exactly N items");
    check(bad.load() == 0, "constTriggerImporter delivered the same value each time");

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}

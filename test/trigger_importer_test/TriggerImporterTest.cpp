#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

//Functional tests for triggerImporter<T,true> (the self-threaded importer):
//  1. a single producer fires N int64_t items; the sink must receive exactly
//     N of them, in order, with no loss/duplication
//  2. several producer threads fire tagged items; nothing may be lost or
//     duplicated and each producer's sequence must stay in order
//
//triggerImporter sets its running flag in start() before creating its
//thread, so fire() right after finalize() is delivered. Every test still
//hand-shakes with a sentinel value until the thread is up, then waits for
//the queue to drain before firing the measured items.

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

    int g_failures = 0;
    void check(bool ok, char const *what) {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
        g_failures += !ok;
    }

    template <class F>
    bool waitUntil(F pred, std::chrono::milliseconds timeout) {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    //Collects every delivered value (sentinels included) in arrival order.
    template <class T>
    struct Collector {
        std::mutex mtx;
        std::vector<T> seqs;
        T sentinel;
        explicit Collector(T s) : sentinel(std::move(s)) {}
        void handle(T &&v) {
            std::lock_guard<std::mutex> lock(mtx);
            seqs.push_back(std::move(v));
        }
        int64_t size() {
            std::lock_guard<std::mutex> lock(mtx);
            return static_cast<int64_t>(seqs.size());
        }
        std::vector<T> snapshot() {
            std::lock_guard<std::mutex> lock(mtx);
            return seqs;
        }
    };

    //Fires the sentinel until the importer thread is up (at least one item
    //delivered), then waits until the queue has drained (delivered count is
    //stable for a quiet window). Returns the number of delivered sentinels.
    template <class T, class Fire, class Count>
    bool startTriggerAndDrain(Fire const &fire, T const &sentinel, Count const &count, int64_t *base) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (count() == 0 && std::chrono::steady_clock::now() < deadline) {
            fire(T(sentinel));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (count() == 0) {
            return false;
        }
        int64_t last = count();
        auto stable = std::chrono::steady_clock::now();
        auto const drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < drainDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            int64_t cur = count();
            if (cur != last) {
                last = cur;
                stable = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - stable >= std::chrono::milliseconds(50)) {
                *base = cur;
                return true;
            }
        }
        return false;
    }

    void singleProducerFifoTest() {
        constexpr int64_t kSentinel = -1;
        constexpr int64_t kN = 100000;
        Collector<int64_t> state(kSentinel);
        Env env;
        auto r = std::make_unique<R>(&env);
        auto imp = M::triggerImporter<int64_t>();
        auto sink = M::template pureExporter<int64_t>([&state](int64_t &&v) {
            state.handle(std::move(v));
        });
        r->connect(r->importItem("src", std::get<0>(imp)), r->exporterAsSink("sink", sink));
        r->finalize();
        auto &fire = std::get<1>(imp);

        int64_t base = 0;
        check(startTriggerAndDrain<int64_t>(fire, kSentinel, [&] { return state.size(); }, &base), "thread started and queue drained");

        for (int64_t i = 0; i < kN; ++i) {
            fire(int64_t(i));
        }
        check(waitUntil([&] { return state.size() == base + kN; }, std::chrono::seconds(30)), "single producer: all items delivered");

        std::get<0>(imp).reset();
        r.reset(); // Join the importer while the sink and its state are still alive.
        auto seqs = state.snapshot();
        bool exact = static_cast<int64_t>(seqs.size()) == base + kN;
        for (int64_t i = 0; i < base && exact; ++i) {
            if (seqs[static_cast<std::size_t>(i)] != kSentinel) {
                exact = false;
            }
        }
        for (int64_t i = 0; i < kN && exact; ++i) {
            if (seqs[static_cast<std::size_t>(base + i)] != i) {
                exact = false;
            }
        }
        check(exact, "single producer: exact count, no loss/dup, seq 0..N-1 in order");
    }

    void uniformImporterStopTest() {
        Env env;
        auto runner = std::make_unique<R>(&env);
        std::promise<void> entered;
        auto enteredFuture = entered.get_future();
        bool interrupted = false;
        auto importer = M::uniformSimpleImporter<int>([&](Env *, M::ImporterStopContext const &stop)
            -> std::tuple<bool, M::Data<int>> {
            entered.set_value();
            interrupted = stop.waitForStop(std::chrono::seconds(60));
            return {false, std::nullopt};
        });
        auto sink = M::pureExporter<int>([](int &&) {});
        runner->connect(runner->importItem("uniform", importer), runner->exporterAsSink("sink", sink));
        runner->finalize();
        check(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "uniform importer entered callback");
        importer.reset();
        runner.reset();
        check(interrupted, "uniform importer wakes callback on destruction");
    }

    struct TaggedItem {
        int32_t producer;
        int64_t seq;
    };

    void multiProducerNoLossTest() {
        constexpr int32_t kProducers = 4;
        constexpr int64_t kPerProducer = 25000;
        Collector<TaggedItem> state(TaggedItem {-1, -1});
        Env env;
        auto r = std::make_unique<R>(&env);
        auto imp = M::triggerImporter<TaggedItem>();
        auto sink = M::template pureExporter<TaggedItem>([&state](TaggedItem &&v) {
            state.handle(std::move(v));
        });
        r->connect(r->importItem("src", std::get<0>(imp)), r->exporterAsSink("sink", sink));
        r->finalize();
        auto &fire = std::get<1>(imp);

        int64_t base = 0;
        check(startTriggerAndDrain<TaggedItem>(fire, TaggedItem {-1, -1}, [&] { return state.size(); }, &base), "multi-producer: thread started and queue drained");

        std::vector<std::thread> producers;
        for (int32_t p = 0; p < kProducers; ++p) {
            producers.emplace_back([&fire, p] {
                for (int64_t s = 0; s < kPerProducer; ++s) {
                    fire(TaggedItem {p, s});
                }
            });
        }
        for (auto &t : producers) {
            t.join();
        }
        int64_t const expected = kProducers * kPerProducer;
        check(waitUntil([&] { return state.size() == base + expected; }, std::chrono::seconds(30)), "multi-producer: all items delivered");

        std::get<0>(imp).reset();
        r.reset(); // Join the importer while the sink and its state are still alive.
        auto seqs = state.snapshot();
        bool exact = static_cast<int64_t>(seqs.size()) == base + expected;
        for (int64_t i = 0; i < base && exact; ++i) {
            if (seqs[static_cast<std::size_t>(i)].producer != -1) {
                exact = false;
            }
        }
        int64_t nextExpected[kProducers] = {};
        for (int64_t i = base; i < base + expected && exact; ++i) {
            TaggedItem const &v = seqs[static_cast<std::size_t>(i)];
            if (v.producer < 0 || v.producer >= kProducers || v.seq != nextExpected[v.producer]++) {
                exact = false;
            }
        }
        for (int32_t p = 0; p < kProducers && exact; ++p) {
            if (nextExpected[p] != kPerProducer) {
                exact = false;
            }
        }
        check(exact, "multi-producer: no loss/dup, each producer's seq in order");
    }
} // namespace

int main() {
    singleProducerFifoTest();
    multiProducerNoLossTest();
    uniformImporterStopTest();
    std::puts(g_failures ? "FAILED" : "ALL PASSED");
    return g_failures ? 1 : 0;
}

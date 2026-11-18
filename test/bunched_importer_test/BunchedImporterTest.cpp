#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

//Functional test for bunchedImporter<T>: it wraps a self-threaded importer
//(here triggerImporter<T>) and aggregates the stream into vector<T> batches.
//This verifies, through a real graph, that N pushed items arrive as batches
//whose total item count is exactly N, strictly in order with no loss or
//duplication, and that the finalFlag stays false when no final item was
//produced. A second part drives the importer with a custom source that can emit
//a final flag and checks that the final batch carries it and that later items
//are dropped once the node has stopped.

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

    template <class T>
    inline int64_t seqOf(T const &v) { return v.seq; }

    //Sink for vector<T> batches: counts total items, checks strict order for
    //non-negative seq values (negative values are warm-up noise), and records
    //how many batches carried finalFlag.
    template <class T>
    struct VectorSink final : public M::template AbstractExporter<std::vector<T>> {
        std::atomic<int64_t> items{0};
        std::atomic<int64_t> bad{0};
        std::atomic<int64_t> finalBatches{0};
        std::atomic<int64_t> nonFinalBatches{0};
        std::atomic<int64_t> nextExpected{0};
        virtual void start(Env *) override final {}
        virtual void handle(typename M::template InnerData<std::vector<T>> &&d) override final {
            (d.timedData.finalFlag ? finalBatches : nonFinalBatches).fetch_add(1, std::memory_order_relaxed);
            for (auto const &v : d.timedData.value) {
                items.fetch_add(1, std::memory_order_relaxed);
                int64_t const seq = seqOf(v);
                if (seq >= 0) {
                    int64_t const expected = nextExpected.load(std::memory_order_relaxed);
                    if (seq != expected) {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    }
                    nextExpected.store(seq + 1, std::memory_order_relaxed);
                }
            }
        }
    };

    int g_failures = 0;
    void check(bool cond, std::string const &what) {
        if (!cond) {
            ++g_failures;
            std::printf("FAIL: %s\n", what.c_str());
        } else {
            std::printf("ok:   %s\n", what.c_str());
        }
    }

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

    //A source that pushes items with an explicit final flag, so the final
    //semantics of bunchedImporter can be exercised directly.
    template <class T>
    struct FinalPushSource final : public M::template AbstractImporter<T> {
        virtual void start(Env *) override final {}
        void push(Env *env, T &&v, bool finalFlag) {
            this->publish(M::template pureInnerData<T>(env, std::move(v), finalFlag));
        }
    };
} // namespace

int main() {
    {
        constexpr int64_t kN = 200000;
        auto *env = new Env(); //leaked: node threads outlive main
        auto *r = new R(env);
        auto *sink = new VectorSink<Payload>();
        auto underlying = M::triggerImporter<Payload>();
        auto bunched = M::bunchedImporter<Payload>(std::get<0>(underlying));
        auto sinkHolder = M::exporter(sink);
        r->connect(r->importItem("bunched", bunched), r->exporterAsSink("sink", sinkHolder));
        r->finalize();
        auto &fire = std::get<1>(underlying);

        //The trigger and bunch threads set their running flags in start()
        //before creating their threads, so fire() right after finalize() is
        //delivered; the retry is kept as a warm-up guard (negative seq is
        //ignored by the order check).
        for (int tries = 0; tries < 1000 && sink->items.load() == 0; ++tries) {
            fire(Payload{-1});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(sink->items.load() > 0, "bunchedImporter thread started and delivered a warm-up item");
        int64_t const base = settle([&] { return sink->items.load(); });
        sink->nextExpected.store(0, std::memory_order_relaxed);

        for (int64_t i = 0; i < kN; ++i) {
            fire(Payload{i});
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (sink->items.load() - base < kN && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        check(sink->items.load() - base == kN, "bunchedImporter aggregated exactly N items");
        check(sink->bad.load() == 0, "bunchedImporter preserved order with no loss/duplication");
        check(sink->finalBatches.load() == 0, "bunchedImporter did not set finalFlag for non-final inputs");
        check(sink->nonFinalBatches.load() > 0, "bunchedImporter emitted at least one batch");
    }

    {
        //finalFlag: a final item marks its batch final and stops the node.
        auto *env = new Env();
        auto *r = new R(env);
        auto *sink = new VectorSink<Payload>();
        auto *src = new FinalPushSource<Payload>();
        auto bunched = M::bunchedImporter<Payload>(M::importer(src));
        auto sinkHolder = M::exporter(sink);
        r->connect(r->importItem("bunched", bunched), r->exporterAsSink("sink", sinkHolder));
        r->finalize();

        for (int tries = 0; tries < 1000 && sink->items.load() == 0; ++tries) {
            src->push(env, Payload{-1}, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        int64_t const base = settle([&] { return sink->items.load(); });
        sink->nextExpected.store(0, std::memory_order_relaxed);

        src->push(env, Payload{0}, false);
        src->push(env, Payload{1}, true); //final
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while ((sink->items.load() - base < 2 || sink->finalBatches.load() == 0) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(sink->items.load() - base == 2, "bunchedImporter delivered the two non-warm-up items");
        check(sink->finalBatches.load() == 1, "bunchedImporter marked exactly one final batch");
        check(sink->bad.load() == 0, "bunchedImporter final path preserved order");

        //the node has now stopped: a later push is dropped
        src->push(env, Payload{2}, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(sink->items.load() - base == 2, "bunchedImporter dropped items after the final flag");
    }

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}

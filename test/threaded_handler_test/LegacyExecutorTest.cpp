#include <tm_kit/infra/LegacyDedicatedThreadExecutor.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace dev::cd606::tm::infra;
namespace
{
    int failures = 0;
    void check(bool ok, char const *name)
    {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", name);
        failures += !ok;
    }
    template <class F>
    bool wait(F pred)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }
    struct Item {
        unsigned producer;
        uint64_t seq;
        std::unique_ptr<uint64_t> value;
        Item(unsigned p, uint64_t s)
            : producer(p)
            , seq(s)
            , value(new uint64_t(s))
        {
        }
        Item(Item &&) = default;
        Item &operator=(Item &&) = default;
    };
    using Exec = LegacyDedicatedThreadExecutor<Item>;
    struct Env : RealTimeThreadedExecutorComponent<LegacyDedicatedThreadExecutor> {};
    static_assert(std::is_same_v<RealTimeThreadedExecutorFor<Env, Item>, Exec>);
    struct Host {
        std::mutex mutex;
        std::condition_variable cond;
        bool released = false;
        std::atomic<bool> entered{false};
        std::atomic<uint64_t> count{0}, errors{0}, idle{0}, batches{0}, firstBatch{0};
        std::array<uint64_t, 4> expected{};
        std::function<void()> afterHandle;
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
            if (x.seq != expected[x.producer]++ || !x.value || *x.value != x.seq) {
                errors.fetch_add(1);
            }
            if (afterHandle) {
                afterHandle();
            }
            count.fetch_add(1, std::memory_order_release);
        }
        void executorBeforeBatch(Item const &, std::size_t n)
        {
            if (batches.fetch_add(1) == 0) {
                firstBatch.store(n);
            }
        }
        bool executorHasIdleWork()
        {
            return false;
        }
        void executorIdleWork()
        {
            idle.fetch_add(1);
        }
    };
    void fifoAndIdleTest()
    {
        // Hosts live to process exit: the executor contract has detached shutdown.
        auto *h = new Host;
        ThreadedExecOptions opts;
        opts.trackPendingCount = true;
        auto *e = new Exec(opts);
        check(!e->push(Item(0, 0)) && !e->threadHandle(), "reject before start, no native handle");
        e->start(h);
        check(wait([&] { return h->entered.load(std::memory_order_acquire); }) && e->threadHandle().has_value(), "start barrier and native handle");
        std::vector<std::thread> producers;
        for (unsigned p = 0; p < 4; ++p) {
            producers.emplace_back([=] {
                for (uint64_t seq = 0; seq < 5000; ++seq) {
                    if (!e->push(Item(p, seq))) {
                        std::abort();
                    }
                }
            });
        }
        for (auto &p : producers) {
            p.join();
        }
        check(h->count.load() == 0, "startup gates callbacks");
        h->release();
        check(wait([&] { return h->count.load(std::memory_order_acquire) == 20000; }) && h->errors.load() == 0, "four producers, move-only non-default Item, exact count and FIFO");
        check(h->batches.load() == 1 && h->firstBatch.load() == 20000, "splice batch reports its original list size");
        check(wait([&] { return h->idle.load() > 0; }), "historical periodic idle retained even when hasIdleWork is false");
        e->stop();
        check(!e->push(Item(0, 5000)), "reject after stop");
        delete e;
    }
    void callbackStopTest()
    {
        auto *h = new Host;
        auto *e = new Exec;
        h->afterHandle = [e] { e->stop(); };
        e->start(h);
        check(wait([&] { return h->entered.load(std::memory_order_acquire); }), "callback-stop start entered");
        e->push(Item(0, 0));
        e->push(Item(0, 1));
        h->release();
        check(wait([&] { return h->count.load(std::memory_order_acquire) >= 1; }), "callback may stop its executor");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(h->count.load() == 1 && !e->push(Item(0, 2)), "stop drops remainder of an already-spliced batch");
        delete e;
    }
    void detachedStateTest()
    {
        auto *h = new Host;
        auto *e = new Exec;
        e->start(h);
        check(wait([&] { return h->entered.load(std::memory_order_acquire); }), "destruction during startup entered");
        e->push(Item(0, 0));
        delete e;
        h->release();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(h->count.load() == 0, "shared state survives executor destruction and drops pending data");
    }
} // namespace
int main()
{
    fifoAndIdleTest();
    callbackStopTest();
    detachedStateTest();
    std::puts(failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}

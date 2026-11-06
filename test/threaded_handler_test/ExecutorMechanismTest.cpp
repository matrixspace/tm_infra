#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/RealTimeSharedPoolExecutor.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>

using namespace dev::cd606::tm;
namespace
{
    int failures = 0;
    void check(bool value, char const *name)
    {
        std::cout << (value ? "ok: " : "FAIL: ") << name << '\n';
        failures += !value;
    }
    struct Item {
        static int moves, live;
        static bool throwMove;
        int value;
        explicit Item(int v)
            : value(v)
        {
            ++live;
        }
        Item(Item const &) = delete;
        Item &operator=(Item const &) = delete;
        Item(Item &&x)
            : value(x.value)
        {
            if (throwMove) {
                throw std::runtime_error("move");
            }
            ++moves;
            ++live;
        }
        ~Item()
        {
            --live;
        }
    };
    int Item::moves = 0;
    int Item::live = 0;
    bool Item::throwMove = false;
    void queueTest()
    {
        using Q = infra::realtime_threaded_executor_detail::MPSCQueue<Item>;
        {
            Q queue;
            bool called = false;
            check(!queue.tryConsume([&](Item &&) { called = true; }) && !called, "empty queue never invokes consumer");
            Item value(7);
            Item::throwMove = true;
            try {
                queue.push(std::move(value));
                check(false, "throwing move propagated");
            } catch (std::runtime_error const &) {
            }
            Item::throwMove = false;
            check(!queue.maybeNonEmpty(), "throwing enqueue leaves queue unchanged");
            queue.push(std::move(value));
            check(Item::moves == 1, "enqueue constructs move-only non-default item once");
            int observed = 0;
            queue.tryConsume([&](Item &&x) { observed = x.value; });
            check(observed == 7 && Item::moves == 1 && Item::live == 1, "consume in place has no extra move and destroys queued item");
            queue.push(Item(9));
            queue.push(Item(10));
            try {
                queue.tryConsume([](Item &&) { throw std::runtime_error("consume"); });
            } catch (std::runtime_error const &) {
            }
            check(queue.tryConsume([&](Item &&x) { observed = x.value; }) && observed == 10, "throwing consumer leaves valid FIFO stub");
            queue.push(Item(11)); // destructor drops a still-queued item
        }
        check(Item::live == 0, "queue drains/destroys all item lifetimes");
    }
    struct Clock {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = TimePointType::duration;
        static constexpr bool PreserveInputRelativeOrder = false, CanBeActualTimeClock = true;
    };
    using Env = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, Clock>;
    using C = infra::RealTimeAppComponents<Env>;
    int customPushes = 0;
    template <class T>
    struct CustomExecutor : infra::InlineExecutor<T> {
        using Base = infra::InlineExecutor<T>;
        using Base::pushWithHost; // inherited hook must not bypass custom push
        explicit CustomExecutor(infra::ThreadedExecOptions const &o)
            : Base(o)
        {
        }
        bool push(T &&x)
        {
            ++customPushes;
            return Base::push(std::move(x));
        }
    };
    struct CustomPolicy {
        template <class T>
        using Executor = CustomExecutor<T>;
    };
    struct Handler : C::ExecHandler<int, Handler, CustomPolicy> {
        int handled = 0;
        Handler()
        {
            startThread();
        }
        void actuallyHandle(infra::TimedDataWithEnvironment<int, Env> &&)
        {
            ++handled;
        }
    };
    void inlineTest()
    {
        struct Host {
            int sum = 0;
            void executorHandle(int &&x)
            {
                sum += x;
            }
        } host;
        infra::InlineExecutor<int> exec;
        exec.start(&host);
        check(exec.pushWithHost(2, &host) && host.sum == 2, "typed inline path reaches bound host");
        auto legacyPush = &infra::InlineExecutor<int>::push;
        check((exec.*legacyPush)(3) && host.sum == 5, "erased inline contract remains usable");
        exec.stop();
        check(!exec.pushWithHost(7, &host) && !exec.push(11) && host.sum == 5, "both inline paths reject after stop");
        Env env;
        Handler h;
        h.handle({&env, {Clock::TimePointType{}, 1, false}});
        check(customPushes == 1 && h.handled == 1, "generic handler never bypasses a custom synchronous executor");
    }
    struct Signal {
        std::mutex mutex;
        std::condition_variable cond;
        bool entered = false, release = false, done = false;
    };
    using Pool = infra::RealTimeSharedPoolExecutor<int>;
    struct BlockingHost {
        std::shared_ptr<Signal> signal;
        void executorWaitForStart()
        {
        }
        bool executorHasIdleWork()
        {
            return false;
        }
        void executorIdleWork()
        {
        }
        void executorBeforeBatch(int const &, std::size_t)
        {
        }
        void executorHandle(int &&)
        {
            auto s = signal;
            std::unique_lock<std::mutex> lock(s->mutex);
            s->entered = true;
            s->cond.notify_all();
            s->cond.wait(lock, [&] { return s->release; });
            s->done = true;
        }
    };
    void poolDestructionTest()
    {
        auto signal = std::make_shared<Signal>();
        BlockingHost host{signal};
        auto exec = std::make_unique<Pool>();
        exec->start(&host);
        exec->push(1);
        {
            std::unique_lock<std::mutex> lock(signal->mutex);
            bool entered = signal->cond.wait_for(lock, std::chrono::seconds(2), [&] { return signal->entered; });
            check(entered, "pool callback entered before destruction test");
        }
        auto destruction = std::async(std::launch::async, [&] { exec.reset(); });
        check(destruction.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout, "pool destructor waits for an active typed slice");
        {
            std::lock_guard<std::mutex> lock(signal->mutex);
            signal->release = true;
            signal->cond.notify_all();
        }
        destruction.get();
        check(signal->done, "typed slice finishes before pool executor destruction returns");
    }
    struct SelfDestroyHost {
        std::shared_ptr<Signal> signal;
        std::unique_ptr<Pool> *owner;
        void executorWaitForStart()
        {
        }
        bool executorHasIdleWork()
        {
            return false;
        }
        void executorIdleWork()
        {
        }
        void executorBeforeBatch(int const &, std::size_t)
        {
        }
        void executorHandle(int &&)
        {
            auto s = signal;
            owner->reset();
            std::lock_guard<std::mutex> lock(s->mutex);
            s->done = true;
            s->cond.notify_all();
        }
    };
    void poolSelfDestructionTest()
    {
        auto signal = std::make_shared<Signal>();
        auto exec = std::make_unique<Pool>();
        SelfDestroyHost host{signal, &exec};
        exec->start(&host);
        exec->push(1);
        std::unique_lock<std::mutex> lock(signal->mutex);
        bool done = signal->cond.wait_for(lock, std::chrono::seconds(2), [&] { return signal->done; });
        check(done && !exec, "typed slice supports executor destruction inside its callback");
    }
    struct IdleHost {
        std::atomic<bool> wants{false};
        std::atomic<int> handled{0}, idle{0};
        void executorWaitForStart()
        {
        }
        bool executorHasIdleWork()
        {
            return wants.load();
        }
        void executorIdleWork()
        {
            ++idle;
        }
        void executorBeforeBatch(int const &, std::size_t)
        {
        }
        void executorHandle(int &&)
        {
            ++handled;
        }
    };
    template <class F>
    bool eventually(F f)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!f() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return f();
    }
    void poolIdleActivationTest()
    {
        IdleHost host;
        Pool exec;
        exec.start(&host);
        std::thread updater([&] {
            for (int i = 0; i < 1000; ++i) {
                host.wants = (i % 2 == 0);
                exec.wakeIdle();
            }
        });
        for (int i = 0; i < 20000; ++i) {
            exec.push(int(i));
        }
        updater.join();
        host.wants = true;
        exec.wakeIdle();
        check(eventually([&] { return host.handled == 20000 && host.idle > 0; }), "pool idle activation survives concurrent data and wake updates");
        host.wants = false;
        exec.wakeIdle();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        auto before = host.idle.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        check(host.idle == before, "pool removes disabled idle work from active registry");
        host.wants = true;
        exec.wakeIdle();
        check(eventually([&] { return host.idle > before; }), "pool timer resumes after an empty active registry");
        exec.stop();
    }
    void poolIdleMembershipTest()
    {
        std::array<IdleHost, 8> hosts;
        std::array<std::unique_ptr<Pool>, 8> execs;
        for (unsigned i = 0; i < 8; ++i) {
            hosts[i].wants = true;
            execs[i] = std::make_unique<Pool>();
            execs[i]->start(&hosts[i]);
        }
        check(
            eventually([&] {
                for (auto &h : hosts) {
                    if (!h.idle.load()) {
                        return false;
                    }
                }
                return true;
            }),
            "all idle strands initially active");
        // Exercise erase-and-swap at different indices, then reinsert them.
        for (unsigned i : {0, 3, 6, 1}) {
            hosts[i].wants = false;
            execs[i]->wakeIdle();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::array<int, 8> before;
        for (unsigned i = 0; i < 8; ++i) {
            before[i] = hosts[i].idle.load();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bool membership = true;
        for (unsigned i = 0; i < 8; ++i) {
            membership &= hosts[i].wants ? hosts[i].idle > before[i] : hosts[i].idle == before[i];
        }
        check(membership, "active idle index survives removal and compaction");
        for (unsigned i = 0; i < 8; ++i) {
            before[i] = hosts[i].idle.load();
            hosts[i].wants = true;
            execs[i]->wakeIdle();
        }
        check(
            eventually([&] {
                for (unsigned i = 0; i < 8; ++i) {
                    if (hosts[i].idle <= before[i]) {
                        return false;
                    }
                }
                return true;
            }),
            "compacted idle strands can all reactivate");
        for (unsigned i : {2, 5, 1, 7, 0, 6, 3, 4}) {
            execs[i].reset();
        }
    }
    struct StartGateHost {
        std::shared_ptr<Signal> signal;
        std::atomic<int> queries{0}, idle{0};
        void executorWaitForStart()
        {
            std::unique_lock<std::mutex> lock(signal->mutex);
            signal->entered = true;
            signal->cond.notify_all();
            signal->cond.wait(lock, [&] { return signal->release; });
        }
        bool executorHasIdleWork()
        {
            ++queries;
            return true;
        }
        void executorIdleWork()
        {
            ++idle;
        }
        void executorBeforeBatch(int const &, std::size_t)
        {
        }
        void executorHandle(int &&)
        {
        }
    };
    void poolStopDuringStartTest()
    {
        auto signal = std::make_shared<Signal>();
        StartGateHost host{signal};
        auto exec = std::make_unique<Pool>();
        exec->start(&host);
        {
            std::unique_lock<std::mutex> lock(signal->mutex);
            check(signal->cond.wait_for(lock, std::chrono::seconds(2), [&] { return signal->entered; }), "pool start waiter entered");
        }
        exec->stop(); // must not wait for the host mutex held by start
        {
            std::lock_guard<std::mutex> lock(signal->mutex);
            signal->release = true;
            signal->cond.notify_all();
        }
        exec.reset();
        check(host.queries == 0 && host.idle == 0, "stop during start prevents late idle registration");
    }

} // namespace
int main()
{
    infra::SharedThreadPoolConfig config;
    config.threadCount = 2;
    infra::SharedThreadPool::configure(config);
    queueTest();
    inlineTest();
    poolDestructionTest();
    poolSelfDestructionTest();
    poolIdleActivationTest();
    poolIdleMembershipTest();
    poolStopDuringStartTest();
    std::cout << (failures ? "FAILED" : "ALL PASSED") << std::endl;
    return failures ? 1 : 0;
}

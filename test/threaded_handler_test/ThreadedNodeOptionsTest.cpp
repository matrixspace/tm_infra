#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// Per-node ThreadedNodeOptions through RealTimeApp::configureThreadedNode:
//  1. threaded liftPure node: CPU affinity applied to its dedicated thread
//  2. non-threaded node: returns false, nothing changes
//  3. DedicatedThreadExecutor: configure before start applies at start;
//     spinWait accepted and ignored
//  4. executor without configure(): node reports false, no error
//  5. invalid CPU id: false, previous affinity kept
using namespace dev::cd606::tm::infra;
namespace
{
    int failures = 0;
    void check(bool ok, std::string const &name)
    {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", name.c_str());
        failures += !ok;
    }
    template <class F>
    bool waitUntil(F pred)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    struct TestClockComponent {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        TimePointType now() { return std::chrono::system_clock::now(); }
        TimePointType resolveTime() { return now(); }
        TimePointType resolveTime(TimePointType const &) { return now(); }
        void log(LogLevel, std::string const &) {}
    };
    using TP = TestClockComponent::TimePointType;

    // Minimal custom executor without configure(): wraps the default one.
    template <class Item>
    class NoConfigureExecutor {
        DedicatedThreadExecutor<Item> inner_;

    public:
        static constexpr bool kOwnContext = true;
        static constexpr bool kSerialCallbacks = true;
        explicit NoConfigureExecutor(ThreadedExecOptions const &o = ThreadedExecOptions{}) : inner_(o) {}
        template <class Host>
        void start(Host *h) { inner_.start(h); }
        bool push(Item &&x) { return inner_.push(std::move(x)); }
        void stop() { inner_.stop(); }
        void wakeIdle() { inner_.wakeIdle(); }
        std::optional<std::thread::native_handle_type> threadHandle() { return inner_.threadHandle(); }
    };

    using DefaultEnv = Environment<CheckTimeComponent<false>, TrivialExitControlComponent, TestClockComponent>;
    struct NoConfigureEnv : DefaultEnv, RealTimeThreadedExecutorComponent<NoConfigureExecutor> {};

    std::vector<int> affinityOf(std::thread::native_handle_type h)
    {
        std::vector<int> r;
#ifdef __linux__
        cpu_set_t set;
        CPU_ZERO(&set);
        if (pthread_getaffinity_np(h, sizeof(set), &set) == 0) {
            for (int c = 0; c < CPU_SETSIZE; ++c) {
                if (CPU_ISSET(c, &set)) {
                    r.push_back(c);
                }
            }
        }
#endif
        return r;
    }
    // Two CPUs the process may run on, so the test works under taskset.
    std::vector<int> allowedCpus()
    {
        return affinityOf(pthread_self());
    }

    void graphNodeTest()
    {
        using M = RealTimeApp<DefaultEnv>;
        auto *env = new DefaultEnv();
        auto allowed = allowedCpus();
        if (allowed.empty()) {
            check(false, "process affinity readable");
            return;
        }
        int target = allowed.back();

        auto threaded = M::liftPure<int>([](int &&x) { return x + 1; }, LiftParameters<TP>().SuggestThreaded(true));
        auto plain = M::liftPure<int>([](int &&x) { return x; });
        bool cfgOk = M::configureThreadedNode(threaded, ThreadedNodeOptions().Cpus({target}).SpinWait(std::chrono::microseconds(50)));
        auto h = M::threadHandle(threaded);
        check(cfgOk && h.has_value(), "threaded node accepts options and has a thread");
        if (h) {
            auto now = affinityOf(*h);
            check(now.size() == 1 && now[0] == target, "threaded node thread pinned to CPU " + std::to_string(target));
        }
        check(!M::configureThreadedNode(plain, ThreadedNodeOptions().Cpus({target})), "non-threaded node reports false");

        bool bad = M::configureThreadedNode(threaded, ThreadedNodeOptions().Cpus({-1}));
        if (h) {
            auto now = affinityOf(*h);
            check(!bad && now.size() == 1 && now[0] == target, "invalid CPU id rejected, affinity kept");
        }

        // nodes intentionally leaked: their threads are detached
        (void)env;
    }

    struct Host {
        std::atomic<int> handled{0};
        void executorWaitForStart() {}
        void executorHandle(int &&) { ++handled; }
        void executorIdleWork() {}
        bool executorHasIdleWork() { return false; }
        void executorBeforeBatch(int const &, std::size_t) {}
    };

    void dedicatedBeforeStartTest()
    {
        auto allowed = allowedCpus();
        if (allowed.empty()) {
            return;
        }
        int target = allowed.front();
        auto *h = new Host;
        auto *e = new DedicatedThreadExecutor<int>;
        bool ok = e->configure(ThreadedNodeOptions().Cpus({target}).SpinWait(std::chrono::milliseconds(1)));
        e->start(h);
        auto th = e->threadHandle();
        auto now = th ? affinityOf(*th) : std::vector<int>{};
        check(ok && now.size() == 1 && now[0] == target, "dedicated: affinity set before start applied at start");
        e->push(1);
        check(waitUntil([&] { return h->handled.load() == 1; }), "dedicated: spinWait accepted and ignored, data flows");
        e->stop();
    }

    void noConfigureExecutorTest()
    {
        using M = RealTimeApp<NoConfigureEnv>;
        auto threaded = M::liftPure<int>([](int &&x) { return x; }, LiftParameters<TP>().SuggestThreaded(true));
        check(!M::configureThreadedNode(threaded, ThreadedNodeOptions().Cpus({0})), "executor without configure(): false, no error");
        check(M::threadHandle(threaded).has_value(), "executor without configure(): node still threaded");
    }
} // namespace

int main()
{
    graphNodeTest();
    dedicatedBeforeStartTest();
    noConfigureExecutorTest();
    std::puts(failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}

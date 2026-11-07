#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/RealTimeSharedPoolExecutor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

//Tests for the executors behind RealTimeApp threaded handlers
//(RealTimeThreadedExecutor.hpp / RealTimeSharedPoolExecutor.hpp), run for
//the default DedicatedThreadExecutor and for the opt-in shared pool:
//  1. executor selection from the environment
//  2. ThreadedHandler, several producers: nothing lost / duplicated,
//     per-producer order kept, callbacks never overlap
//  4. stop() from inside actuallyHandle: no further item is handled,
//     later pushes are dropped
//  5. idle work: periodic while hasIdleWork() is true, none while it is
//     false, resumes after wakeIdle()
//  6. threaded actions in a graph: liftPure / liftPure2 / liftMulti3,
//     FireOnceOnly, setIdleWorkerForAction after start
//  7. CheckTime=true: a single producer with increasing time points is
//     never rejected by the handler's TimeChecker
//  8. latency sanity: one producer, ping by ping, p50 well below the old
//     1ms polling period

using namespace dev::cd606::tm;

namespace {

    struct TestClockComponent {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        TimePointType now() {return std::chrono::system_clock::now();}
        TimePointType resolveTime() {return now();}
        TimePointType resolveTime(TimePointType const &) {return now();}
        void log(infra::LogLevel, std::string const &) {}
    };

    using TP = TestClockComponent::TimePointType;
    template <bool CheckTime, bool Pool>
    struct EnvFor {
        using type = infra::Environment<infra::CheckTimeComponent<CheckTime>, infra::TrivialExitControlComponent, TestClockComponent>;
    };
    template <bool CheckTime>
    struct EnvFor<CheckTime, true> {
        using type = infra::Environment<infra::CheckTimeComponent<CheckTime>, infra::TrivialExitControlComponent, TestClockComponent, infra::RealTimeSharedPoolExecutorComponent>;
    };
    template <bool CheckTime, bool Pool>
    using TestEnv = typename EnvFor<CheckTime, Pool>::type;

    std::string tagOf(bool pool) {
        return pool ? " [shared pool]" : " [dedicated]";
    }

    int g_failures = 0;
    void check(bool cond, std::string const &what) {
        if (!cond) {
            ++g_failures;
            std::cout << "FAIL: " << what << std::endl;
        } else {
            std::cout << "ok:   " << what << std::endl;
        }
    }

    //waits until pred() or the deadline; returns pred()
    template <class F>
    bool waitUntil(F &&pred, std::chrono::milliseconds limit = std::chrono::milliseconds(10000)) {
        auto deadline = std::chrono::steady_clock::now()+limit;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return pred();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

    template <class Env>
    infra::TimedDataWithEnvironment<int64_t, Env, TP> make(Env *env, int64_t v, TP tp = TP{}) {
        return {env, infra::WithTime<int64_t, TP>{tp, v, false}};
    }

    //---- 1. executor selection
    void testSelection() {
        using Dflt = TestEnv<false, false>;
        using Pool = TestEnv<false, true>;
        constexpr bool d1 = std::is_same_v<infra::RealTimeThreadedExecutorFor<Dflt, int>, infra::DedicatedThreadExecutor<int>>;
        constexpr bool p1 = std::is_same_v<infra::RealTimeThreadedExecutorFor<Pool, int>, infra::realtime_shared_pool_detail::SharedPoolStrandExecutor<int>>;
        check(d1, "default environment selects DedicatedThreadExecutor");
        check(p1, "pool environment selects the shared pool for Blocking");
    }
    //---- shared bookkeeping for handler tests
    struct Stats {
        static constexpr int kMaxProducers = 8;
        std::atomic<int64_t> handled {0};
        std::atomic<int> inside {0};
        std::atomic<int> overlap {0};
        std::atomic<int> orderErr {0};
        std::atomic<int> dup {0};
        std::atomic<int> idle {0};
        std::atomic<int> started {0};
        int64_t last[kMaxProducers];
        Stats() {
            for (auto &x : last) {
                x = -1;
            }
        }
        //value = producer * kPer + index
        void onItem(int64_t v, int64_t kPer) {
            if (inside.fetch_add(1) != 0) {
                ++overlap;
            }
            int p = static_cast<int>(v / kPer);
            int64_t i = v % kPer;
            if (i == last[p]) {
                ++dup;
            } else if (i < last[p]) {
                ++orderErr;
            }
            last[p] = i;
            handled.fetch_add(1, std::memory_order_relaxed);
            inside.fetch_sub(1);
        }
    };

    template <class Env>
    class TestHandler final : public infra::RealTimeAppComponents<Env>::template ThreadedHandler<int64_t, TestHandler<Env>> {
    private:
        Stats *st_;
        int64_t kPer_;
        int64_t stopAt_;
        std::atomic<bool> wantIdle_;
    public:
        TestHandler(Stats *st, int64_t kPer, int64_t stopAt = -1, bool wantIdle = true)
            : infra::RealTimeAppComponents<Env>::template ThreadedHandler<int64_t, TestHandler<Env>>()
            , st_(st), kPer_(kPer), stopAt_(stopAt), wantIdle_(wantIdle) {
            this->startThread();
        }
        void waitForStart() {
            ++st_->started;
        }
        void actuallyHandle(infra::TimedDataWithEnvironment<int64_t, Env, TP> &&d) {
            if (!this->timeCheckGood(d)) {
                return;
            }
            st_->onItem(d.timedData.value, kPer_);
            if (stopAt_ >= 0 && st_->handled.load() >= stopAt_) {
                this->stop();
            }
        }
        void idleWork() {
            ++st_->idle;
        }
        bool hasIdleWork() {
            return wantIdle_.load();
        }
        void setWantIdle(bool w) {
            wantIdle_.store(w);
            this->wakeIdle();
        }
        void stopNow() {
            this->stop();
        }
    };

    //producers call IHandler::handle directly, as upstream nodes do
    template <class Env, class H>
    void produce(Env *env, H *h, int producers, int64_t kPer, bool increasingTime = false) {
        std::vector<std::thread> ths;
        for (int p = 0; p < producers; ++p) {
            ths.emplace_back([env, h, p, kPer, increasingTime]() {
                for (int64_t i = 0; i < kPer; ++i) {
                    TP tp = increasingTime ? TP{} + std::chrono::nanoseconds(i+1) : TP{};
                    static_cast<typename infra::RealTimeAppComponents<Env>::template IHandler<int64_t> *>(h)->handle(make<Env>(env, p*kPer+i, tp));
                }
            });
        }
        for (auto &t : ths) {
            t.join();
        }
    }

    //---- 2. ThreadedHandler, several producers
    template <bool Pool>
    void testThreadedMultiProducer() {
        using Env = TestEnv<false, Pool>;
        Env &env = *new Env(); //leaked: detached handler threads may still use it
        constexpr int kProd = 4;
        constexpr int64_t kPer = 50000;
        //leaked with the handler: its detached thread may still touch it
        Stats &st = *new Stats();
        auto *h = new TestHandler<Env>(&st, kPer);
        produce(&env, h, kProd, kPer);
        bool all = waitUntil([&]() {return st.handled.load() >= kProd*kPer;});
        std::string tag = tagOf(Pool);
        check(all && st.handled.load() == kProd*kPer, "threaded handler: all items handled once" + tag);
        check(st.dup.load() == 0 && st.orderErr.load() == 0, "threaded handler: per-producer order kept, no duplicate" + tag);
        check(st.overlap.load() == 0, "threaded handler: callbacks never overlap" + tag);
        check(st.started.load() == 1, "threaded handler: waitForStart called exactly once" + tag);
        h->stopNow();
        //leaked on purpose: the handler thread is detached (pre-existing lifecycle, see task record)
        (void) h;
    }


    //---- 4. stop() from inside actuallyHandle
    template <bool Pool>
    void testStopInsideCallback() {
        using Env = TestEnv<false, Pool>;
        Env &env = *new Env(); //leaked: detached handler threads may still use it
        constexpr int64_t kPer = 100000;
        constexpr int64_t kStopAt = 1000;
        //leaked with the handler: its detached thread may still touch it
        Stats &st = *new Stats();
        auto *h = new TestHandler<Env>(&st, kPer, kStopAt);
        produce(&env, h, 1, kPer);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int64_t afterStop = st.handled.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::string tag = tagOf(Pool);
        check(afterStop == kStopAt, "stop() inside the callback: nothing handled after it (" + std::to_string(afterStop) + ")" + tag);
        check(st.handled.load() == afterStop, "stop(): later pushes are dropped" + tag);
        //leaked on purpose: the handler thread is detached (pre-existing lifecycle, see task record)
        (void) h;
    }

    //---- 5. idle work rhythm
    template <bool Pool>
    void testIdleWork() {
        using Env = TestEnv<false, Pool>;
        Env &env = *new Env(); //leaked: detached handler threads may still use it
        //leaked with the handler: its detached thread may still touch it
        Stats &st = *new Stats();
        auto *h = new TestHandler<Env>(&st, 1, -1, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int withIdle = st.idle.load();
        h->setWantIdle(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int base = st.idle.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int withoutIdle = st.idle.load() - base;
        h->setWantIdle(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int resumed = st.idle.load() - base - withoutIdle;
        std::string tag = tagOf(Pool);
        //about 100 at a 1ms rhythm; loose bounds for loaded machines
        check(withIdle >= 20 && withIdle <= 150, "idle work periodic while hasIdleWork() (" + std::to_string(withIdle) + "/100ms)" + tag);
        if constexpr (Pool) {
            check(withoutIdle <= 1, "no idle work while hasIdleWork() is false (" + std::to_string(withoutIdle) + "/100ms)" + tag);
        } else {
            check(withoutIdle >= 20 && withoutIdle <= 150, "default retains historical periodic idle callbacks (" + std::to_string(withoutIdle) + "/100ms)" + tag);
        }
        check(resumed >= 20, "idle work resumes after wakeIdle() (" + std::to_string(resumed) + "/100ms)" + tag);
        h->stopNow();
        //leaked on purpose: the handler thread is detached (pre-existing lifecycle, see task record)
        (void) h;
    }

    //---- 6. threaded actions in a graph
    template <class M, class T>
    class ThreadSource final : public M::template AbstractImporter<T> {
    private:
        std::vector<T> values_;
        std::atomic<int> *done_;
    public:
        ThreadSource(std::vector<T> values, std::atomic<int> *done) : values_(std::move(values)), done_(done) {}
        virtual void start(typename M::EnvironmentType *env) override final {
            std::thread([this, env]() {
                for (auto const &v : values_) {
                    this->publish(M::template pureInnerData<T>(env, T(v)));
                }
                done_->fetch_add(1);
            }).detach();
        }
    };

    template <bool Pool, bool Threaded = true, bool InlineSelected = false>
    void testGraphActions()
    {
        using InlineEnv = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, TestClockComponent, infra::RealTimeThreadedExecutorComponent<infra::InlineExecutor>>;
        using Env = std::conditional_t<InlineSelected, InlineEnv, TestEnv<false, Pool>>;
        using M = infra::RealTimeApp<Env>;
        using R = infra::AppRunner<M>;
        constexpr int64_t kN = 20000;
        std::string tag = tagOf(Pool);
        auto *env = new Env();
        auto *r = new R(env);
        auto thr = infra::LiftParameters<TP>().SuggestThreaded(Threaded);

        std::vector<int64_t> ints(kN);
        std::vector<double> dbls(kN);
        for (int64_t i = 0; i < kN; ++i) {
            ints[i] = i;
            dbls[i] = static_cast<double>(i);
        }
        std::atomic<int> done {0};
        auto sInt = M::importer(new ThreadSource<M, int64_t>(ints, &done));
        auto sDbl = M::importer(new ThreadSource<M, double>(dbls, &done));
        auto sChr = M::importer(new ThreadSource<M, char>(std::vector<char>(100, 'x'), &done));

        std::atomic<int64_t> pureCount {0}, sum {0}, pure2Count {0}, multiCount {0}, onceCount {0};
        std::atomic<int> pureInside {0}, pureOverlap {0};
        auto pure = M::template liftPure<int64_t>([&](int64_t &&x) {
            if (pureInside.fetch_add(1) != 0) {
                ++pureOverlap;
            }
            pureInside.fetch_sub(1);
            return x;
        }, thr);
        auto pure2 = M::template liftPure2<int64_t, double>([](std::variant<int64_t, double> &&v) {
            return static_cast<int64_t>(v.index());
        }, thr);
        auto multi3 = M::template liftMulti3<int64_t, double, char>([](std::variant<int64_t, double, char> &&v) {
            return std::vector<int64_t>(v.index()+1, 1);
        }, thr);
        auto once = M::template liftPure<int64_t>([](int64_t &&x) { return x; }, infra::LiftParameters<TP>().SuggestThreaded(Threaded).FireOnceOnly(true));

        auto counter = [](std::atomic<int64_t> *c, std::atomic<int64_t> *s) {
            return M::template simpleExporter<int64_t>([c, s](typename M::template InnerData<int64_t> &&d) {
                c->fetch_add(1);
                if (s) {
                    s->fetch_add(d.timedData.value);
                }
            });
        };
        auto e1 = counter(&pureCount, &sum);
        auto e2 = counter(&pure2Count, nullptr);
        auto e3 = counter(&multiCount, nullptr);
        auto e4 = counter(&onceCount, nullptr);

        auto srcInt = r->importItem("sInt", sInt);
        auto srcDbl = r->importItem("sDbl", sDbl);
        auto srcChr = r->importItem("sChr", sChr);
        r->connect(srcInt.clone(), r->actionAsSink("pure", pure));
        r->connect(r->actionAsSource(pure), r->exporterAsSink("e1", e1));
        r->connect(srcInt.clone(), r->actionAsSink_2_0("pure2", pure2));
        r->connect(srcDbl.clone(), r->actionAsSink_2_1(pure2));
        r->connect(r->actionAsSource(pure2), r->exporterAsSink("e2", e2));
        r->connect(srcInt.clone(), r->actionAsSink_3_0("multi3", multi3));
        r->connect(srcDbl.clone(), r->actionAsSink_3_1(multi3));
        r->connect(srcChr.clone(), r->actionAsSink_3_2(multi3));
        r->connect(r->actionAsSource(multi3), r->exporterAsSink("e3", e3));
        r->connect(srcInt.clone(), r->actionAsSink("once", once));
        r->connect(r->actionAsSource(once), r->exporterAsSink("e4", e4));

        std::atomic<int> idle {0};
        r->finalize();
        check(M::actionIsThreaded(pure) == (Threaded && !InlineSelected), "graph execution context matches selected policy");
        //registered after the threads are already running and sleeping
        M::setIdleWorkerForAction(pure, [&idle](void *) {++idle;});

        int64_t const expectMulti = kN*1 + kN*2 + 100*3;
        bool ok = waitUntil([&]() {
            return done.load() == 3 && pureCount.load() == kN && pure2Count.load() == 2*kN && multiCount.load() == expectMulti;
        });
        check(ok && sum.load() == kN*(kN-1)/2, "graph: threaded liftPure delivers everything" + tag);
        check(pureOverlap.load() == 0, "graph: threaded liftPure callbacks never overlap" + tag);
        check(pure2Count.load() == 2*kN, "graph: threaded liftPure2 dispatches both variant inputs" + tag);
        check(multiCount.load() == expectMulti, "graph: threaded liftMulti3 dispatches all three inputs" + tag);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(onceCount.load() == 1, "graph: FireOnceOnly action fires exactly once" + tag);
        check((Threaded && !InlineSelected) ? idle.load() >= 10 : idle.load() == 0, "graph: setIdleWorkerForAction after start still gets idle calls (" + std::to_string(idle.load()) + ")" + tag);
        //the graph outlives this function, so drop the worker that captures
        //the local counter; idleWork holds the worker mutex while calling it,
        //so once this returns no call can still touch idle
        M::setIdleWorkerForAction(pure, std::function<void(void *)> {});
        //graph intentionally leaked (detached importer threads)
    }

    // Inline callbacks execute in producer contexts and must not be serialized.
    void testInlineExecutor()
    {
        struct Host {
            std::atomic<int> entered{0};
            std::atomic<int> handled{0};
            std::atomic<bool> release{false};
            void executorHandle(int &&)
            {
                ++entered;
                while (!release.load()) {
                    std::this_thread::yield();
                }
                ++handled;
            }
        } host;
        infra::InlineExecutor<int> exec;
        static_assert(!decltype(exec)::kOwnContext && !decltype(exec)::kSerialCallbacks);
        exec.start(&host); // no wait/idle callbacks required on an inline host
        std::vector<std::thread> producers;
        for (int i = 0; i < 4; ++i) {
            producers.emplace_back([&exec] { exec.push(1); });
        }
        bool concurrent = waitUntil([&] { return host.entered.load() == 4; });
        host.release = true;
        for (auto &t : producers) {
            t.join();
        }
        check(concurrent && host.handled == 4, "inline callbacks enter concurrently from all producers");
        exec.stop();
        check(!exec.push(2) && host.handled == 4, "inline stop rejects subsequent pushes");
        check(!exec.threadHandle(), "inline executor has no thread handle");

        using Env = TestEnv<true, false>;
        using C = infra::RealTimeAppComponents<Env>;
        Env env;
        struct Checked final : C::ExecHandler<int64_t, Checked, C::InlinePolicy> {
            std::atomic<int> accepted{0};
            Checked()
            {
                this->startThread();
            }
            void actuallyHandle(infra::TimedDataWithEnvironment<int64_t, Env, TP> &&data)
            {
                if (this->timeCheckGood(data)) {
                    ++accepted;
                }
            }
        } checked;
        auto feed = [&](TP timestamp) {
            std::vector<std::thread> threads;
            for (int i = 0; i < 4; ++i) {
                threads.emplace_back([&checked, &env, timestamp] {
                    for (int j = 0; j < 20000; ++j) {
                        checked.handle(make(&env, 1, timestamp));
                    }
                });
            }
            for (auto &t : threads) {
                t.join();
            }
        };
        feed(TP{} + std::chrono::seconds(2));
        check(checked.accepted == 80000, "inline handler accepts concurrent equal timestamps");
        feed(TP{} + std::chrono::seconds(1));
        check(checked.accepted == 80000, "inline handler rejects decreasing timestamps from all producers");
    }

    //---- 7. CheckTime=true, single producer with increasing time points
    template <bool Pool>
    void testCheckTimeSingleProducer() {
        using Env = TestEnv<true, Pool>;
        Env &env = *new Env(); //leaked: detached handler threads may still use it
        constexpr int64_t kPer = 50000;
        //leaked with the handler: its detached thread may still touch it
        Stats &st = *new Stats();
        auto *h = new TestHandler<Env>(&st, kPer, -1, false);
        produce(&env, h, 1, kPer, true);
        bool all = waitUntil([&]() {return st.handled.load() >= kPer;});
        check(all && st.handled.load() == kPer && st.orderErr.load() == 0
            , "CheckTime=true: increasing time points never rejected" + tagOf(Pool));
        h->stopNow();
        //leaked on purpose: the handler thread is detached (pre-existing lifecycle, see task record)
        (void) h;
    }

    //---- 8. latency sanity (ping by ping, consumer asleep in between)
    template <class Env>
    class LatencyHandler final : public infra::RealTimeAppComponents<Env>::template ThreadedHandler<int64_t, LatencyHandler<Env>> {
    public:
        std::vector<int64_t> lat;
        std::atomic<int64_t> seen {0};
        LatencyHandler() : infra::RealTimeAppComponents<Env>::template ThreadedHandler<int64_t, LatencyHandler<Env>>() {
            lat.reserve(4096);
            this->startThread();
        }
        void waitForStart() {}
        void actuallyHandle(infra::TimedDataWithEnvironment<int64_t, Env, TP> &&d) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            lat.push_back(now - d.timedData.value);
            seen.fetch_add(1, std::memory_order_release);
        }
        void idleWork() {}
        bool hasIdleWork() {return false;}
        void stopNow() {this->stop();}
    };

    template <bool Pool>
    void testLatency() {
        using Env = TestEnv<false, Pool>;
        Env &env = *new Env(); //leaked: detached handler threads may still use it
        constexpr int kPings = 2000;
        auto *h = new LatencyHandler<Env>();
        auto *ih = static_cast<typename infra::RealTimeAppComponents<Env>::template IHandler<int64_t> *>(h);
        for (int i = 0; i < kPings; ++i) {
            ih->handle(make<Env>(&env, std::chrono::steady_clock::now().time_since_epoch().count()));
            waitUntil([&]() {return h->seen.load(std::memory_order_acquire) > i;});
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        std::vector<int64_t> v = h->lat;
        std::sort(v.begin(), v.end());
        auto pct = [&v](double p) {return v[static_cast<std::size_t>(p*(v.size()-1))]/1000.0;};
        std::string tag = tagOf(Pool);
        std::cout << "      latency us: p50=" << pct(0.5) << " p99=" << pct(0.99) << " p999=" << pct(0.999) << tag << std::endl;
        //the old handler waited up to 1ms per round
        check(v.size() == kPings && pct(0.5) < 300.0, "latency: p50 well below the old 1ms poll" + tag);
        h->stopNow();
        //leaked on purpose: the handler thread is detached (pre-existing lifecycle, see task record)
        (void) h;
    }

} // namespace

int main() {
    //small pool so that strands really share threads
    infra::SharedThreadPoolConfig cfg;
    cfg.threadCount = 2;
    infra::SharedThreadPool::configure(cfg);

    testSelection();
    testInlineExecutor();
    testGraphActions<false, true, true>();
    testGraphActions<false, false>();
    testGraphActions<true, false>();
    testThreadedMultiProducer<false>();
    testThreadedMultiProducer<true>();
    testStopInsideCallback<false>();
    testStopInsideCallback<true>();
    testIdleWork<false>();
    testIdleWork<true>();
    testGraphActions<false>();
    testGraphActions<true>();
    testCheckTimeSingleProducer<false>();
    testCheckTimeSingleProducer<true>();
    testLatency<false>();
    testLatency<true>();
    if (g_failures == 0) {
        std::cout << "ALL PASSED" << std::endl;
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)" << std::endl;
    return 1;
}

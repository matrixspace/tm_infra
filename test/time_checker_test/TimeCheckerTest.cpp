#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/VersionedData.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

//Tests for the multi-threaded TimeChecker<true,T> used by non-threaded nodes,
//run for both the default environment (always the original mutex checker)
//and an environment with LockFreeTimeCheckerComponent (opt-in):
//  1. the chosen implementation (Trivial / AtomicCAS / Mutex) per input type,
//     CheckTime setting and opt-in
//  2. single-threaded equivalence with the original mutex rule on random
//     time sequences
//  3. several producer threads feeding one checker: nothing lost when
//     CheckTime=false; when CheckTime=true every thread's accepted sequence
//     is monotonic, nothing older than the final accepted maximum is
//     accepted after it, and the totals add up
//  4. a real graph: two importer threads -> one simpleExporter

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

    template <bool CheckTime, bool LockFree>
    struct TestEnvFor {
        using type = infra::Environment<infra::CheckTimeComponent<CheckTime>, infra::TrivialExitControlComponent, TestClockComponent>;
    };
    template <bool CheckTime>
    struct TestEnvFor<CheckTime, true> {
        using type = infra::Environment<infra::CheckTimeComponent<CheckTime>, infra::TrivialExitControlComponent, TestClockComponent, infra::LockFreeTimeCheckerComponent>;
    };
    template <bool CheckTime, bool LockFree>
    using TestEnv = typename TestEnvFor<CheckTime, LockFree>::type;

    std::string tagOf(bool checkTime, bool lockFree) {
        return std::string(" CheckTime=")+(checkTime ? "true" : "false")+(lockFree ? " lock-free" : " default");
    }

    using TP = std::chrono::system_clock::time_point;
    using Versioned = infra::VersionedData<int64_t, int64_t>;

    int g_failures = 0;
    void check(bool cond, std::string const &what) {
        if (!cond) {
            ++g_failures;
            std::cout << "FAIL: " << what << std::endl;
        } else {
            std::cout << "ok:   " << what << std::endl;
        }
    }

    template <class Env, class T>
    using TC = typename infra::RealTimeAppComponents<Env>::template TimeChecker<true, T>;
    template <class Env, class T>
    constexpr auto modeOf() {
        return infra::RealTimeAppComponents<Env>::template TimeCheckerModeFor<T>::value;
    }
    template <class Env>
    using Mode = typename infra::RealTimeAppComponents<Env>::TimeCheckerMode;

    template <class Env, class T>
    infra::TimedDataWithEnvironment<T, Env, TP> make(Env *env, TP tp, T v) {
        return {env, infra::WithTime<T, TP>{tp, std::move(v), false}};
    }

    //reference: the pre-change rule
    struct RefChecker {
        bool hasData = false;
        TP last{};
        bool operator()(TP t, bool checkTime) {
            if (checkTime && hasData && t < last) {
                return false;
            }
            hasData = true;
            last = t;
            return true;
        }
    };

    void testModes() {
        //default environment: always the original mutex checker
        static_assert(modeOf<TestEnv<false,false>, int64_t>() == Mode<TestEnv<false,false>>::Mutex);
        static_assert(modeOf<TestEnv<true,false>, int64_t>() == Mode<TestEnv<true,false>>::Mutex);
        static_assert(modeOf<TestEnv<false,false>, std::string>() == Mode<TestEnv<false,false>>::Mutex);
        static_assert(modeOf<TestEnv<false,false>, Versioned>() == Mode<TestEnv<false,false>>::Mutex);
        //opt-in: lock-free unless the input has version state
        static_assert(modeOf<TestEnv<false,true>, int64_t>() == Mode<TestEnv<false,true>>::Trivial);
        static_assert(modeOf<TestEnv<true,true>, int64_t>() == Mode<TestEnv<true,true>>::AtomicCAS);
        static_assert(modeOf<TestEnv<false,true>, std::string>() == Mode<TestEnv<false,true>>::Trivial);
        static_assert(modeOf<TestEnv<false,true>, Versioned>() == Mode<TestEnv<false,true>>::Mutex);
        static_assert(modeOf<TestEnv<true,true>, Versioned>() == Mode<TestEnv<true,true>>::Mutex);
        static_assert(modeOf<TestEnv<false,true>, std::shared_ptr<Versioned>>() == Mode<TestEnv<false,true>>::Mutex);
        static_assert(modeOf<TestEnv<false,true>, std::tuple<int, Versioned>>() == Mode<TestEnv<false,true>>::Mutex);
        check(true, "implementation selection, default -> Mutex, opt-in -> Trivial/AtomicCAS (static_asserts)");
    }

    template <bool CheckTime, bool LockFree>
    void testSingleThreadEquivalence() {
        using Env = TestEnv<CheckTime, LockFree>;
        Env env;
        std::mt19937_64 rng(12345);
        bool same = true;
        for (int round=0; round<200 && same; ++round) {
            TC<Env, int64_t> tc;
            RefChecker ref;
            for (int ii=0; ii<1000; ++ii) {
                //small range so that equal and older time points are frequent
                TP t = TP{} + std::chrono::nanoseconds(static_cast<int64_t>(rng()%50));
                bool a = tc(make<Env, int64_t>(&env, t, ii));
                bool b = ref(t, CheckTime);
                if (a != b) {
                    same = false;
                    break;
                }
            }
            same = same && tc.good();
        }
        check(same, "single-thread equivalence with the original rule"+tagOf(CheckTime, LockFree));
    }

    template <bool CheckTime, bool LockFree>
    void testMultiThread() {
        using Env = TestEnv<CheckTime, LockFree>;
        Env env;
        TC<Env, int64_t> tc;
        constexpr int kThreads = 4;
        constexpr int kPerThread = 500000;
        std::atomic<int64_t> clockCounter{0};
        std::atomic<bool> go{false};
        std::vector<std::vector<int64_t>> accepted(kThreads);
        std::vector<int64_t> rejected(kThreads, 0);
        std::vector<std::thread> threads;
        for (int k=0; k<kThreads; ++k) {
            accepted[k].reserve(kPerThread);
            threads.emplace_back([&, k]() {
                while (!go.load(std::memory_order_acquire)) {
                }
                for (int ii=0; ii<kPerThread; ++ii) {
                    //globally increasing time source, but threads race between
                    //taking the time and handing it to the checker, so the
                    //checker sees out-of-order time points
                    int64_t ns = clockCounter.fetch_add(1, std::memory_order_relaxed);
                    if (tc(make<Env, int64_t>(&env, TP{} + std::chrono::nanoseconds(ns), ns))) {
                        accepted[k].push_back(ns);
                    } else {
                        ++rejected[k];
                    }
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto &t : threads) {
            t.join();
        }
        int64_t total = 0, totalRejected = 0;
        bool perThreadMonotonic = true;
        int64_t maxAccepted = -1;
        for (int k=0; k<kThreads; ++k) {
            total += static_cast<int64_t>(accepted[k].size());
            totalRejected += rejected[k];
            for (std::size_t ii=1; ii<accepted[k].size(); ++ii) {
                if (accepted[k][ii] < accepted[k][ii-1]) {
                    perThreadMonotonic = false;
                }
            }
            if (!accepted[k].empty()) {
                maxAccepted = std::max(maxAccepted, accepted[k].back());
            }
        }
        std::string tag = tagOf(CheckTime, LockFree);
        check(total+totalRejected == int64_t(kThreads)*kPerThread, "accepted+rejected == sent" + tag);
        check(tc.good(), "good() after data" + tag);
        if (!CheckTime) {
            check(totalRejected == 0, "nothing rejected without time check" + tag);
        } else {
            check(perThreadMonotonic, "per-thread accepted time points are monotonic" + tag);
            //once the maximum has been accepted, a later call with an older time
            //point must be rejected
            bool afterMax = tc(make<Env, int64_t>(&env, TP{} + std::chrono::nanoseconds(maxAccepted-1), 0));
            check(!afterMax, "older than the accepted maximum is rejected afterwards" + tag);
            bool equalMax = tc(make<Env, int64_t>(&env, TP{} + std::chrono::nanoseconds(maxAccepted), 0));
            check(equalMax, "equal to the accepted maximum is accepted" + tag);
            std::cout << "      (" << totalRejected << " of " << kThreads*kPerThread << " rejected as out of order)" << std::endl;
        }
    }

    template <bool LockFree>
    void testVersionedStillChecked() {
        using Env = TestEnv<false, LockFree>;
        Env env;
        TC<Env, Versioned> tc;
        TP t{};
        bool a = tc(make<Env, Versioned>(&env, t, Versioned{5, 0}));
        bool b = tc(make<Env, Versioned>(&env, t, Versioned{4, 0}));
        bool c = tc(make<Env, Versioned>(&env, t, Versioned{6, 0}));
        check(a && !b && c, "VersionedData goes through the version check (mutex path)"+tagOf(false, LockFree));
    }

    //graph level: two importer threads, one non-threaded simpleExporter
    template <class M>
    class ThreadSource final : public M::template AbstractImporter<int64_t> {
    private:
        int64_t n_;
        std::atomic<int> *done_;

    public:
        ThreadSource(int64_t n, std::atomic<int> *done)
            : n_(n)
            , done_(done) {}
        virtual void start(typename M::EnvironmentType *env) override final {
            std::thread([this, env]() {
                for (int64_t ii=0; ii<n_; ++ii) {
                    this->publish(M::template pureInnerData<int64_t>(env, int64_t(ii)));
                }
                done_->fetch_add(1);
            }).detach();
        }
    };

    template <bool CheckTime, bool LockFree>
    void testGraph() {
        using Env = TestEnv<CheckTime, LockFree>;
        using M = infra::RealTimeApp<Env>;
        using R = infra::AppRunner<M>;
        constexpr int64_t kN = 200000;
        auto *env = new Env();
        auto *r = new R(env);
        std::atomic<int> done{0};
        std::atomic<int64_t> received{0};
        auto s1 = M::importer(new ThreadSource<M>(kN, &done));
        auto s2 = M::importer(new ThreadSource<M>(kN, &done));
        auto sink = M::template simpleExporter<int64_t>([&received](typename M::template InnerData<int64_t> &&) {
            received.fetch_add(1, std::memory_order_relaxed);
        });
        auto sinkS = r->exporterAsSink("sink", sink);
        r->connect(r->importItem("s1", s1), sinkS);
        r->connect(r->importItem("s2", s2), sinkS);
        r->finalize();
        while (done.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::string tag = tagOf(CheckTime, LockFree);
        if (!CheckTime) {
            check(received.load() == 2*kN, "graph: two importer threads -> simpleExporter, nothing lost" + tag);
        } else {
            //real clock time points from two threads: some may be dropped as
            //out of order, but at least one full thread's worth gets through
            //in the common case and never more than sent
            check(received.load() > 0 && received.load() <= 2*kN, "graph: two importer threads -> simpleExporter, sane count" + tag);
            std::cout << "      (" << received.load() << " of " << 2*kN << " delivered)" << std::endl;
        }
        //graph intentionally leaked (detached importer threads already finished)
    }

} // namespace

int main() {
    using DefaultEnv = infra::Environment<infra::TrivialExitControlComponent, TestClockComponent>;
    static_assert(!infra::CheckTimeComponent<>::CheckTime);
    static_assert(!infra::CheckTimeEnabled<DefaultEnv>::value);
    static_assert(infra::CheckTimeEnabled<TestEnv<true, false>>::value);
    TC<DefaultEnv, int> defaultChecker;
    auto now = std::chrono::system_clock::now();
    check(defaultChecker({nullptr, {now, 1, false}})
        && defaultChecker({nullptr, {now-std::chrono::seconds(1), 2, false}}),
        "missing time policy accepts decreasing timestamps");

    TC<DefaultEnv, Versioned> defaultVersionChecker;
    DefaultEnv defaultEnv;
    check(defaultVersionChecker(make<DefaultEnv, Versioned>(&defaultEnv, now, Versioned{5, 0}))
        && !defaultVersionChecker(make<DefaultEnv, Versioned>(&defaultEnv, now, Versioned{4, 0})),
        "default policy preserves version checks");
    testModes();
    testSingleThreadEquivalence<false,false>();
    testSingleThreadEquivalence<true,false>();
    testSingleThreadEquivalence<false,true>();
    testSingleThreadEquivalence<true,true>();
    testMultiThread<false,false>();
    testMultiThread<true,false>();
    testMultiThread<false,true>();
    testMultiThread<true,true>();
    testVersionedStillChecked<false>();
    testVersionedStillChecked<true>();
    testGraph<false,false>();
    testGraph<true,false>();
    testGraph<false,true>();
    testGraph<true,true>();
    if (g_failures == 0) {
        std::cout << "ALL PASSED" << std::endl;
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)" << std::endl;
    return 1;
}

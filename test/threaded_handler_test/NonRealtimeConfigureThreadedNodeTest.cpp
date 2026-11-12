// Verifies that M::configureThreadedNode compiles and silently returns false
// for non-realtime App types (SinglePassIterationApp, TopDownSinglePassIterationApp,
// BasicWithTimeApp), so that code shared between realtime and non-realtime builds
// can call configureThreadedNode without conditional compilation.
#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/SinglePassIterationApp.hpp>
#include <tm_kit/infra/TopDownSinglePassIterationApp.hpp>
#include <tm_kit/infra/BasicWithTimeApp.hpp>
#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace
{
    int failures = 0;
    void check(bool ok, std::string const &name)
    {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", name.c_str());
        failures += !ok;
    }

    // SinglePassIterationApp and TopDownSinglePassIterationApp require
    // StateT::PreserveInputRelativeOrder == true.
    struct SinglePassClock {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType  = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = true;
        static constexpr bool CanBeActualTimeClock = false;
        TimePointType now() { return std::chrono::system_clock::now(); }
        TimePointType resolveTime() { return now(); }
        TimePointType resolveTime(TimePointType const &t) { return t; }
        void log(dev::cd606::tm::infra::LogLevel, std::string const &) {}
    };

    // RealTimeApp and BasicWithTimeApp work with PreserveInputRelativeOrder == false.
    struct RealTimeClock {
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType  = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        TimePointType now() { return std::chrono::system_clock::now(); }
        TimePointType resolveTime() { return now(); }
        TimePointType resolveTime(TimePointType const &) { return now(); }
        void log(dev::cd606::tm::infra::LogLevel, std::string const &) {}
    };

    using SinglePassEnv = dev::cd606::tm::infra::Environment<
        dev::cd606::tm::infra::CheckTimeComponent<false>,
        dev::cd606::tm::infra::TrivialExitControlComponent,
        SinglePassClock>;

    using RealTimeEnv = dev::cd606::tm::infra::Environment<
        dev::cd606::tm::infra::CheckTimeComponent<false>,
        dev::cd606::tm::infra::TrivialExitControlComponent,
        RealTimeClock>;

    using SPTP = SinglePassClock::TimePointType;
    using RTTP = RealTimeClock::TimePointType;

    // ----- SinglePassIterationApp ----------------------------------------
    void testSinglePass()
    {
        using M = dev::cd606::tm::infra::SinglePassIterationApp<SinglePassEnv>;
        auto node = M::liftPure<int>([](int x) { return x + 1; });
        // Must compile; must return false (no-op).
        bool r = M::configureThreadedNode(
            node,
            dev::cd606::tm::infra::ThreadedNodeOptions().SpinWait(std::chrono::microseconds(10)));
        check(!r, "SinglePassIterationApp::configureThreadedNode returns false");
    }

    // ----- TopDownSinglePassIterationApp ---------------------------------
    void testTopDownSinglePass()
    {
        using M = dev::cd606::tm::infra::TopDownSinglePassIterationApp<SinglePassEnv>;
        auto node = M::liftPure1<int>([](int x) { return x + 1; });
        bool r = M::configureThreadedNode(
            node,
            dev::cd606::tm::infra::ThreadedNodeOptions().SpinWait(std::chrono::microseconds(10)));
        check(!r, "TopDownSinglePassIterationApp::configureThreadedNode returns false");
    }

    // ----- BasicWithTimeApp ----------------------------------------------
    void testBasicWithTime()
    {
        using M = dev::cd606::tm::infra::BasicWithTimeApp<RealTimeEnv>;
        auto node = M::liftPure<int>(
            [](int) { return 0; },
            dev::cd606::tm::infra::LiftParameters<RTTP>());
        bool r = M::configureThreadedNode(
            node,
            dev::cd606::tm::infra::ThreadedNodeOptions().SpinWait(std::chrono::microseconds(10)));
        check(!r, "BasicWithTimeApp::configureThreadedNode returns false");
    }

    // ----- RealTimeApp: non-threaded node still returns false ------------
    // Regression guard: the existing behaviour must not break.
    void testRealTimeReturnsFalseForNonThreaded()
    {
        using M = dev::cd606::tm::infra::RealTimeApp<RealTimeEnv>;
        // liftPure without SuggestThreaded(true) produces an inline (non-threaded)
        // node; configureThreadedNode must return false for it.
        auto node = M::liftPure<int>([](int &&x) { return x + 1; },
            dev::cd606::tm::infra::LiftParameters<RTTP>().SuggestThreaded(false));
        bool r = M::configureThreadedNode(
            node,
            dev::cd606::tm::infra::ThreadedNodeOptions().SpinWait(std::chrono::microseconds(10)));
        check(!r, "RealTimeApp::configureThreadedNode on non-threaded node returns false");
    }

} // namespace

int main()
{
    testSinglePass();
    testTopDownSinglePass();
    testBasicWithTime();
    testRealTimeReturnsFalseForNonThreaded();
    std::printf("%s\n", failures == 0 ? "All passed." : "FAILURES.");
    return failures;
}

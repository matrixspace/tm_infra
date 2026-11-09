#include <tm_kit/infra/RealTimeApp.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace dev::cd606::tm::infra;
namespace
{
    struct TestEnv {
        using TimePointType = std::chrono::system_clock::time_point;
        static constexpr bool CheckTime = false;
    };
    using Components = RealTimeAppComponents<TestEnv>;
    using Data = TimedDataWithEnvironment<int, TestEnv, TestEnv::TimePointType>;

    // Concrete Environment deliberately makes the base non-dependent: these
    // historical spellings require ThreadedHandler's injected class name.
    class Handler : public Components::ThreadedHandler<int, Handler> {
    public:
        std::atomic<int> result{0};
        Handler()
            : ThreadedHandler()
        {
        }
        void start()
        {
            ThreadedHandler::startThread();
        }
        void waitForStart()
        {
        }
        void idleWork()
        {
        }
        void actuallyHandle(Data &&data)
        {
            bool good = ThreadedHandler::timeCheckGood(data);
            ThreadedHandler::stop();
            result.store(good ? data.timedData.value : -1, std::memory_order_release);
        }
    };
} // namespace

int main()
{
    // Detached callbacks require the host to live through process exit.
    auto *handler = new Handler;
    auto *env = new TestEnv;
    handler->start();
    Components::IHandler<int> *input = handler;
    input->handle(Data{env, {TestEnv::TimePointType{}, 42, false}});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (handler->result.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool ok = handler->result.load(std::memory_order_acquire) == 42;
    std::puts(ok ? "ALL PASSED" : "FAILED");
    return ok ? 0 : 1;
}

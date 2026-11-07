#ifndef TM_KIT_INFRA_LEGACY_DEDICATED_THREAD_EXECUTOR_HPP_
#define TM_KIT_INFRA_LEGACY_DEDICATED_THREAD_EXECUTOR_HPP_

#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#include <list>

namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace infra
            {

                // Profiling baseline from develop f569d42, RealTimeApp::ThreadedHandlerBase.
                // Retains two std::lists with splice, per-push notification and periodic
                // idle calls even without idle work. Unlike the original develop code,
                // the wait now checks queued data, stop and explicit idle wakes: a push
                // while handling a batch must not cause another 1ms wait.
                // spinWait is intentionally ignored.
                // Adaptations: shared detached state, Host callbacks, and per-item stop check.
                // Host lifetime follows the regular DedicatedThreadExecutor contract.
                template <class Item>
                class [[deprecated("Profiling baseline only; use DedicatedThreadExecutor")]] LegacyDedicatedThreadExecutor {
                    struct State {
                        ThreadedExecOptions opts;
                        std::mutex mutex;
                        std::condition_variable cond;
                        std::atomic<bool> running{false};
                        bool wakeRequested = false; // protected by mutex
                        std::list<Item> incoming, processing;
                        explicit State(ThreadedExecOptions const &o)
                            : opts(o)
                        {
                        }
                    };
                    std::shared_ptr<State> state_;
                    std::optional<std::thread::native_handle_type> handle_;

                    template <class Host>
                    static void run(std::shared_ptr<State> s, Host *host)
                    {
                        host->executorWaitForStart();
                        while (s->running.load(std::memory_order_acquire)) {
                            {
                                std::unique_lock<std::mutex> lock(s->mutex);
                                s->cond.wait_for(lock, std::chrono::milliseconds(1), [&] { return !s->incoming.empty() || !s->running.load(std::memory_order_acquire) || s->wakeRequested; });
                                if (!s->running.load(std::memory_order_acquire)) {
                                    return;
                                }
                                if (s->incoming.empty()) {
                                    // Consume an idle wake only when delivering idle work.
                                    s->wakeRequested = false;
                                    lock.unlock();
                                    host->executorIdleWork();
                                    continue;
                                }
                                s->processing.splice(s->processing.end(), s->incoming);
                            }
                            if (s->opts.trackPendingCount) {
                                host->executorBeforeBatch(s->processing.front(), s->processing.size());
                            }
                            while (!s->processing.empty()) {
                                if (!s->running.load(std::memory_order_acquire)) {
                                    return;
                                }
                                host->executorHandle(std::move(s->processing.front()));
                                s->processing.pop_front();
                            }
                        }
                    }

                public:
                    static constexpr bool kOwnContext = true;
                    static constexpr bool kSerialCallbacks = true;
                    explicit LegacyDedicatedThreadExecutor(ThreadedExecOptions const &opts = ThreadedExecOptions{})
                        : state_(std::make_shared<State>(opts))
                    {
                    }
                    LegacyDedicatedThreadExecutor(LegacyDedicatedThreadExecutor const &) = delete;
                    LegacyDedicatedThreadExecutor &operator=(LegacyDedicatedThreadExecutor const &) = delete;
                    ~LegacyDedicatedThreadExecutor()
                    {
                        stop();
                    }
                    template <class Host>
                    void start(Host *host)
                    {
                        state_->running.store(true, std::memory_order_release);
                        std::thread thread(&LegacyDedicatedThreadExecutor::run<Host>, state_, host);
                        handle_ = thread.native_handle();
                        thread.detach();
                    }
                    bool push(Item &&item)
                    {
                        if (!state_->running.load(std::memory_order_acquire)) {
                            return false;
                        }
                        {
                            std::lock_guard<std::mutex> lock(state_->mutex);
                            state_->incoming.push_back(std::move(item));
                        }
                        state_->cond.notify_one();
                        return true;
                    }
                    void stop()
                    {
                        {
                            std::lock_guard<std::mutex> lock(state_->mutex);
                            state_->running.store(false, std::memory_order_release);
                        }
                        state_->cond.notify_one();
                    }
                    void wakeIdle()
                    {
                        {
                            std::lock_guard<std::mutex> lock(state_->mutex);
                            state_->wakeRequested = true;
                        }
                        state_->cond.notify_one();
                    }
                    std::optional<std::thread::native_handle_type> threadHandle()
                    {
                        return handle_;
                    }
                };

            } // namespace infra
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif

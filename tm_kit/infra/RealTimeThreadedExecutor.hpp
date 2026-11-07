#ifndef TM_KIT_INFRA_REALTIME_THREADED_EXECUTOR_HPP_
#define TM_KIT_INFRA_REALTIME_THREADED_EXECUTOR_HPP_

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <list>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

// Handler executor contract: template <class Item> class Exec.
// Exec(ThreadedExecOptions), start(Host*), push(Item&&), stop(), wakeIdle(),
// threadHandle(). kOwnContext describes execution outside the caller;
// kSerialCallbacks describes mutual exclusion of callbacks on one host.
// Host provides executorWaitForStart/Handle/IdleWork/HasIdleWork/BeforeBatch.
// Own-context executors call WaitForStart once before other callbacks, and
// idle work only with no pending data. Inline executors never call either.
// push returns false after stop; stop is nonblocking and callback-safe.
// Per-producer FIFO is required; weaker cross-producer ordering must be
// documented. The default also preserves linearized cross-producer FIFO.
// Host must outlive callbacks, including detached-thread shutdown.

namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace infra
            {

                struct DedicatedThreadExecutorConfig {
                    static constexpr std::chrono::nanoseconds idlePeriod = std::chrono::milliseconds(1);
                    static constexpr unsigned spinBeforeSleep = 0;
                };

                struct ThreadedExecOptions {
                    //maintain the pending count passed to executorBeforeBatch (one
                    //extra relaxed RMW per push); set for OutputRealTimeThreadBufferSizeComponent
                    bool trackPendingCount = false;
                    // Enhanced / Moodycamel active wait; SharedPool uses its pool config.
                    // The default mutex/list Dedicated executor ignores this option.
                    // Bounded active wait after data, then the normal blocking path.
                    // Zero disables it. Idle timeouts do not renew this budget.
                    std::chrono::nanoseconds spinWait = std::chrono::microseconds(200);
                };

                namespace realtime_threaded_executor_detail
                {
                    inline void cpuRelax()
                    {
#if defined(__i386__) || defined(__x86_64__)
                        __asm__ __volatile__("pause");
#elif defined(__aarch64__)
                        __asm__ __volatile__("yield");
#else
                        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
                    }

                    template <class Ready>
                    bool spinUntil(std::chrono::nanoseconds budget, Ready &&ready)
                    {
                        if (budget <= std::chrono::nanoseconds::zero()) {
                            return false;
                        }
                        auto const end = std::chrono::steady_clock::now() + budget;
                        unsigned checks = 0;
                        do {
                            if (ready()) {
                                return true;
                            }
                            cpuRelax();
                            // Let another runnable consumer progress on a shared CPU.
                            if ((checks & 255) == 255) {
                                std::this_thread::yield();
                            }
                            // Amortize the clock read, but bound the active wait.
                        } while ((++checks & 31) != 0 || std::chrono::steady_clock::now() < end);
                        return false;
                    }

                } // namespace realtime_threaded_executor_detail

                // Default dedicated executor: mutex-protected incoming list and
                // consumer-local processing list, transferred with splice. Predicate
                // waits prevent lost batch notifications. Periodic idle callbacks are
                // retained even when HasIdleWork is false; spinWait is ignored.
                template <class Item, class Config = DedicatedThreadExecutorConfig>
                class DedicatedThreadExecutor {
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
                                s->cond.wait_for(lock, Config::idlePeriod, [&] { return !s->incoming.empty() || !s->running.load(std::memory_order_acquire) || s->wakeRequested; });
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
                    explicit DedicatedThreadExecutor(ThreadedExecOptions const &opts = ThreadedExecOptions{})
                        : state_(std::make_shared<State>(opts))
                    {
                    }
                    DedicatedThreadExecutor(DedicatedThreadExecutor const &) = delete;
                    DedicatedThreadExecutor &operator=(DedicatedThreadExecutor const &) = delete;
                    ~DedicatedThreadExecutor()
                    {
                        stop();
                    }
                    template <class Host>
                    void start(Host *host)
                    {
                        state_->running.store(true, std::memory_order_release);
                        std::thread thread(&DedicatedThreadExecutor::run<Host>, state_, host);
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

                // Synchronous callbacks may run concurrently on different producer threads.
                template <class Item>
                class InlineExecutor {
                    std::atomic<bool> running_{false};
                    void *host_ = nullptr;
                    void (*handle_)(void *, Item &&) = nullptr;

                public:
                    static constexpr bool kOwnContext = false;
                    static constexpr bool kSerialCallbacks = false;
                    explicit InlineExecutor(ThreadedExecOptions const & = ThreadedExecOptions{})
                    {
                    }
                    template <class Host>
                    void start(Host *h)
                    {
                        host_ = h;
                        handle_ = [](void *p, Item &&x) { static_cast<Host *>(p)->executorHandle(std::move(x)); };
                        running_.store(true, std::memory_order_release);
                    }
                    bool push(Item &&x)
                    {
                        if (!running_.load(std::memory_order_acquire)) {
                            return false;
                        }
                        handle_(host_, std::move(x));
                        return true;
                    }
                    // Internal fast path: h must be the Host passed to start().
                    // Retain the stop check and never bypass custom executors.
                    template <class Host>
                    bool pushWithHost(Item &&x, Host *h)
                    {
                        if (!running_.load(std::memory_order_acquire)) {
                            return false;
                        }
                        h->executorHandle(std::move(x));
                        return true;
                    }
                    void stop()
                    {
                        running_.store(false, std::memory_order_release);
                    }
                    void wakeIdle()
                    {
                    }
                    std::optional<std::thread::native_handle_type> threadHandle()
                    {
                        return std::nullopt;
                    }
                };

                //Environment component selecting the executor of all threaded handlers,
                //e.g. struct MyEnv : ..., RealTimeThreadedExecutorComponent<MyExec> {};
                template <template <class> class Exec>
                struct RealTimeThreadedExecutorComponent {
                    template <class Item>
                    using RealTimeThreadedExecutor = Exec<Item>;
                };

                namespace realtime_threaded_executor_detail
                {
                    template <class Exec, class Item, class Host>
                    auto submit(Exec &exec, Item &&item, Host *host, int) -> std::enable_if_t<std::is_same_v<Exec, InlineExecutor<std::decay_t<Item>>>, bool>
                    {
                        return exec.pushWithHost(std::move(item), host);
                    }
                    template <class Exec, class Item, class Host>
                    bool submit(Exec &exec, Item &&item, Host *, long)
                    {
                        return exec.push(std::move(item));
                    }

                    template <class StateT, class Item, class = void>
                    struct ExecutorFor {
                        using type = DedicatedThreadExecutor<Item>;
                    };
                    template <class StateT, class Item>
                    struct ExecutorFor<StateT, Item, std::void_t<typename StateT::template RealTimeThreadedExecutor<Item>>> {
                        using type = typename StateT::template RealTimeThreadedExecutor<Item>;
                    };
                } // namespace realtime_threaded_executor_detail
                //resolved where a node is instantiated, so StateT only has to be
                //complete there (same rule as TimeCheckerModeFor)
                template <class StateT, class Item>
                using RealTimeThreadedExecutorFor = typename realtime_threaded_executor_detail::ExecutorFor<StateT, Item>::type;
            } // namespace infra
        } // namespace tm
    } // namespace cd606
} // namespace dev

#endif

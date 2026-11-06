#ifndef TM_KIT_INFRA_REALTIME_THREADED_EXECUTOR_HPP_
#define TM_KIT_INFRA_REALTIME_THREADED_EXECUTOR_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
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
                };

                namespace realtime_threaded_executor_detail
                {
                    //Vyukov intrusive unbounded MPSC queue with a stub node.
                    //push is wait-free (one exchange + one store); the global order is
                    //the exchange order, so items of all producers form one FIFO.
                    template <class Item>
                    class MPSCQueue {
                    private:
                        struct Node {
                            std::atomic<Node *> next{nullptr};
                            std::optional<Item> value;
                        };
                        //written by producers
                        alignas(64) std::atomic<Node *> head_;
                        //owned by the single consumer
                        alignas(64) Node *tail_;

                    public:
                        MPSCQueue()
                            : head_(nullptr)
                            , tail_(nullptr)
                        {
                            Node *stub = new Node();
                            head_.store(stub, std::memory_order_relaxed);
                            tail_ = stub;
                        }
                        ~MPSCQueue()
                        {
                            Node *n = tail_;
                            while (n) {
                                Node *next = n->next.load(std::memory_order_relaxed);
                                delete n;
                                n = next;
                            }
                        }
                        MPSCQueue(MPSCQueue const &) = delete;
                        MPSCQueue &operator=(MPSCQueue const &) = delete;
                        //any thread. The exchange is seq_cst: together with the
                        //seq_cst load of the sleeping flag that follows it in the
                        //executor, and the consumer's seq_cst store of that flag
                        //followed by maybeNonEmpty(), it forms a Dekker handshake
                        //(either the consumer sees the item or the producer sees the
                        //consumer sleeping). Plain seq_cst operations are used instead of
                        //atomic_thread_fence so that TSan can check the protocol.
                        void push(Item &&x)
                        {
                            Node *n = new Node();
                            n->value.emplace(std::move(x));
                            Node *prev = head_.exchange(n, std::memory_order_seq_cst);
                            prev->next.store(n, std::memory_order_release);
                        }
                        //consumer only; may transiently report empty while a producer
                        //is between its exchange and its link store, the wake-up
                        //protocol of the executor covers that window
                        bool tryPop(std::optional<Item> &out)
                        {
                            Node *next = tail_->next.load(std::memory_order_acquire);
                            if (!next) {
                                return false;
                            }
                            out.emplace(std::move(*(next->value)));
                            next->value.reset();
                            delete tail_;
                            tail_ = next;
                            return true;
                        }
                        //consumer only. Unlike tryPop this also reports items whose
                        //producer has done the exchange but not yet linked the node, so
                        //the consumer never goes to sleep inside that window
                        bool maybeNonEmpty() const
                        {
                            return head_.load(std::memory_order_seq_cst) != tail_;
                        }
                    };

                    //everything the execution thread touches lives here and is shared
                    //with that (detached) thread, so the executor object itself may be
                    //destroyed while the thread is still winding down
                    template <class Item>
                    struct DedicatedThreadState {
                        ThreadedExecOptions opts;
                        MPSCQueue<Item> queue;
                        alignas(64) std::atomic<bool> running{false};
                        alignas(64) std::atomic<bool> sleeping{false};
                        std::atomic<bool> wakeRequested{false};
                        std::atomic<std::size_t> pending{0};
                        std::mutex mutex;
                        std::condition_variable cond;

                        explicit DedicatedThreadState(ThreadedExecOptions const &o)
                            : opts(o)
                        {
                        }

                        void notifyLocked()
                        {
                            std::lock_guard<std::mutex> _(mutex);
                            cond.notify_one();
                        }
                    };
                } // namespace realtime_threaded_executor_detail

                //Default executor: one dedicated (detached) thread per node.
                //Blocking: the producer only takes the mutex / notifies when the consumer
                //has announced it is about to sleep (eventcount style), so the hot path
                //is one wait-free push without syscall. While the host has idle work the
                //consumer waits at most Config::idlePeriod and then calls the idle work,
                //otherwise it sleeps until push / stop / wakeIdle.

                template <class Item, class Config = DedicatedThreadExecutorConfig>
                class DedicatedThreadExecutor {
                private:
                    using State = realtime_threaded_executor_detail::DedicatedThreadState<Item>;
                    std::shared_ptr<State> s_;
                    std::optional<std::thread::native_handle_type> handle_;

                    static void wakeIfSleeping(State *s)
                    {
                        //seq_cst load after the seq_cst exchange in queue.push; pairs
                        //with the seq_cst store + maybeNonEmpty() in waitForWork
                        if (s->sleeping.load(std::memory_order_seq_cst)) {
                            s->notifyLocked();
                        }
                    }
                    template <class Host>
                    static bool drainBatch(State *s, Host *h)
                    {
                        std::optional<Item> item;
                        if (!s->queue.tryPop(item)) {
                            return false;
                        }
                        if (s->opts.trackPendingCount) {
                            h->executorBeforeBatch(*item, s->pending.load(std::memory_order_relaxed));
                        }
                        do {
                            if (s->opts.trackPendingCount) {
                                s->pending.fetch_sub(1, std::memory_order_relaxed);
                            }
                            h->executorHandle(std::move(*item));
                            item.reset();
                            if (!s->running.load(std::memory_order_acquire)) {
                                break;
                            }
                        } while (s->queue.tryPop(item));
                        return true;
                    }
                    //Blocking mode: returns once there may be work, the idle period has
                    //elapsed, or the executor is stopping
                    template <class Host>
                    static void waitForWork(State *s, Host *h)
                    {
                        for (unsigned i = 0; i < Config::spinBeforeSleep; ++i) {
                            if (s->queue.maybeNonEmpty() || !s->running.load(std::memory_order_acquire)) {
                                return;
                            }
                        }
                        bool const hasIdle = h->executorHasIdleWork();
                        s->sleeping.store(true, std::memory_order_seq_cst);
                        auto ready = [s]() { return s->queue.maybeNonEmpty() || !s->running.load(std::memory_order_acquire) || s->wakeRequested.load(std::memory_order_acquire); };
                        if (!ready()) {
                            std::unique_lock<std::mutex> lock(s->mutex);
                            if (hasIdle) {
                                s->cond.wait_for(lock, Config::idlePeriod, ready);
                            } else {
                                s->cond.wait(lock, ready);
                            }
                        }
                        s->sleeping.store(false, std::memory_order_relaxed);
                        //a wakeIdle racing with this reset is not lost: its host state
                        //change happened before it set the flag, and the next round
                        //re-reads executorHasIdleWork() before sleeping again
                        s->wakeRequested.exchange(false, std::memory_order_acq_rel);
                    }
                    template <class Host>
                    static void run(std::shared_ptr<State> s, Host *h)
                    {
                        h->executorWaitForStart();
                        while (s->running.load(std::memory_order_acquire)) {
                            if (drainBatch(s.get(), h)) {
                                //more data may have arrived while handling; only
                                //idle / sleep once the queue is really empty
                                continue;
                            }
                            waitForWork(s.get(), h);
                            if (!s->running.load(std::memory_order_acquire)) {
                                return;
                            }
                            if (!s->queue.maybeNonEmpty()) {
                                //timed out (or woken by wakeIdle) with nothing pending
                                h->executorIdleWork();
                            }
                        }
                    }

                public:
                    static constexpr bool kOwnContext = true;
                    static constexpr bool kSerialCallbacks = true;
                    explicit DedicatedThreadExecutor(ThreadedExecOptions const &opts = ThreadedExecOptions{})
                        : s_(std::make_shared<State>(opts))
                        , handle_(std::nullopt)
                    {
                    }
                    DedicatedThreadExecutor(DedicatedThreadExecutor const &) = delete;
                    DedicatedThreadExecutor &operator=(DedicatedThreadExecutor const &) = delete;
                    ~DedicatedThreadExecutor()
                    {
                        stop();
                    }
                    template <class Host>
                    void start(Host *h)
                    {
                        s_->running.store(true, std::memory_order_release);
                        std::thread th(&DedicatedThreadExecutor::run<Host>, s_, h);
                        handle_ = th.native_handle();
                        //same lifecycle as the old handler bases: the thread is detached
                        th.detach();
                    }
                    bool push(Item &&x)
                    {
                        State *s = s_.get();
                        if (!s->running.load(std::memory_order_acquire)) {
                            return false;
                        }
                        if (s->opts.trackPendingCount) {
                            s->pending.fetch_add(1, std::memory_order_relaxed);
                        }
                        s->queue.push(std::move(x));
                        wakeIfSleeping(s);
                        return true;
                    }
                    void stop()
                    {
                        s_->running.store(false, std::memory_order_release);
                        s_->notifyLocked();
                    }
                    void wakeIdle()
                    {
                        s_->wakeRequested.store(true, std::memory_order_release);
                        s_->notifyLocked();
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

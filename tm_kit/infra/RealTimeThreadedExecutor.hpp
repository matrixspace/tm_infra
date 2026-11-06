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

//Execution model of RealTimeApp threaded handlers (ThreadedHandlerBase,
//BusyLoopThreadedHandlerBase and everything built on them, e.g. threaded
//ActionCore / MultiActionCore).
//
//A threaded handler owns one executor Exec<Item, Mode>. The executor decides
//who calls the handler callbacks, on which thread and when; the handler
//logic is unchanged. The default is DedicatedThreadExecutor (one thread per
//node, as before). An environment replaces it by providing
//    template <class Item, ThreadedExecMode M>
//    using RealTimeThreadedExecutor = ...;
//most easily by inheriting RealTimeThreadedExecutorComponent<Exec>.
//
//Executor contract (Exec<Item, Mode>):
//    explicit Exec(ThreadedExecOptions const &);
//    template <class Host> void start(Host *h);
//        h stays valid while the executor runs; h->executorWaitForStart()
//        is called once on the execution context before any other callback
//    bool push(Item &&x);  //any thread; returns false (x dropped) once stopped
//    void stop();          //non-blocking, may be called from inside a callback
//    void wakeIdle();      //h->executorHasIdleWork() may have changed
//    std::optional<std::thread::native_handle_type> threadHandle();
//Host callbacks the executor may call:
//    void executorWaitForStart();
//    void executorHandle(Item &&);
//    void executorIdleWork();
//    bool executorHasIdleWork();
//    void executorBeforeBatch(Item const &first, std::size_t pending);
//        only called when ThreadedExecOptions::trackPendingCount is set
//Guarantees every executor must give:
//  1. callbacks of one host never run concurrently with each other
//  2. items pushed by one thread are handled in push order; the default
//     executor also keeps the linearized push order across producers (as
//     the old mutex+list did), which the monotonic TimeChecker relies on.
//     An executor with weaker ordering must document it.
//  3. once stop() has been called, no new executorHandle call is started
//  4. executorIdleWork is only called when no item is pending; while
//     executorHasIdleWork() is false the executor may block indefinitely
//     until push / stop / wakeIdle

namespace dev { namespace cd606 { namespace tm { namespace infra {

    enum class ThreadedExecMode {Blocking, BusyLoop};

    struct ThreadedExecOptions {
        //BusyLoop only: do not call std::this_thread::yield() between rounds
        bool noYield = false;
        //Blocking only: how long to wait for data before calling the idle
        //work, while the host has idle work (same 1ms rhythm as before)
        std::chrono::nanoseconds idlePeriod = std::chrono::milliseconds(1);
        //Blocking only: rounds of polling the queue before going to sleep;
        //trades CPU for wake-up latency, 0 keeps the thread fully idle
        unsigned spinBeforeSleep = 0;
        //maintain the pending count passed to executorBeforeBatch (one
        //extra relaxed RMW per push); set for OutputRealTimeThreadBufferSizeComponent
        bool trackPendingCount = false;
    };

    namespace realtime_threaded_executor_detail {
        //Vyukov intrusive unbounded MPSC queue with a stub node.
        //push is wait-free (one exchange + one store); the global order is
        //the exchange order, so items of all producers form one FIFO.
        template <class Item>
        class MPSCQueue {
        private:
            struct Node {
                std::atomic<Node *> next {nullptr};
                std::optional<Item> value;
            };
            //written by producers
            alignas(64) std::atomic<Node *> head_;
            //owned by the single consumer
            alignas(64) Node *tail_;
        public:
            MPSCQueue() : head_(nullptr), tail_(nullptr) {
                Node *stub = new Node();
                head_.store(stub, std::memory_order_relaxed);
                tail_ = stub;
            }
            ~MPSCQueue() {
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
            void push(Item &&x) {
                Node *n = new Node();
                n->value.emplace(std::move(x));
                Node *prev = head_.exchange(n, std::memory_order_seq_cst);
                prev->next.store(n, std::memory_order_release);
            }
            //consumer only; may transiently report empty while a producer
            //is between its exchange and its link store, the wake-up
            //protocol of the executor covers that window
            bool tryPop(std::optional<Item> &out) {
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
            bool maybeNonEmpty() const {
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
            alignas(64) std::atomic<bool> running {false};
            alignas(64) std::atomic<bool> sleeping {false};
            std::atomic<bool> wakeRequested {false};
            std::atomic<std::size_t> pending {0};
            std::mutex mutex;
            std::condition_variable cond;

            explicit DedicatedThreadState(ThreadedExecOptions const &o) : opts(o) {}

            void notifyLocked() {
                std::lock_guard<std::mutex> _(mutex);
                cond.notify_one();
            }
        };
    }

    //Default executor: one dedicated (detached) thread per node.
    //Blocking: the producer only takes the mutex / notifies when the consumer
    //has announced it is about to sleep (eventcount style), so the hot path
    //is one wait-free push without syscall. While the host has idle work the
    //consumer waits at most opts.idlePeriod and then calls the idle work,
    //otherwise it sleeps until push / stop / wakeIdle.
    //BusyLoop: same queue, polling loop, idle work every round (as before).
    template <class Item, ThreadedExecMode Mode>
    class DedicatedThreadExecutor {
    private:
        using State = realtime_threaded_executor_detail::DedicatedThreadState<Item>;
        std::shared_ptr<State> s_;
        std::optional<std::thread::native_handle_type> handle_;

        static void wakeIfSleeping(State *s) {
            //seq_cst load after the seq_cst exchange in queue.push; pairs
            //with the seq_cst store + maybeNonEmpty() in waitForWork
            if (s->sleeping.load(std::memory_order_seq_cst)) {
                s->notifyLocked();
            }
        }
        template <class Host>
        static bool drainBatch(State *s, Host *h) {
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
        static void waitForWork(State *s, Host *h) {
            for (unsigned i = 0; i < s->opts.spinBeforeSleep; ++i) {
                if (s->queue.maybeNonEmpty() || !s->running.load(std::memory_order_acquire)) {
                    return;
                }
            }
            bool const hasIdle = h->executorHasIdleWork();
            s->sleeping.store(true, std::memory_order_seq_cst);
            auto ready = [s]() {
                return s->queue.maybeNonEmpty()
                    || !s->running.load(std::memory_order_acquire)
                    || s->wakeRequested.load(std::memory_order_acquire);
            };
            if (!ready()) {
                std::unique_lock<std::mutex> lock(s->mutex);
                if (hasIdle) {
                    s->cond.wait_for(lock, s->opts.idlePeriod, ready);
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
        static void run(std::shared_ptr<State> s, Host *h) {
            h->executorWaitForStart();
            while (s->running.load(std::memory_order_acquire)) {
                if constexpr (Mode == ThreadedExecMode::BusyLoop) {
                    h->executorIdleWork();
                    drainBatch(s.get(), h);
                    if (!s->opts.noYield) {
                        std::this_thread::yield();
                    }
                } else {
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
        }
    public:
        explicit DedicatedThreadExecutor(ThreadedExecOptions const &opts = ThreadedExecOptions {})
            : s_(std::make_shared<State>(opts)), handle_(std::nullopt) {}
        DedicatedThreadExecutor(DedicatedThreadExecutor const &) = delete;
        DedicatedThreadExecutor &operator=(DedicatedThreadExecutor const &) = delete;
        ~DedicatedThreadExecutor() {
            stop();
        }
        template <class Host>
        void start(Host *h) {
            s_->running.store(true, std::memory_order_release);
            std::thread th(&DedicatedThreadExecutor::run<Host>, s_, h);
            handle_ = th.native_handle();
            //same lifecycle as the old handler bases: the thread is detached
            th.detach();
        }
        bool push(Item &&x) {
            State *s = s_.get();
            if (!s->running.load(std::memory_order_acquire)) {
                return false;
            }
            if (s->opts.trackPendingCount) {
                s->pending.fetch_add(1, std::memory_order_relaxed);
            }
            s->queue.push(std::move(x));
            if constexpr (Mode == ThreadedExecMode::Blocking) {
                wakeIfSleeping(s);
            }
            return true;
        }
        void stop() {
            s_->running.store(false, std::memory_order_release);
            if constexpr (Mode == ThreadedExecMode::Blocking) {
                s_->notifyLocked();
            }
        }
        void wakeIdle() {
            if constexpr (Mode == ThreadedExecMode::Blocking) {
                s_->wakeRequested.store(true, std::memory_order_release);
                s_->notifyLocked();
            }
        }
        std::optional<std::thread::native_handle_type> threadHandle() {
            return handle_;
        }
    };

    //Environment component selecting the executor of all threaded handlers,
    //e.g. struct MyEnv : ..., RealTimeThreadedExecutorComponent<MyExec> {};
    template <template <class, ThreadedExecMode> class Exec>
    struct RealTimeThreadedExecutorComponent {
        template <class Item, ThreadedExecMode M>
        using RealTimeThreadedExecutor = Exec<Item, M>;
    };

    namespace realtime_threaded_executor_detail {
        template <class StateT, class Item, ThreadedExecMode M, class = void>
        struct ExecutorFor {
            using type = DedicatedThreadExecutor<Item, M>;
        };
        template <class StateT, class Item, ThreadedExecMode M>
        struct ExecutorFor<StateT, Item, M, std::void_t<typename StateT::template RealTimeThreadedExecutor<Item, M>>> {
            using type = typename StateT::template RealTimeThreadedExecutor<Item, M>;
        };
    }
    //resolved where a node is instantiated, so StateT only has to be
    //complete there (same rule as TimeCheckerModeFor)
    template <class StateT, class Item, ThreadedExecMode M>
    using RealTimeThreadedExecutorFor = typename realtime_threaded_executor_detail::ExecutorFor<StateT, Item, M>::type;

} } } }

#endif

#ifndef TM_KIT_INFRA_REALTIME_THREADED_EXECUTOR_HPP_
#define TM_KIT_INFRA_REALTIME_THREADED_EXECUTOR_HPP_

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <list>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

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
// Host must outlive callbacks, including shutdown after stop()+join.
// Optional: bool configure(ThreadedNodeOptions const &), called before the
// graph starts (possibly after start()); executors without it ignore options.

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

                    // True when every id is usable in a cpu_set_t (Linux only).
                    inline bool validCpus(std::vector<int> const &cpus)
                    {
#ifdef __linux__
                        for (int c : cpus) {
                            if (c < 0 || c >= CPU_SETSIZE) {
                                return false;
                            }
                        }
                        return true;
#else
                        return cpus.empty();
#endif
                    }

                    // Pins a thread to the given CPU set. Empty cpus leaves the
                    // affinity unchanged. Returns false on failure or non-Linux.
                    inline bool setThreadAffinity(std::thread::native_handle_type h, std::vector<int> const &cpus)
                    {
                        if (cpus.empty()) {
                            return true;
                        }
#ifdef __linux__
                        cpu_set_t set;
                        CPU_ZERO(&set);
                        for (int c : cpus) {
                            if (c < 0 || c >= CPU_SETSIZE) {
                                return false;
                            }
                            CPU_SET(c, &set);
                        }
                        return pthread_setaffinity_np(h, sizeof(set), &set) == 0;
#else
                        (void)h;
                        return false;
#endif
                    }

                } // namespace realtime_threaded_executor_detail

                // Automatic CPU choice for a busy node thread (see IThreadCpuAllocator).
                struct CpuAutoBindRequest {
                    // Restrict the choice to this NUMA node (OS index); empty lets
                    // the allocator choose.
                    std::optional<int> numaNode;
                };

                // Per-node thread settings, applied after the node is created and
                // before the graph starts (RealTimeApp::configureThreadedNode).
                // Executors opt in with bool configure(ThreadedNodeOptions const &);
                // executors without it (Inline, SharedPool, older custom ones)
                // ignore spinWait / cpus and report false. cpuAutoBind is
                // resolved by the node, see ExecHandlerCore::configureThread.
                struct ThreadedNodeOptions {
                    // Overrides ThreadedExecOptions::spinWait for this node. Ignored
                    // by the default mutex/list DedicatedThreadExecutor.
                    std::optional<std::chrono::nanoseconds> spinWait;
                    // CPUs the node's dedicated thread may run on (Linux). Empty
                    // keeps the inherited affinity.
                    std::vector<int> cpus;
                    // Let the Environment's IThreadCpuAllocator pick one CPU for
                    // this thread. Exclusive with cpus; failures throw.
                    std::optional<CpuAutoBindRequest> cpuAutoBind;

                    ThreadedNodeOptions &SpinWait(std::chrono::nanoseconds s)
                    {
                        spinWait = s;
                        return *this;
                    }
                    ThreadedNodeOptions &Cpus(std::vector<int> c)
                    {
                        cpus = std::move(c);
                        return *this;
                    }
                    ThreadedNodeOptions &CpuAutoBind()
                    {
                        cpuAutoBind = CpuAutoBindRequest{};
                        return *this;
                    }
                    // Automatic choice restricted to one NUMA node (OS index), e.g.
                    // the node of the NIC this thread polls. Not a CPU number:
                    // use Cpus() to pin to specific CPUs.
                    ThreadedNodeOptions &CpuAutoBindOnNuma(int numaNode)
                    {
                        cpuAutoBind = CpuAutoBindRequest{numaNode};
                        return *this;
                    }
                };

                // Chooses CPUs for busy node threads. Implementations must be
                // thread safe and throw (never fall back silently) when no
                // suitable CPU exists. basic::HwlocThreadCpuAllocator is the
                // hwloc-based implementation.
                class IThreadCpuAllocator {
                public:
                    virtual ~IThreadCpuAllocator() = default;
                    // Returns the OS CPU index the thread is to be pinned to.
                    virtual int acquire(CpuAutoBindRequest const &req, std::string const &who) = 0;
                    // Records CPUs pinned explicitly (ThreadedNodeOptions::cpus) so
                    // automatic choices avoid them; throws if they conflict with
                    // automatically assigned CPUs.
                    virtual void reserve(std::vector<int> const &cpus, std::string const &who) = 0;
                    // Returns CPUs obtained from acquire() or reserve(); never throws.
                    virtual void release(std::vector<int> const &cpus) noexcept = 0;
                };

                //Environment component providing the allocator used by CpuAutoBind,
                //e.g. struct MyEnv : ..., ThreadCpuAllocatorComponent<MyAllocatorAccess> {};
                //where MyAllocatorAccess::instance() returns IThreadCpuAllocator &.
                template <class Access>
                struct ThreadCpuAllocatorComponent {
                    static IThreadCpuAllocator &threadCpuAllocator()
                    {
                        return Access::instance();
                    }
                };

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
                    std::thread thread_;           // joined in destructor after stop()
                    std::optional<std::thread::native_handle_type> handle_;
                    std::vector<int> pendingCpus_; // protected by state_->mutex

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
                        if (thread_.joinable()) {
                            // Destroyed from its own callback: the thread cannot join
                            // itself; it exits on its own after the callback returns.
                            if (thread_.get_id() == std::this_thread::get_id()) {
                                thread_.detach();
                            } else {
                                thread_.join();
                            }
                        }
                    }
                    template <class Host>
                    void start(Host *host)
                    {
                        state_->running.store(true, std::memory_order_release);
                        std::thread thread(&DedicatedThreadExecutor::run<Host>, state_, host);
                        {
                            std::lock_guard<std::mutex> lock(state_->mutex);
                            handle_ = thread.native_handle();
                            realtime_threaded_executor_detail::setThreadAffinity(*handle_, pendingCpus_);
                        }
                        thread_ = std::move(thread);
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
                    // spinWait is ignored (this executor never spins). The CPU set
                    // is applied immediately when the thread runs, otherwise at start.
                    bool configure(ThreadedNodeOptions const &o)
                    {
                        if (!realtime_threaded_executor_detail::validCpus(o.cpus)) {
                            return false;
                        }
                        std::lock_guard<std::mutex> lock(state_->mutex);
                        if (o.cpus.empty()) {
                            return true; // keep the current / pending affinity
                        }
                        pendingCpus_ = o.cpus;
                        if (handle_ && state_->running.load(std::memory_order_acquire)) {
                            return realtime_threaded_executor_detail::setThreadAffinity(*handle_, pendingCpus_);
                        }
                        return true;
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

                    // Executors without configure(ThreadedNodeOptions) ignore node options.
                    template <class Exec>
                    auto configure(Exec &exec, ThreadedNodeOptions const &o, int) -> decltype(exec.configure(o), bool())
                    {
                        return exec.configure(o);
                    }
                    template <class Exec>
                    bool configure(Exec &, ThreadedNodeOptions const &, long)
                    {
                        return false;
                    }

                    // Detects StateT::threadCpuAllocator() (ThreadCpuAllocatorComponent).
                    template <class StateT, class = void>
                    struct HasThreadCpuAllocator : std::false_type {};
                    template <class StateT>
                    struct HasThreadCpuAllocator<StateT, std::void_t<decltype(StateT::threadCpuAllocator())>> : std::true_type {};

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

                // Thrown by ThreadCpuLease when a thread cannot be pinned.
                class ThreadCpuBindError : public std::runtime_error {
                public:
                    explicit ThreadCpuBindError(std::string const &s) : std::runtime_error(s) {}
                };

                // CPUs held from an IThreadCpuAllocator; returned when the lease is
                // destroyed, reset or overwritten by move assignment. Movable, not
                // copyable; one lease object must not be used by two threads at once.
                //
                // bindCurrentThread* pin the calling thread, so they can be called
                // concurrently from any number of threads: the allocator serialises
                // the choice, each thread only changes its own affinity. A thread may
                // hold at most one such lease at a time; a second bind on the same
                // thread throws instead of silently occupying another core.
                //
                //   // first line of your own thread function
                //   auto lease = ThreadCpuLease::bindCurrentThread<Env>("nic-rx");
                class ThreadCpuLease {
                public:
                    ThreadCpuLease() = default;
                    ThreadCpuLease(ThreadCpuLease const &) = delete;
                    ThreadCpuLease &operator=(ThreadCpuLease const &) = delete;
                    ThreadCpuLease(ThreadCpuLease &&o) noexcept
                    {
                        take(o);
                    }
                    ThreadCpuLease &operator=(ThreadCpuLease &&o) noexcept
                    {
                        if (this != &o) {
                            reset();
                            take(o);
                        }
                        return *this;
                    }
                    ~ThreadCpuLease()
                    {
                        reset();
                    }

                    // Automatic choice (same rules as ThreadedNodeOptions::CpuAutoBind).
                    template <class Env>
                    static ThreadCpuLease bindCurrentThread(std::string const &who)
                    {
                        static_assert(realtime_threaded_executor_detail::HasThreadCpuAllocator<Env>::value,
                            "ThreadCpuLease::bindCurrentThread needs an Environment with a ThreadCpuAllocatorComponent (e.g. basic::HwlocThreadCpuAllocatorComponent)");
                        return bindCurrentThread(Env::threadCpuAllocator(), who);
                    }
                    // Automatic choice restricted to one NUMA node (OS index).
                    template <class Env>
                    static ThreadCpuLease bindCurrentThreadOnNuma(int numaNode, std::string const &who)
                    {
                        static_assert(realtime_threaded_executor_detail::HasThreadCpuAllocator<Env>::value,
                            "ThreadCpuLease::bindCurrentThreadOnNuma needs an Environment with a ThreadCpuAllocatorComponent (e.g. basic::HwlocThreadCpuAllocatorComponent)");
                        return bindCurrentThreadOnNuma(Env::threadCpuAllocator(), numaNode, who);
                    }
                    // Explicit CPUs, reserved so automatic choices avoid them.
                    template <class Env>
                    static ThreadCpuLease bindCurrentThreadTo(std::vector<int> const &cpus, std::string const &who)
                    {
                        static_assert(realtime_threaded_executor_detail::HasThreadCpuAllocator<Env>::value,
                            "ThreadCpuLease::bindCurrentThreadTo needs an Environment with a ThreadCpuAllocatorComponent (e.g. basic::HwlocThreadCpuAllocatorComponent)");
                        return bindCurrentThreadTo(Env::threadCpuAllocator(), cpus, who);
                    }

                    // Same, for code that holds the allocator rather than the Environment.
                    static ThreadCpuLease bindCurrentThread(IThreadCpuAllocator &a, std::string const &who)
                    {
                        return bindSelf(a, who, [&] { return std::vector<int>{a.acquire(CpuAutoBindRequest{}, who)}; });
                    }
                    static ThreadCpuLease bindCurrentThreadOnNuma(IThreadCpuAllocator &a, int numaNode, std::string const &who)
                    {
                        return bindSelf(a, who, [&] { return std::vector<int>{a.acquire(CpuAutoBindRequest{numaNode}, who)}; });
                    }
                    static ThreadCpuLease bindCurrentThreadTo(IThreadCpuAllocator &a, std::vector<int> const &cpus, std::string const &who)
                    {
                        if (cpus.empty() || !realtime_threaded_executor_detail::validCpus(cpus)) {
                            throw ThreadCpuBindError("ThreadCpuLease for " + who + ": invalid CPU list");
                        }
                        return bindSelf(a, who, [&] {
                            a.reserve(cpus, who);
                            return cpus;
                        });
                    }

                    // Takes ownership of CPUs already obtained from a (acquire or
                    // reserve) for a thread pinned by other means, e.g. a node thread.
                    static ThreadCpuLease adopt(IThreadCpuAllocator &a, std::vector<int> cpus) noexcept
                    {
                        ThreadCpuLease l;
                        l.allocator_ = &a;
                        l.cpus_ = std::move(cpus);
                        return l;
                    }

                    bool active() const
                    {
                        return allocator_ != nullptr && !cpus_.empty();
                    }
                    std::vector<int> const &cpus() const
                    {
                        return cpus_;
                    }
                    // First (for automatic leases: the only) CPU; -1 when inactive.
                    int cpu() const
                    {
                        return cpus_.empty() ? -1 : cpus_.front();
                    }
                    // Exchanges ownership; nothing is released.
                    void swap(ThreadCpuLease &o) noexcept
                    {
                        std::swap(allocator_, o.allocator_);
                        cpus_.swap(o.cpus_);
                        bound_.swap(o.bound_);
                    }
                    friend void swap(ThreadCpuLease &a, ThreadCpuLease &b) noexcept
                    {
                        a.swap(b);
                    }
                    // Returns the CPUs now. The thread's affinity is left as it is.
                    void reset() noexcept
                    {
                        if (allocator_ && !cpus_.empty()) {
                            allocator_->release(cpus_);
                        }
                        if (bound_) {
                            unregisterThread(*bound_);
                        }
                        allocator_ = nullptr;
                        cpus_.clear();
                        bound_.reset();
                    }

                private:
                    IThreadCpuAllocator *allocator_ = nullptr;
                    std::vector<int> cpus_;
                    std::optional<std::uint64_t> bound_; // thread token, set by bindCurrentThread*

                    void take(ThreadCpuLease &o) noexcept
                    {
                        allocator_ = o.allocator_;
                        cpus_ = std::move(o.cpus_);
                        bound_ = o.bound_;
                        o.allocator_ = nullptr;
                        o.cpus_.clear();
                        o.bound_.reset();
                    }

                    // Never reused within the process, unlike std::thread::id: a lease
                    // moved off a finished thread must not block a later thread that
                    // happens to receive the same id.
                    static std::uint64_t currentThreadToken()
                    {
                        static std::atomic<std::uint64_t> next{1};
                        thread_local std::uint64_t const token = next.fetch_add(1, std::memory_order_relaxed);
                        return token;
                    }
                    // Threads currently holding a bindCurrentThread* lease, by token
                    // (not a thread_local flag) so a lease moved to and destroyed on
                    // another thread still unregisters the right one.
                    static std::mutex &registryMutex()
                    {
                        static std::mutex *m = new std::mutex; // leaked: used from detached threads at exit
                        return *m;
                    }
                    static std::set<std::uint64_t> &registry()
                    {
                        static std::set<std::uint64_t> *r = new std::set<std::uint64_t>;
                        return *r;
                    }
                    static void unregisterThread(std::uint64_t id) noexcept
                    {
                        std::lock_guard<std::mutex> _(registryMutex());
                        registry().erase(id);
                    }

                    template <class Obtain>
                    static ThreadCpuLease bindSelf(IThreadCpuAllocator &a, std::string const &who, Obtain &&obtain)
                    {
                        auto const self = currentThreadToken();
                        {
                            std::lock_guard<std::mutex> _(registryMutex());
                            if (!registry().insert(self).second) {
                                throw ThreadCpuBindError("ThreadCpuLease for " + who + ": this thread already holds a CPU lease; reset it before binding again");
                            }
                        }
                        ThreadCpuLease l;
                        l.bound_ = self; // unregisters on any exit below
                        l.allocator_ = &a;
                        l.cpus_ = obtain(); // allocator exceptions propagate
#ifdef __linux__
                        if (!realtime_threaded_executor_detail::setThreadAffinity(pthread_self(), l.cpus_)) {
                            std::string list;
                            for (int c : l.cpus_) {
                                list += (list.empty() ? "" : ",") + std::to_string(c);
                            }
                            throw ThreadCpuBindError("ThreadCpuLease for " + who + ": pthread_setaffinity_np failed for CPUs " + list);
                        }
#else
                        throw ThreadCpuBindError("ThreadCpuLease for " + who + ": thread CPU binding is only supported on Linux");
#endif
                        return l;
                    }
                };
            } // namespace infra
        } // namespace tm
    } // namespace cd606
} // namespace dev

#endif

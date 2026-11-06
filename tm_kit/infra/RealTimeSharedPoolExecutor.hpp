#ifndef TM_KIT_INFRA_REALTIME_SHARED_POOL_EXECUTOR_HPP_
#define TM_KIT_INFRA_REALTIME_SHARED_POOL_EXECUTOR_HPP_

#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unordered_map>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

//Optional executor, NOT the default: all threaded handlers of an environment
//share one process-wide thread pool instead of owning a thread each.
//Enable it with
//    struct MyEnv : ..., RealTimeSharedPoolExecutorComponent {};
//and (optionally) configure the pool before the first threaded node is built:
//    SharedThreadPoolConfig cfg;
//    cfg.threadCount = 4;
//    cfg.cpus = {2,3,4,5};
//    SharedThreadPool::configure(cfg);
//
//Every node is a strand: its own MPSC queue plus a signed pending counter
//(items + idle requests). The producer whose increment moves the counter
//from 0 schedules the strand on the pool; a pool thread drains at most
//maxBatch items, then reschedules the strand if the counter is still
//positive. So the callbacks of one node never overlap (contract 1) and the
//cross-producer FIFO of the MPSC queue is kept (contract 2).
//
//Differences from DedicatedThreadExecutor (document them to users):
//  - threadHandle() returns std::nullopt; nodes no longer own a thread, so
//    thread_local state, per-node pinning and "each threaded action has its
//    own thread" (PATTERNS.md 4.1) do not hold
//  - a callback that blocks or retries for long (waitForStart waiting for
//    the graph start, ChainWriter with IdleLogic, blocking client calls)
//    occupies a pool thread meanwhile; such nodes should stay on the
//    default executor
//  - idle work is driven by one pool-wide tick (SharedThreadPoolConfig::
//    idleTick, default 1ms) instead of DedicatedThreadExecutorConfig::idlePeriod;
//    only nodes with idle work participate; with no active idle nodes
//    the timer blocks until a registration/wakeIdle change
//  - the destructor waits until no pool thread is inside a callback of the
//    node (unless called from that callback), so the host is never used
//    after the executor is gone
//  - pool threads are detached and the pool is intentionally leaked at
//    process exit (avoids static destruction order problems)

namespace dev { namespace cd606 { namespace tm { namespace infra {

    struct SharedThreadPoolConfig {
        //0 means std::thread::hardware_concurrency() (at least 1)
        unsigned threadCount = 0;
        //if not empty, pool thread i is pinned to cpus[i % cpus.size()] (Linux)
        std::vector<int> cpus;
        //period of the pool-wide idle tick
        std::chrono::nanoseconds idleTick = std::chrono::milliseconds(1);
        //max items one strand handles before giving the pool thread back
        unsigned maxBatch = 64;
    };

    namespace realtime_shared_pool_detail {
        class IStrand : public std::enable_shared_from_this<IStrand> {
        public:
            virtual ~IStrand() = default;
            virtual void runSlice() = 0;
            virtual void requestIdle() = 0;
            // Queried only while updating the idle registry; must not lock host state.
            virtual bool idleActive() const
            {
                return true;
            }
        };
        //the strand whose callbacks the current pool thread is running
        inline thread_local IStrand const *tlCurrentStrand = nullptr;
    }

    class SharedThreadPool {
    private:
        using IStrand = realtime_shared_pool_detail::IStrand;
        SharedThreadPoolConfig cfg_;
        std::mutex mutex_;
        std::condition_variable cond_;
        std::deque<std::shared_ptr<IStrand>> ready_;
        std::mutex registryMutex_;
        std::condition_variable registryCond_;
        struct IdleEntry {
            IStrand *key;
            std::weak_ptr<IStrand> weak;
        };
        std::vector<IdleEntry> registry_;
        std::unordered_map<IStrand *, std::size_t> registryIndex_;

        static std::mutex &configMutex() {
            static std::mutex m;
            return m;
        }
        static SharedThreadPoolConfig &pendingConfig() {
            static SharedThreadPoolConfig c;
            return c;
        }
        static bool &started() {
            static bool b = false;
            return b;
        }

        explicit SharedThreadPool(SharedThreadPoolConfig const &cfg) : cfg_(cfg) {
            if (cfg_.threadCount == 0) {
                cfg_.threadCount = std::thread::hardware_concurrency();
                if (cfg_.threadCount == 0) {
                    cfg_.threadCount = 1;
                }
            }
            if (cfg_.maxBatch == 0) {
                cfg_.maxBatch = 1;
            }
            for (unsigned i = 0; i < cfg_.threadCount; ++i) {
                std::thread th(&SharedThreadPool::workerLoop, this);
#ifdef __linux__
                if (!cfg_.cpus.empty()) {
                    cpu_set_t set;
                    CPU_ZERO(&set);
                    CPU_SET(cfg_.cpus[i % cfg_.cpus.size()], &set);
                    pthread_setaffinity_np(th.native_handle(), sizeof(set), &set);
                }
#endif
                th.detach();
            }
            std::thread(&SharedThreadPool::tickLoop, this).detach();
        }
        void workerLoop() {
            while (true) {
                std::shared_ptr<IStrand> s;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cond_.wait(lock, [this]() {return !ready_.empty();});
                    s = std::move(ready_.front());
                    ready_.pop_front();
                }
                //s keeps the strand alive for the whole slice, even if its
                //executor is destroyed meanwhile
                realtime_shared_pool_detail::tlCurrentStrand = s.get();
                s->runSlice();
                realtime_shared_pool_detail::tlCurrentStrand = nullptr;
            }
        }
        // Called under registryMutex_. Keep the scanned entries contiguous;
        // the hash index is used only for membership changes, not each tick.
        void eraseIdleLocked(IStrand *key)
        {
            auto it = registryIndex_.find(key);
            if (it == registryIndex_.end()) {
                return;
            }
            auto index = it->second;
            if (index + 1 != registry_.size()) {
                registry_[index] = std::move(registry_.back());
                registryIndex_.find(registry_[index].key)->second = index;
            }
            registry_.pop_back();
            registryIndex_.erase(it);
        }
        void tickLoop() {
            std::vector<std::shared_ptr<IStrand>> alive;
            std::unique_lock<std::mutex> lock(registryMutex_);
            while (true) {
                registryCond_.wait(lock, [this] { return !registry_.empty(); });
                if (registryCond_.wait_for(lock, cfg_.idleTick, [this] { return registry_.empty(); })) {
                    continue;
                }
                for (std::size_t i = 0; i < registry_.size();) {
                    if (auto p = registry_[i].weak.lock()) {
                        alive.push_back(std::move(p));
                        ++i;
                    } else {
                        eraseIdleLocked(registry_[i].key);
                    }
                }
                lock.unlock();
                for (auto &p : alive) {
                    p->requestIdle();
                }
                alive.clear(); // release references, retain buffer capacity
                lock.lock();
            }
        }
    public:
        //must be called before the first threaded node using the pool is
        //built; throws std::logic_error afterwards
        static void configure(SharedThreadPoolConfig const &cfg) {
            std::lock_guard<std::mutex> _(configMutex());
            if (started()) {
                throw std::logic_error("SharedThreadPool::configure called after the pool was started");
            }
            pendingConfig() = cfg;
        }
        static SharedThreadPool &instance() {
            static SharedThreadPool *pool = []() {
                std::lock_guard<std::mutex> _(configMutex());
                started() = true;
                //leaked on purpose, its threads are detached
                return new SharedThreadPool(pendingConfig());
            }();
            return *pool;
        }
        SharedThreadPoolConfig const &config() const {
            return cfg_;
        }
        void schedule(std::shared_ptr<IStrand> s) {
            {
                std::lock_guard<std::mutex> _(mutex_);
                ready_.push_back(std::move(s));
            }
            cond_.notify_one();
        }
        void registerStrand(std::shared_ptr<IStrand> const &s) {
            std::lock_guard<std::mutex> lock(registryMutex_);
            // stop() removes under this same lock. Rechecking the flags
            // here prevents a delayed activation from re-registering a
            // strand after stop has already removed it.
            if (s->idleActive()) {
                auto it = registryIndex_.find(s.get());
                if (it != registryIndex_.end()) {
                    registry_[it->second].weak = s;
                } else {
                    registry_.push_back({s.get(), s});
                    try {
                        registryIndex_.emplace(s.get(), registry_.size() - 1);
                    } catch (...) {
                        registry_.pop_back();
                        throw;
                    }
                }
            } else {
                eraseIdleLocked(s.get());
            }
            registryCond_.notify_one();
        }
        void unregisterStrand(IStrand *s)
        {
            std::lock_guard<std::mutex> lock(registryMutex_);
            eraseIdleLocked(s);
            if (registry_.empty()) {
                registryCond_.notify_one();
            }
        }
    };

    namespace realtime_shared_pool_detail {
        //pending_ counts work units: queued items, one start token and at
        //most one idle token. Whoever moves it away from 0 schedules the
        //strand; a slice subtracts what it consumed and reschedules if
        //anything is left. hostMutex_ is held for the whole slice, which
        //serializes the callbacks (contract 1) and lets the executor
        //destructor wait for a running slice.
        template <class Item>
        class Strand final : public IStrand {
        private:
            ThreadedExecOptions opts_;
            realtime_threaded_executor_detail::MPSCQueue<Item> queue_;
            alignas(64) std::atomic<std::int64_t> pending_ {0};
            alignas(64) std::atomic<bool> running_ {false};
            std::atomic<bool> idleRequested_ {false};
            //cached result of executorHasIdleWork(), refreshed inside slices
            std::atomic<bool> wantsIdle_{false};
            std::atomic<bool> orphaned_ {false};
            bool started_ = false;
            SharedThreadPool *pool_ = nullptr;
            std::mutex hostMutex_;
            void *host_ = nullptr;
            // Type erasure once per slice, not once per handled item.
            void (*run_)(Strand *) = nullptr;

            void setWantsIdle(bool wants)
            {
                // Only slices write this cache, under hostMutex_.
                if (wantsIdle_.load(std::memory_order_relaxed) != wants) {
                    wantsIdle_.store(wants, std::memory_order_release);
                    pool_->registerStrand(this->shared_from_this());
                }
            }
            void addWork() {
                if (pending_.fetch_add(1, std::memory_order_acq_rel) == 0) {
                    pool_->schedule(this->shared_from_this());
                }
            }
            void addIdleToken() {
                if (!idleRequested_.exchange(true, std::memory_order_acq_rel)) {
                    addWork();
                }
            }
        public:
            explicit Strand(ThreadedExecOptions const &o) : opts_(o) {}
            template <class Host>
            void start(Host *h)
            {
                pool_ = &SharedThreadPool::instance();
                {
                    std::lock_guard<std::mutex> _(hostMutex_);
                    host_ = h;
                    run_ = [](Strand *self) { self->template runSliceTyped<Host>(); };
                }
                running_.store(true, std::memory_order_release);
                //start token: the first slice runs executorWaitForStart
                addWork();
            }
            bool push(Item &&x) {
                if (!running_.load(std::memory_order_acquire)) {
                    return false;
                }
                queue_.push(std::move(x));
                addWork();
                return true;
            }
            void stop() {
                if (running_.exchange(false, std::memory_order_acq_rel) && pool_) {
                    pool_->unregisterStrand(this);
                }
            }
            void wakeIdle() {
                if (pool_) {
                    //forces a slice that re-reads executorHasIdleWork()
                    addIdleToken();
                }
            }
            //called by the executor destructor
            void detachHost() {
                stop();
                if (tlCurrentStrand == static_cast<IStrand const *>(this)) {
                    //destroyed from inside our own callback: the running
                    //slice drops the host once the callback returns
                    orphaned_.store(true, std::memory_order_release);
                } else {
                    std::lock_guard<std::mutex> _(hostMutex_);
                    host_ = nullptr;
                }
            }
            bool idleActive() const override
            {
                return running_.load(std::memory_order_acquire) && wantsIdle_.load(std::memory_order_acquire);
            }
            void requestIdle() override {
                if (running_.load(std::memory_order_acquire) && wantsIdle_.load(std::memory_order_acquire)) {
                    addIdleToken();
                }
            }
            void runSlice() override
            {
                run_(this);
            }

        private:
            template <class Host>
            void runSliceTyped()
            {
                std::int64_t consumed = 0;
                {
                    std::lock_guard<std::mutex> lock(hostMutex_);
                    auto alive = [this]() {
                        return host_ && running_.load(std::memory_order_acquire)
                            && !orphaned_.load(std::memory_order_acquire);
                    };
                    Host *h = static_cast<Host *>(host_);
                    if (!started_) {
                        started_ = true;
                        ++consumed;
                        if (alive()) {
                            h->executorWaitForStart();
                            if (alive()) {
                                setWantsIdle(h->executorHasIdleWork());
                            }
                        }
                    }
                    unsigned n = 0;
                    unsigned const maxBatch = pool_->config().maxBatch;
                    while (n < maxBatch && queue_.tryConsume([&](Item &&item) {
                        ++consumed;
                        ++n;
                        if (alive()) {
                            if (opts_.trackPendingCount && n == 1) {
                                h->executorBeforeBatch(item, static_cast<std::size_t>(std::max<std::int64_t>(pending_.load(std::memory_order_relaxed), 0)));
                            }
                            h->executorHandle(std::move(item));
                        }
                        // After stop(), remaining items are destroyed in place.
                    })) {
                    }
                    if (idleRequested_.exchange(false, std::memory_order_acq_rel)) {
                        ++consumed;
                        if (alive()) {
                            bool const w = h->executorHasIdleWork();
                            setWantsIdle(w);
                            //contract 4: idle work only with nothing pending
                            if (w && !queue_.maybeNonEmpty()) {
                                h->executorIdleWork();
                            }
                        }
                    }
                    if (orphaned_.load(std::memory_order_acquire)) {
                        host_ = nullptr;
                    }
                }
                //an item whose producer is between exchange and link is
                //counted but not yet poppable; the reschedule picks it up
                if (pending_.fetch_sub(consumed, std::memory_order_acq_rel) - consumed > 0) {
                    pool_->schedule(this->shared_from_this());
                }
            }
        };

        template <class Item>
        class SharedPoolStrandExecutor {
        private:
            std::shared_ptr<Strand<Item>> s_;
        public:
            static constexpr bool kOwnContext = true;
            static constexpr bool kSerialCallbacks = true;
            explicit SharedPoolStrandExecutor(ThreadedExecOptions const &opts = ThreadedExecOptions {})
                : s_(std::make_shared<Strand<Item>>(opts)) {}
            SharedPoolStrandExecutor(SharedPoolStrandExecutor const &) = delete;
            SharedPoolStrandExecutor &operator=(SharedPoolStrandExecutor const &) = delete;
            ~SharedPoolStrandExecutor() {
                s_->detachHost();
            }
            template <class Host>
            void start(Host *h) {
                s_->start(h);
            }
            bool push(Item &&x) {
                return s_->push(std::move(x));
            }
            void stop() {
                s_->stop();
            }
            void wakeIdle() {
                s_->wakeIdle();
            }
            std::optional<std::thread::native_handle_type> threadHandle() {
                return std::nullopt;
            }
        };
    }

    template <class Item>
    using RealTimeSharedPoolExecutor = realtime_shared_pool_detail::SharedPoolStrandExecutor<Item>;

    struct RealTimeSharedPoolExecutorComponent
        : public RealTimeThreadedExecutorComponent<RealTimeSharedPoolExecutor> {};

} } } }

#endif

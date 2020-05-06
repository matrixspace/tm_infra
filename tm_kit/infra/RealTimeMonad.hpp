#ifndef TM_KIT_INFRA_REALTIME_MONAD_HPP_
#define TM_KIT_INFRA_REALTIME_MONAD_HPP_

#include <tm_kit/infra/WithTimeData.hpp>

#include <vector>
#include <list>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

namespace dev { namespace cd606 { namespace tm { namespace infra {
    class RealTimeMonadException : public std::runtime_error {
    public:
        RealTimeMonadException(std::string const &s) : std::runtime_error(s) {}
    };

    template <class StateT>
    class RealTimeMonadComponents {
    public:
        template <class T>
        using Key = Key<T,StateT>;
        template <class A, class B>
        using KeyedData = KeyedData<A,B,StateT>;

        template <class T>
        class IHandler {
        public:
            virtual ~IHandler() {}
            virtual void handle(TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> &&data) = 0;
        };
        template <class T>
        class TimeChecker {
        private:
            bool hasData_;
            typename StateT::TimePointType lastTime_;
        public:
            TimeChecker(FanInParamMask const &notUsed=FanInParamMask()) : hasData_(false), lastTime_() {}
            inline bool operator()(TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> const &data) {
                if (StateT::CheckTime) {
                    if (hasData_ && data.timedData.timePoint < lastTime_) {
                        return false;
                    }
                    hasData_ = true;
                    lastTime_ = data.timedData.timePoint;
                }
                return true;
            }
            inline bool good() const {
                return hasData_;
            }
        };

        #include <tm_kit/infra/RealTimeMonad_TimeChecker_Piece.hpp>

    private:
        template <class T>
        class ThreadedHandlerBase {
        private:
            TimeChecker<T> timeChecker_;
            std::mutex mutex_;
            std::condition_variable cond_;
            std::thread th_;
            std::atomic<bool> running_;
            std::list<TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType>> incoming_, processing_;  

            void stopThread() {
                running_ = false;
            }
            void runThread() {
                while (running_) {
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cond_.wait_for(lock, std::chrono::seconds(1));
                        if (!running_) {
                            lock.unlock();
                            return;
                        }
                        if (incoming_.empty()) {
                            lock.unlock();
                            continue;
                        }
                        processing_.splice(processing_.end(), incoming_);
                        lock.unlock();
                    }
                    while (!processing_.empty()) {
                        auto &data = processing_.front();
                        actuallyHandle(std::move(data));
                        processing_.pop_front();
                    }
                }  
            }
        protected:
            bool timeCheckGood(TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> const &data) {
                return timeChecker_(data);
            }
            virtual void actuallyHandle(TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> &&data) = 0;
        public:
            ThreadedHandlerBase(FanInParamMask const &requireMask=FanInParamMask()) : timeChecker_(requireMask), mutex_(), cond_(), th_(), running_(false), incoming_(), processing_() {
                running_ = true;
                th_ = std::thread(&ThreadedHandlerBase::runThread, this);
                th_.detach();
            }
            virtual ~ThreadedHandlerBase() {
                stopThread();
            }
        protected:
            void putData(TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> &&data) {
                if (running_) {
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        incoming_.push_back(std::move(data));
                    }
                    cond_.notify_one();
                }                    
            }
            TimeChecker<T> const &timeChecker() const {
                return timeChecker_;
            }
        };

    public:
        template <class T>
        class ThreadedHandler : public virtual IHandler<T>, public ThreadedHandlerBase<T> {
        public:
            ThreadedHandler(FanInParamMask const &requireMask=FanInParamMask()) : ThreadedHandlerBase<T>(requireMask) {
            }
            virtual ~ThreadedHandler() {
            }
            virtual void handle(TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> &&data) override final {
                ThreadedHandlerBase<T>::putData(std::move(data));                
            }
        };

        #include <tm_kit/infra/RealTimeMonad_ThreadedHandler_Piece.hpp>

        template <class T>
        class Producer {
        private:
            std::vector<IHandler<T> *> handlers_;
            std::unordered_set<IHandler<T> *> handlerSet_;
            std::mutex mutex_;
        public:
            Producer() : handlers_(), handlerSet_(), mutex_() {}
            Producer(Producer const &) = delete;
            Producer &operator=(Producer const &) = delete;
            Producer(Producer &&) = default;
            Producer &operator=(Producer &&) = default;
            virtual ~Producer() {}
            void addHandler(IHandler<T> *h) {
                if (h == nullptr) {
                    return;
                }
                std::lock_guard<std::mutex> _(mutex_);
                if (handlerSet_.find(h) == handlerSet_.end()) {
                    handlers_.push_back(h);
                    handlerSet_.insert(h);
                }               
            }
            void publish(StateT *env, T &&data) {
                publish(withtime_utils::pureTimedDataWithEnvironment<T, StateT, typename StateT::TimePointType>(env, std::move(data)));
            }
            void publish(TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> &&data) {
                std::lock_guard<std::mutex> _(mutex_);
                auto s = handlers_.size();
                switch (s) {
                    case 0:
                        return;
                    case 1:
                        handlers_.front()->handle(std::move(data));
                        break;
                    default:
                        {
                            TimedDataWithEnvironment<T, StateT, typename StateT::TimePointType> ownedCopy { std::move(data) };
                            for (auto *h : handlers_) {
                                h->handle(ownedCopy.clone());
                            }
                        }
                        break;
                }
            }
        };
        //It is NOT ALLOWED to directly publish a KeyedData<A,B> in a producer of KeyedData<A,B>
        //The reason is that any producer of KeyedData<A,B> is supposed to be an on-order facility
        //and the produced KeyedData<A,B> must come from a **STORED** Key<A>. So, the producer is 
        //only allowed to calculate and publish Key<B>'s, and the logic here will automatically
        //lookup the correct Key<A> to match with it and combine them into KeyedData<A,B>.
        //This is why this specialization is a completely separate implemention from the generic 
        //Producer<T>.
        template <class A, class B>
        class Producer<KeyedData<A,B>> {
        private:
            std::unordered_map<typename StateT::IDType, std::tuple<Key<A>, IHandler<KeyedData<A,B>> *>, typename StateT::IDHash> theMap_;
            std::mutex mutex_;
        public:
            Producer() : theMap_(), mutex_() {}
            Producer(Producer const &) = delete;
            Producer &operator=(Producer const &) = delete;
            Producer(Producer &&) = default;
            Producer &operator=(Producer &&) = default;
            virtual ~Producer() {}
            void registerKeyHandler(Key<A> const &k, IHandler<KeyedData<A,B>> *handler) {
                std::lock_guard<std::mutex> _(mutex_);
                if (handler != nullptr) {
                    theMap_[k.id()] = {k, handler};
                }
            }
            void publish(StateT *env, Key<B> &&data, bool isFinal) {
                auto ret = withtime_utils::pureTimedDataWithEnvironment<Key<B>, StateT, typename StateT::TimePointType>(env, std::move(data), isFinal);
                publish(std::move(ret));
            }
            void publish(TimedDataWithEnvironment<Key<B>, StateT, typename StateT::TimePointType> &&data) {
                std::lock_guard<std::mutex> _(mutex_);
                auto iter = theMap_.find(data.timedData.value.id());
                if (iter == theMap_.end()) {
                    return;
                }
                auto *h = std::get<1>(iter->second);
                if (h == nullptr) {  
                    return;
                }
                bool isFinal = data.timedData.finalFlag;
                KeyedData<A,B> outputD {std::get<0>(iter->second), std::move(data.timedData.value.key())};
                //There is a slight difference in how RealTimeMonad and SinglePassIterationMonad
                //handles the "final" reply message in an OnOrderFacility
                //For SinglePassIterationMonad, when the message goes to the consumer, it will be marked as
                //"final" ONLY IF this message is the last one ever in the OnOrderFacility (meaning that the
                //key is a "final" one too). This makes sense because otherwise we will terminate the
                //OnOrderFacility too early in that monad.
                //However, here, for RealTimeMonad, the final flag will be preserved when it gets into the consumer
                //The reason is that in RealTimeMonad, the final flag is only used to release 
                //internal key records of OnOrderFacility, so we pass the final flag in case that the consumer
                //is actually somehow passing it to another OnOrderFacility which will release its internal key
                //object. Since RealTimeMonad does not really terminate the logic of OnOrderFacility based
                //on final flag, this is harmless
                WithTime<KeyedData<A,B>,typename StateT::TimePointType> outputT {data.timedData.timePoint, std::move(outputD), isFinal};                                  
                h->handle(withtime_utils::pureTimedDataWithEnvironment<KeyedData<A,B>, StateT, typename StateT::TimePointType>(data.environment, std::move(outputT)));    
                if (isFinal) {
                    theMap_.erase(iter);
                }  
            }
            void publishResponse(TimedDataWithEnvironment<Key<B>, StateT, typename StateT::TimePointType> &&data) {
                this->publish(std::move(data));
            }
        };

        class IExternalComponent {
        public:
            virtual ~IExternalComponent() {}
            virtual void start(StateT *environment) = 0;
        };

        template <class A, class B>
        class AbstractAction : public virtual IHandler<A>, public Producer<B> {
        };

        #include <tm_kit/infra/RealTimeMonad_AbstractAction_Piece.hpp>

        //KeyedData cannot be imported "out of the blue"
        template <class T, std::enable_if_t<!is_keyed_data_v<T>,int> = 0>
        class AbstractImporter : public virtual IExternalComponent, public Producer<T> {
        };
        //Keys and KeyedData can be exported, for example to be written to database,
        //so there is no check on the exporter
        template <class T>
        class AbstractExporter : public virtual IExternalComponent, public virtual IHandler<T> {
        };
        template <class A, class B>
        using AbstractOnOrderFacility = AbstractAction<Key<A>,KeyedData<A,B>>;
        template <class A, class B>
        class AbstractOffShoreFacility : public virtual IExternalComponent, public virtual AbstractOnOrderFacility<A,B> {};

        template <class A, class B>
        class OneLevelDownKleisli {
        protected:
            virtual ~OneLevelDownKleisli() {}
            virtual TimedMonadData<B, StateT> action(StateT *env, WithTime<A, typename StateT::TimePointType> &&data) = 0;
        };

        template <class A, class B, class F, bool ForceFinal>
        class PureOneLevelDownKleisli : public virtual OneLevelDownKleisli<A,B> {
        private:
            F f_;
            virtual TimedMonadData<B, StateT> action(StateT *env, WithTime<A,typename StateT::TimePointType> &&data) override final {
                if (ForceFinal) {
                    auto ret = withtime_utils::pureTimedDataWithEnvironmentLift(env, f_, std::move(data));
                    ret.timedData.finalFlag = true;
                    return ret;
                } else {
                    return withtime_utils::pureTimedDataWithEnvironmentLift(env, f_, std::move(data));
                }               
            }
        public:
            PureOneLevelDownKleisli(F &&f) : f_(std::move(f)) {}
            PureOneLevelDownKleisli(PureOneLevelDownKleisli const &) = delete;
            PureOneLevelDownKleisli &operator=(PureOneLevelDownKleisli const &) = delete;
            PureOneLevelDownKleisli(PureOneLevelDownKleisli &&) = default;
            PureOneLevelDownKleisli &operator=(PureOneLevelDownKleisli &&) = default;
            virtual ~PureOneLevelDownKleisli() {}
        };
        template <class A, class B, class F, bool ForceFinal>
        class MaybeOneLevelDownKleisli : public virtual OneLevelDownKleisli<A, B> {
        private:
            F f_;
            virtual TimedMonadData<B, StateT> action(StateT *env, WithTime<A,typename StateT::TimePointType> &&data) override final {
                auto b = f_(std::move(data.value));
                if (b) {
                    return withtime_utils::pureTimedDataWithEnvironment(env, WithTime<B, typename StateT::TimePointType> {
                        data.timePoint,
                        std::move(*b),
                        (ForceFinal?true:data.finalFlag)
                    });
                } else {
                    return std::nullopt;
                }
            }
        public:
            MaybeOneLevelDownKleisli(F &&f) : f_(std::move(f)) {}
            MaybeOneLevelDownKleisli(MaybeOneLevelDownKleisli const &) = delete;
            MaybeOneLevelDownKleisli &operator=(MaybeOneLevelDownKleisli const &) = delete;
            MaybeOneLevelDownKleisli(MaybeOneLevelDownKleisli &&) = default;
            MaybeOneLevelDownKleisli &operator=(MaybeOneLevelDownKleisli &&) = default;
            virtual ~MaybeOneLevelDownKleisli() {}
        };
        template <class A, class B, class F, bool ForceFinal>
        class EnhancedMaybeOneLevelDownKleisli : public virtual OneLevelDownKleisli<A, B> {
        private:
            F f_;
            virtual TimedMonadData<B, StateT> action(StateT *env, WithTime<A,typename StateT::TimePointType> &&data) override final {
                auto b = f_(std::tuple<typename StateT::TimePointType, A> {data.timePoint, std::move(data.value)});
                if (b) {
                    return withtime_utils::pureTimedDataWithEnvironment(env, WithTime<B, typename StateT::TimePointType> {
                        data.timePoint,
                        std::move(*b),
                        (ForceFinal?true:data.finalFlag)
                    });
                } else {
                    return std::nullopt;
                }
            }
        public:
            EnhancedMaybeOneLevelDownKleisli(F &&f) : f_(std::move(f)) {}
            EnhancedMaybeOneLevelDownKleisli(EnhancedMaybeOneLevelDownKleisli const &) = delete;
            EnhancedMaybeOneLevelDownKleisli &operator=(EnhancedMaybeOneLevelDownKleisli const &) = delete;
            EnhancedMaybeOneLevelDownKleisli(EnhancedMaybeOneLevelDownKleisli &&) = default;
            EnhancedMaybeOneLevelDownKleisli &operator=(EnhancedMaybeOneLevelDownKleisli &&) = default;
            virtual ~EnhancedMaybeOneLevelDownKleisli() {}
        };
        template <class A, class B, class F, bool ForceFinal>
        class DirectOneLevelDownKleisli : public virtual OneLevelDownKleisli<A, B> {
        private:
            F f_;
            virtual TimedMonadData<B, StateT> action(StateT *env, WithTime<A,typename StateT::TimePointType> &&data) override final {
                if (ForceFinal) {
                    auto ret = f_(TimedDataWithEnvironment<A, StateT, typename StateT::TimePointType> {
                        env
                        , std::move(data)
                    });
                    if (!ret) {
                        return ret;
                    } else {
                        ret->timedData.finalFlag = true;
                        return ret;
                    }
                } else {
                    return f_(TimedDataWithEnvironment<A, StateT, typename StateT::TimePointType> {
                        env
                        , std::move(data)
                    });
                }               
            }
        public:
            DirectOneLevelDownKleisli(F &&f) : f_(std::move(f)) {}
            DirectOneLevelDownKleisli(DirectOneLevelDownKleisli const &) = delete;
            DirectOneLevelDownKleisli &operator=(DirectOneLevelDownKleisli const &) = delete;
            DirectOneLevelDownKleisli(DirectOneLevelDownKleisli &&) = default;
            DirectOneLevelDownKleisli &operator=(DirectOneLevelDownKleisli &&) = default;
            virtual ~DirectOneLevelDownKleisli() {}
        };

        template <class A, class B, class F, class KleisliImpl, class Main, std::enable_if_t<std::is_base_of_v<OneLevelDownKleisli<A,B>,KleisliImpl>,int> = 0>
        class OneLevelDownKleisliMixin : public virtual KleisliImpl, public Main {
        public:
            template <typename... Args>
            OneLevelDownKleisliMixin(F &&f, Args&&... args) 
                : KleisliImpl(std::move(f)), Main(std::forward<Args>(args)...) {}
            virtual ~OneLevelDownKleisliMixin() {}
            OneLevelDownKleisliMixin(OneLevelDownKleisliMixin const &) = delete;
            OneLevelDownKleisliMixin &operator=(OneLevelDownKleisliMixin const &) = delete;
            OneLevelDownKleisliMixin(OneLevelDownKleisliMixin &&) = delete;
            OneLevelDownKleisliMixin &operator=(OneLevelDownKleisliMixin &&) = delete;
        };
    };
    
    template <class StateT>
    class RealTimeMonad {
    private:  
        friend class MonadRunner<RealTimeMonad>;

    public:
        //The data definition part
        //This part is of course best put into a common code, however, 
        //because of template inheritance issues, it is actually easier
        //just to include it in each monad
        using TimePoint = typename StateT::TimePointType;
        using StateType = StateT;
        using EnvironmentType = StateT;
        template <class T>
        using TimedDataType = WithTime<T, TimePoint>;
        template <class T>
        using Key = Key<T,StateT>;
        template <class A, class B>
        using KeyedData = KeyedData<A,B,StateT>;

        template <class T>
        using InnerData = TimedDataWithEnvironment<T, StateType, TimePoint>;

        template <class T>
        static InnerData<T> pureInnerData(StateT *env, T &&t, bool finalFlag = false) {
            return withtime_utils::pureTimedDataWithEnvironment<T, StateT, TimePoint>(env, std::move(t), finalFlag);
        }
        template <class T>
        static InnerData<T> pureInnerData(StateT *env, TimedDataType<T> &&t, bool preserveTime=false) {
            return withtime_utils::pureTimedDataWithEnvironment<T, StateT, TimePoint>(env, std::move(t), preserveTime);
        }
        template <class T>
        static InnerData<T> pureInnerData(InnerData<T> &&t, bool preserveTime=false) {
            return withtime_utils::pureTimedDataWithEnvironment<T, StateT, TimePoint>(std::move(t), preserveTime);
        }
        template <class A, class F>
        static auto pureInnerDataLift(StateT *environment, F const &f, TimedDataType<A> &&a, bool preserveTime=false) -> decltype(withtime_utils::pureTimedDataWithEnvironmentLift<A, F, StateT, TimePoint>(environment, f, std::move(a), preserveTime)) {
            return withtime_utils::pureTimedDataWithEnvironmentLift<A, F, StateT, TimePoint>(environment, f, std::move(a), preserveTime);
        }
        template <class A, class F>
        static auto pureInnerDataLift(F const &f, InnerData<A> &&a, bool preserveTime=false) -> decltype(withtime_utils::pureTimedDataWithEnvironmentLift<A, F, StateT, TimePoint>(f, std::move(a), preserveTime)) {
            return withtime_utils::pureTimedDataWithEnvironmentLift<A, F, StateT, TimePoint>(f, std::move(a), preserveTime);
        }

        template <class T>
        using IHandler = typename RealTimeMonadComponents<StateT>::template IHandler<T>;
        template <class T>
        using Producer = typename RealTimeMonadComponents<StateT>::template Producer<T>;

        using IExternalComponent = typename RealTimeMonadComponents<StateT>::IExternalComponent;

        template <class T>
        using Data = TimedMonadData<T,StateT>;
    
    private:
        template <class T, class Input, class Output>
        class TwoWayHolder {
        private:
            friend class RealTimeMonad;
            std::unique_ptr<T> core_;
            void release() {
                core_.release();
            }
        public:
            using InputType = Input;
            using OutputType = Output;

            TwoWayHolder(std::unique_ptr<T> &&p) : core_(std::move(p)) {}
            template <class A>
            TwoWayHolder(A *p) : core_(std::unique_ptr<T>(static_cast<T *>(p))) {}
        };
        template <class T, class Data>
        class OneWayHolder {
        private:
            friend class RealTimeMonad;
            std::unique_ptr<T> core_;
            void release() {
                core_.release();
            }
        public:
            using DataType = Data;

            OneWayHolder(std::unique_ptr<T> &&p) : core_(std::move(p)) {}
            template <class A>
            OneWayHolder(A *p) : core_(std::unique_ptr<T>(static_cast<T *>(p))) {}
        };
        template <class T1, class Input, class Output, class T2, class Data>
        class ThreeWayHolder {
        private:
            friend class RealTimeMonad;
            //The reason why we use raw pointers here is that
            //the two pointers may well be pointing to the same
            //object (through different base classes). Therefore
            //we give up on managing the memory for this special
            //case
            T1 *core1_;
            T2 *core2_;
            void release() {
                core1_ = nullptr;
                core2_ = nullptr;
            }
        public:
            using InputType = Input;
            using OutputType = Output;
            using DataType = Data;

            template <class A, class B>
            ThreeWayHolder(A *p1, B*p2) : core1_(static_cast<T1 *>(p1)), core2_(static_cast<T2 *>(p2)) {}
            ThreeWayHolder(ThreeWayHolder const &) = delete;
            ThreeWayHolder &operator=(ThreeWayHolder const &) = delete;
            ThreeWayHolder(ThreeWayHolder &&) = default;
            ThreeWayHolder &operator=(ThreeWayHolder &&) = default;
        };
    
    private:
        template <class A, class B, bool Threaded>
        class ActionCore {};
        template <class A, class B>
        class ActionCore<A,B,true> : public virtual RealTimeMonadComponents<StateT>::template OneLevelDownKleisli<A,B>, public RealTimeMonadComponents<StateT>::template AbstractAction<A,B>, public RealTimeMonadComponents<StateT>::template ThreadedHandler<A> {
        protected:
            virtual void actuallyHandle(InnerData<A> &&data) override final {
                if (!this->timeCheckGood(data)) {
                    return;
                }
                auto res = this->action(data.environment, std::move(data.timedData));
                if (res) {
                    Producer<B>::publish(std::move(*res));
                }
            }
        public:
            ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : RealTimeMonadComponents<StateT>::template AbstractAction<A,B>(), RealTimeMonadComponents<StateT>::template ThreadedHandler<A>(requireMask) {
            }
            virtual ~ActionCore() {
            }
        };
        template <class A, class B>
        class ActionCore<A,B,false> : public virtual RealTimeMonadComponents<StateT>::template OneLevelDownKleisli<A,B>, public RealTimeMonadComponents<StateT>::template AbstractAction<A,B> {
        private:
            typename RealTimeMonadComponents<StateT>::template TimeChecker<A> timeChecker_;
        public:
            ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : RealTimeMonadComponents<StateT>::template AbstractAction<A,B>(), timeChecker_(requireMask) {
            }
            virtual ~ActionCore() {
            }
            virtual void handle(InnerData<A> &&data) override final {
                if (timeChecker_(data)) {
                    auto res = this->action(data.environment, std::move(data.timedData));
                    if (res) {
                        Producer<B>::publish(std::move(*res));
                    }
                }
            }
        };
        //PureActionCore will be specialized so it is not defined with mixin
        template <class A, class B, class F, bool Threaded>
        class PureActionCore final : public virtual RealTimeMonadComponents<StateT>::template PureOneLevelDownKleisli<A,B,F,false>, public ActionCore<A,B,Threaded> {
        public:
            PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : RealTimeMonadComponents<StateT>::template PureOneLevelDownKleisli<A,B,F,false>(std::move(f)), ActionCore<A,B,Threaded>(requireMask) {}
            PureActionCore(PureActionCore const &) = delete;
            PureActionCore &operator=(PureActionCore const &) = delete;
            PureActionCore(PureActionCore &&) = default;
            PureActionCore &operator=(PureActionCore &&) = default;
            virtual ~PureActionCore() {}
        };
        template <class A, class B, class F, bool Threaded>
        using MaybeActionCore = typename RealTimeMonadComponents<StateT>::template OneLevelDownKleisliMixin<
                                A, B, F,
                                typename RealTimeMonadComponents<StateT>::template MaybeOneLevelDownKleisli<A,B,F,false>,
                                ActionCore<A,B,Threaded>
                                >;
        template <class A, class B, class F, bool Threaded>
        using EnhancedMaybeActionCore = typename RealTimeMonadComponents<StateT>::template OneLevelDownKleisliMixin<
                                A, B, F,
                                typename RealTimeMonadComponents<StateT>::template EnhancedMaybeOneLevelDownKleisli<A,B,F,false>,
                                ActionCore<A,B,Threaded>
                                >;
        template <class A, class B, class F, bool Threaded>
        using KleisliActionCore = typename RealTimeMonadComponents<StateT>::template OneLevelDownKleisliMixin<
                                A, B, F,
                                typename RealTimeMonadComponents<StateT>::template DirectOneLevelDownKleisli<A,B,F,false>,
                                ActionCore<A,B,Threaded>
                                >;

        #include <tm_kit/infra/RealTimeMonad_ActionCore_Piece.hpp>

    public:
        //We don't allow any action to manufacture KeyedData "out of the blue"
        //, but it is ok to manipulate Keys, so the check is one-sided
        template <class A, class B, std::enable_if_t<!is_keyed_data_v<B>, int> = 0>
        using AbstractAction = typename RealTimeMonadComponents<StateT>::template AbstractAction<A,B>;

        template <class A, class B, std::enable_if_t<!is_keyed_data_v<B>, int> = 0>
        using Action = TwoWayHolder<typename RealTimeMonadComponents<StateT>::template AbstractAction<A,B>,A,B>;
        
        template <class A, class F>
        static auto liftPure(F &&f, bool threaded=false, FanInParamMask const &requireMask=FanInParamMask()) -> std::shared_ptr<Action<A,decltype(f(A()))>> {
            if (threaded) {
                return std::make_shared<Action<A,decltype(f(A()))>>(new PureActionCore<A,decltype(f(A())),F,true>(std::move(f), requireMask));
            } else {
                return std::make_shared<Action<A,decltype(f(A()))>>(new PureActionCore<A,decltype(f(A())),F,false>(std::move(f), requireMask));
            }
        }     
        template <class A, class F>
        static auto liftMaybe(F &&f, bool threaded=false) -> std::shared_ptr<Action<A, typename decltype(f(A()))::value_type>> {
            if (threaded) {
                return std::make_shared<Action<A,typename decltype(f(A()))::value_type>>(new MaybeActionCore<A,typename decltype(f(A()))::value_type,F,true>(std::move(f)));
            } else {
                return std::make_shared<Action<A,typename decltype(f(A()))::value_type>>(new MaybeActionCore<A,typename decltype(f(A()))::value_type,F,false>(std::move(f)));
            }
        }
        template <class A, class F>
        static auto enhancedMaybe(F &&f, bool threaded=false) -> std::shared_ptr<Action<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>> {
            if (threaded) {
                return std::make_shared<Action<A,typename decltype(f(std::tuple<TimePoint,A>()))::value_type>>(new EnhancedMaybeActionCore<A,typename decltype(f(std::tuple<TimePoint,A>()))::value_type,F,true>(std::move(f)));
            } else {
                return std::make_shared<Action<A,typename decltype(f(std::tuple<TimePoint,A>()))::value_type>>(new EnhancedMaybeActionCore<A,typename decltype(f(std::tuple<TimePoint,A>()))::value_type,F,false>(std::move(f)));
            }
        }
        template <class A, class F>
        static auto kleisli(F &&f, bool threaded=false) -> std::shared_ptr<Action<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>> {
            if (threaded) {
                return std::make_shared<Action<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>>(
                    new KleisliActionCore<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType, F, true>(std::move(f))
                );
            } else {
                return std::make_shared<Action<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>>(
                    new KleisliActionCore<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType, F, false>(std::move(f))
                );
            }
        }
    public:
        template <class A, class B, bool Threaded>
        class OnOrderFacilityCore {};
        template <class A, class B>
        class OnOrderFacilityCore<A,B,true> : public virtual RealTimeMonadComponents<StateT>::template OneLevelDownKleisli<A,B>, public virtual RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B>, public RealTimeMonadComponents<StateT>::template ThreadedHandler<Key<A>> {
        protected:
            virtual void actuallyHandle(InnerData<Key<A>> &&data) override final {  
                if (!this->timeCheckGood(data)) {
                    return;
                }              
                auto id = data.timedData.value.id();
                WithTime<A,TimePoint> a {data.timedData.timePoint, data.timedData.value.key()};
                auto res = this->action(data.environment, std::move(a));
                if (res) {
                    Producer<KeyedData<A,B>>::publish(
                        pureInnerDataLift([id=std::move(id)](B &&b) -> Key<B> {
                            return {std::move(id), std::move(b)};
                        }, std::move(*res))
                    );
                }
            }
        public:
            OnOrderFacilityCore(FanInParamMask const &requireMask=FanInParamMask()) : RealTimeMonadComponents<StateT>::template OneLevelDownKleisli<A,B>(), RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B>(), RealTimeMonadComponents<StateT>::template ThreadedHandler<Key<A>>(requireMask) {
            }
            virtual ~OnOrderFacilityCore() {
            }
        };
        template <class A, class B>
        class OnOrderFacilityCore<A,B,false> : public virtual RealTimeMonadComponents<StateT>::template OneLevelDownKleisli<A,B>, public virtual RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B> {
        private:
            typename RealTimeMonadComponents<StateT>::template TimeChecker<Key<A>> timeChecker_;
        public:
            OnOrderFacilityCore(FanInParamMask const &requireMask=FanInParamMask()) : RealTimeMonadComponents<StateT>::template OneLevelDownKleisli<A,B>(), RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B>(), timeChecker_(requireMask) {
            }
            virtual ~OnOrderFacilityCore() {
            }
            virtual void handle(InnerData<Key<A>> &&data) override final {
                if (timeChecker_(data)) {
                    auto id = data.timedData.value.id();
                    WithTime<A,TimePoint> a {data.timedData.timePoint, data.timedData.value.key()};
                    auto res = this->action(data.environment, std::move(a));
                    if (res) {
                        Producer<KeyedData<A,B>>::publish(
                            pureInnerDataLift([id=std::move(id)](B &&b) -> Key<B> {
                                return {std::move(id), std::move(b)};
                            }, std::move(*res))
                        );
                    }
                }
            }
        };
        template <class A, class B, class F, bool Threaded>
        using PureOnOrderFacilityCore = typename RealTimeMonadComponents<StateT>::template OneLevelDownKleisliMixin<
                                A, B, F,
                                typename RealTimeMonadComponents<StateT>::template PureOneLevelDownKleisli<A,B,F,true>,
                                OnOrderFacilityCore<A,B,Threaded>
                                >;
        template <class A, class B, class F, bool Threaded>
        using MaybeOnOrderFacilityCore = typename RealTimeMonadComponents<StateT>::template OneLevelDownKleisliMixin<
                                A, B, F,
                                typename RealTimeMonadComponents<StateT>::template MaybeOneLevelDownKleisli<A,B,F,true>,
                                OnOrderFacilityCore<A,B,Threaded>
                                >;
        template <class A, class B, class F, bool Threaded>
        using EnhancedMaybeOnOrderFacilityCore = typename RealTimeMonadComponents<StateT>::template OneLevelDownKleisliMixin<
                                A, B, F,
                                typename RealTimeMonadComponents<StateT>::template EnhancedMaybeOneLevelDownKleisli<A,B,F,true>,
                                OnOrderFacilityCore<A,B,Threaded>
                                >;
        template <class A, class B, class F, bool Threaded>
        using KleisliOnOrderFacilityCore = typename RealTimeMonadComponents<StateT>::template OneLevelDownKleisliMixin<
                                A, B, F,
                                typename RealTimeMonadComponents<StateT>::template DirectOneLevelDownKleisli<A,B,F,true>,
                                OnOrderFacilityCore<A,B,Threaded>
                                >;
    public:
        template <class A, class B>
        using AbstractOnOrderFacility = typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B>;
        template <class A, class B>
        using OnOrderFacility = TwoWayHolder<AbstractOnOrderFacility<A,B>,A,B>;

        template <class A, class F>
        static auto liftPureOnOrderFacility(F &&f, bool threaded=false, FanInParamMask const &requireMask=FanInParamMask()) 
            -> std::shared_ptr<OnOrderFacility<A,decltype(f(A()))>> {
            if (threaded) {
                return std::make_shared<OnOrderFacility<A,decltype(f(A()))>>(new PureOnOrderFacilityCore<A,decltype(f(A())),F,true>(std::move(f), requireMask));
            } else {
                return std::make_shared<OnOrderFacility<A,decltype(f(A()))>>(new PureOnOrderFacilityCore<A,decltype(f(A())),F,false>(std::move(f), requireMask));
            }
        }     
        template <class A, class F>
        static auto liftMaybeOnOrderFacility(F &&f, bool threaded=false) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(A()))::value_type>> {
            if (threaded) {
                return std::make_shared<OnOrderFacility<A, typename decltype(f(A()))::value_type>>(new MaybeOnOrderFacilityCore<A,typename decltype(f(A()))::value_type,F,true>(std::move(f)));
            } else {
                return std::make_shared<OnOrderFacility<A, typename decltype(f(A()))::value_type>>(new MaybeOnOrderFacilityCore<A,typename decltype(f(A()))::value_type,F,false>(std::move(f)));
            }
        }
        template <class A, class F>
        static auto enhancedMaybeOnOrderFacility(F &&f, bool threaded=false) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>> {
            if (threaded) {
                return std::make_shared<OnOrderFacility<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>>(new EnhancedMaybeOnOrderFacilityCore<A,typename decltype(f(std::tuple<TimePoint,A>()))::value_type,F,true>(std::move(f)));
            } else {
                return std::make_shared<OnOrderFacility<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>>(new EnhancedMaybeOnOrderFacilityCore<A,typename decltype(f(std::tuple<TimePoint,A>()))::value_type,F,false>(std::move(f)));
            }
        }
        template <class A, class F>
        static auto kleisliOnOrderFacility(F &&f, bool threaded=false) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>> {
            if (threaded) {
                return std::make_shared<OnOrderFacility<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>>(
                    new KleisliOnOrderFacilityCore<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType, F, true>(std::move(f))
                );
            } else {
                return std::make_shared<OnOrderFacility<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>>(
                    new KleisliOnOrderFacilityCore<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType, F, false>(std::move(f))
                );
            }
        }

        template <class A, class B>
        static std::shared_ptr<Action<A,B>> fromAbstractAction(typename RealTimeMonadComponents<StateT>::template AbstractAction<A,B> *t) {
            return std::make_shared<Action<A,B>>(t);
        }
        template <class A, class B>
        static std::shared_ptr<OnOrderFacility<A,B>> fromAbstractOnOrderFacility(typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B> *t) {
            return std::make_shared<OnOrderFacility<A,B>>(t);
        }
    private:
        template <class I0, class O0, class I1, class O1>
        class WrappedOnOrderFacility final : public IExternalComponent, public RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<I0,O0> {
        private:
            OnOrderFacility<I1,O1> toWrap_;
            Action<Key<I0>,Key<I1>> inputT_;
            Action<Key<O1>,Key<O0>> outputT_;
            class Conduit1 final : public RealTimeMonadComponents<StateT>::template IHandler<Key<I1>> {
            private:
                typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<I1,O1> *toWrap_;
                typename RealTimeMonadComponents<StateT>::template IHandler<KeyedData<I1,O1>> *nextConduit_;
            public:
                Conduit1(
                    typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<I1,O1> *toWrap,
                    typename RealTimeMonadComponents<StateT>::template IHandler<KeyedData<I1,O1>> *nextConduit
                ) : toWrap_(toWrap), nextConduit_(nextConduit) {}
                void handle(InnerData<Key<I1>> &&i1) override final {
                    toWrap_->registerKeyHandler(i1.timedData.value, nextConduit_);
                    toWrap_->handle(std::move(i1));
                }
            };
            class Conduit2 final : public RealTimeMonadComponents<StateT>::template IHandler<KeyedData<I1,O1>> {
            private:
                typename RealTimeMonadComponents<StateT>::template AbstractAction<Key<O1>,Key<O0>> *outputT_;
            public:
                Conduit2(typename RealTimeMonadComponents<StateT>::template AbstractAction<Key<O1>,Key<O0>> *outputT)
                    : outputT_(outputT) {}
                void handle(InnerData<KeyedData<I1,O1>> &&o1) override final {
                    auto x = pureInnerDataLift([](KeyedData<I1,O1> &&a) -> Key<O1> {
                        return {a.key.id(), std::move(a.data)};
                    }, std::move(o1));
                    outputT_->handle(std::move(x));
                }
            }; 
            class Conduit3 final : public RealTimeMonadComponents<StateT>::template IHandler<Key<O0>> {
            private:
                WrappedOnOrderFacility *parent_;
            public:
                Conduit3(WrappedOnOrderFacility *parent)
                    : parent_(parent) {}
                void handle(InnerData<Key<O0>> &&o0) override final {
                    parent_->publish(std::move(o0));
                }
            };        
            Conduit3 c3_;   
            Conduit2 c2_;
            Conduit1 c1_;
        public:
            WrappedOnOrderFacility(
                OnOrderFacility<I1,O1> &&toWrap,
                Action<Key<I0>,Key<I1>> &&inputT,
                Action<Key<O1>,Key<O0>> &&outputT
            ) : toWrap_(std::move(toWrap)), inputT_(std::move(inputT)), outputT_(std::move(outputT)),
                c3_(this), c2_(outputT_.core_.get()), c1_(toWrap_.core_.get(), &c2_) {
                outputT_.core_->addHandler(&c3_);
                inputT_.core_->addHandler(&c1_);
            }
            virtual void start(StateT *env) override final {
                auto *p = dynamic_cast<IExternalComponent *>(toWrap_.core_.get());
                if (p != nullptr) {
                    p->start(env);
                }
            }
            virtual void handle(InnerData<Key<I0>> &&i0) override final {
                inputT_.core_->handle(std::move(i0));
            }
        };
    public:
        template <class I0, class O0, class I1, class O1>
        static std::shared_ptr<OnOrderFacility<I0,O0>> wrappedOnOrderFacility(OnOrderFacility<I1,O1> &&toWrap, Action<Key<I0>,Key<I1>> &&inputT, Action<Key<O1>,Key<O0>> &&outputT) {
            return std::make_shared<OnOrderFacility<I0,O0>>(
                new WrappedOnOrderFacility<I0,O0,I1,O1>(std::move(toWrap),std::move(inputT),std::move(outputT))
            );
        };
    private:
        template <class A, class B, class C>
        class Compose final : public RealTimeMonadComponents<StateT>::template AbstractAction<A,C> {
        private:
            std::unique_ptr<typename RealTimeMonadComponents<StateT>::template AbstractAction<A,B>> f_;
            std::unique_ptr<typename RealTimeMonadComponents<StateT>::template AbstractAction<B,C>> g_;
            class InnerHandler : public IHandler<C> {
            private:
                Producer<C> *p_;
            public:
                InnerHandler(Producer<C> *p) : p_(p) {}
                virtual void handle(InnerData<C> &&data) override final {
                    p_->publish(std::move(data));
                }
            };
            InnerHandler innerHandler_;
        protected:
            virtual void handle(InnerData<A> &&data) override final {
                f_->handle(std::move(data));
            }
        public:
            Compose() : f_(), g_(), innerHandler_(this) {}
            Compose(std::unique_ptr<typename RealTimeMonadComponents<StateT>::template AbstractAction<A,B>> &&f, std::unique_ptr<typename RealTimeMonadComponents<StateT>::template AbstractAction<B,C>> &&g) : f_(std::move(f)), g_(std::move(g)), innerHandler_(this) {
                f_->addHandler(g_.get());
                g_->addHandler(&innerHandler_);
            }
        };
    public:   
        template <class A, class B, class C>
        static std::shared_ptr<Action<A,C>> compose(Action<A,B> &&f, Action<B,C> &&g) {
            return std::make_shared<Action<A,C>>(new Compose<A,B,C>(std::move(f.core_), std::move(g.core_)));
        }

    #include <tm_kit/infra/RealTimeMonad_Merge_Piece.hpp>
    #include <tm_kit/infra/RealTimeMonad_PureN_Piece.hpp>
    #include <tm_kit/infra/RealTimeMonad_MaybeN_Piece.hpp>  
    #include <tm_kit/infra/RealTimeMonad_KleisliN_Piece.hpp>  

    public:
        template <class T>
        using AbstractImporter = typename RealTimeMonadComponents<StateT>::template AbstractImporter<T>;
        template <class T>
        using Importer = OneWayHolder<AbstractImporter<T>,T>;
        template <class T>
        class PublisherCall {
        private:
            Producer<T> *pub_;
            StateT *env_;
        public:
            PublisherCall(Producer<T> *p, StateT *env) : pub_(p), env_(env) {}
            inline void operator()(T &&data) const {
                pub_->publish(env_, std::move(data));
            }
        };
    private:
        template <class T, class F>
        class SimpleImporter final : public AbstractImporter<T> {
        private:
            F f_;
            bool threaded_;
        public:
            SimpleImporter(F &&f, bool threaded) : f_(std::move(f)), threaded_(threaded) {}
            virtual void start(StateT *env) override final {
                if (threaded_) {
                    auto pub = std::make_unique<PublisherCall<T>>(this, env);
                    std::thread th([this,pub=std::move(pub)]() {
                        f_(*(pub.get()));
                    });
                    th.detach();
                } else {
                    auto pub = std::make_unique<PublisherCall<T>>(this, env);
                    f_(*(pub.get()));
                }
                
            }
        };
    public:
        template <class T>
        static std::shared_ptr<Importer<T>> importer(AbstractImporter<T> *p) {
            return std::make_shared<Importer<T>>(p);
        }
        template <class T, class F>
        static std::shared_ptr<Importer<T>> simpleImporter(F &&f, bool threaded=false) {
            return std::make_shared<Importer<T>>(std::make_unique<SimpleImporter<T,F>>(std::move(f), threaded));
        }
    public:
        template <class T>
        using AbstractExporter = typename RealTimeMonadComponents<StateT>::template AbstractExporter<T>;
        template <class T>
        using Exporter = OneWayHolder<AbstractExporter<T>,T>;
    private:
        template <class T, class F, bool Threaded>
        class SimpleExporter {};
        template <class T, class F>
        class SimpleExporter<T,F,true> final : public virtual AbstractExporter<T>, public RealTimeMonadComponents<StateT>::template ThreadedHandler<T> {
        private:
            F f_;  
            virtual void actuallyHandle(InnerData<T> &&d) override final {
                if (!this->timeCheckGood(d)) {
                    return;
                }
                f_(std::move(d));
            }    
        public:
        #ifdef _MSC_VER
            SimpleExporter(F &&f) : f_(std::move(f)) {}
        #else
            SimpleExporter(F &&f) : AbstractExporter<T>(), RealTimeMonadComponents<StateT>::template ThreadedHandler<T>(), f_(std::move(f)) {}
        #endif            
            virtual ~SimpleExporter() {}
            virtual void start(StateT *) override final {}
        };
        template <class T, class F>
        class SimpleExporter<T,F,false> final : public virtual AbstractExporter<T> {
        private:
            F f_;     
        public:
        #ifdef _MSC_VER
            SimpleExporter(F &&f) : f_(std::move(f)) {}
        #else
            SimpleExporter(F &&f) : AbstractExporter<T>(), f_(std::move(f)) {}
        #endif
            virtual ~SimpleExporter() {}
            virtual void handle(InnerData<T> &&d) override final {
                f_(std::move(d));
            } 
            virtual void start(StateT *) override final {}
        };
    public:       
        template <class T>
        static std::shared_ptr<Exporter<T>> exporter(AbstractExporter<T> *p) {
            return std::make_shared<Exporter<T>>(p);
        }
        template <class T, class F>
        static std::shared_ptr<Exporter<T>> simpleExporter(F &&f, bool threaded=false) {
            if (threaded) {
                return std::make_shared<Exporter<T>>(std::make_unique<SimpleExporter<T,F,true>>(std::move(f)));
            } else {
                return std::make_shared<Exporter<T>>(std::make_unique<SimpleExporter<T,F,false>>(std::move(f)));
            }            
        }

    public:
        template <class T1, class T2>
        static std::shared_ptr<Importer<T2>> composeImporter(Importer<T1> &&orig, Action<T1,T2> &&post) {
            class LocalI final : public AbstractImporter<T2> {
            private:
                Importer<T1> orig_;
                Action<T1,T2> post_;
                class LocalH final : public RealTimeMonadComponents<StateT>::template IHandler<T2> {
                private:
                    LocalI *parent_;
                public:
                    LocalH(LocalI *parent) : parent_(parent) {}
                    virtual void handle(InnerData<T2> &&t2) override final {
                        parent_->publish(std::move(t2));
                    }
                };
                LocalH localH_;
            public:
                LocalI(Importer<T1> &&orig, Action<T1,T2> &&post)
                    : orig_(std::move(orig)), post_(std::move(post)), localH_(this)
                {
                    orig_.core_->addHandler(post_.core_.get());
                    post_.core_->addHandler(&localH_);
                }
                virtual ~LocalI() {}
                virtual void start(StateT *env) override final {
                    orig_.core_->start(env);
                }
            };
            return std::make_shared<Importer<T2>>(new LocalI(std::move(orig), std::move(post)));
        }
        template <class T1, class T2>
        static std::shared_ptr<Exporter<T1>> composeExporter(Action<T1,T2> &&pre, Exporter<T2> &&orig) {
            class LocalE final : public AbstractExporter<T1> {
            private:
                Action<T1,T2> pre_;
                Exporter<T2> orig_;
            public:
                LocalE(Action<T1,T2> &&pre, Exporter<T2> &&orig)
                    : pre_(std::move(pre)), orig_(std::move(orig))
                {
                    pre_.core_->addHandler(orig_.core_.get());
                }
                virtual ~LocalE() {}
                virtual void start(StateT *env) override final {
                    orig_.core_->start(env);
                }
                virtual void handle(InnerData<T1> &&d) override final {
                    pre_.core_->handle(std::move(d));
                } 
            };
            return std::make_shared<Exporter<T1>>(new LocalE(std::move(pre), std::move(orig)));
        }

    public:
        //The reason why LocalOnOrderFacility is defined as essentially 
        //a tuple instead of an integrated object (as in AbstractIntegratedLocalOnOrderFacility)
        //is to facilitate the separate composition of the facility branch
        //and the exporter branch. 
        //The downside of this is that LocalOnOrderFacility cannot manage the 
        //memory pointed to by the two pointers, since it doesn't know whether
        //they point to the same object or to two different objects. However,
        //since all these objects are "functional" objects, and are supposed to
        //last the whole life of the program, this is a small price to pay. In
        //any case, even with the other types that use unique_ptr to manage their
        //inner objects, those pointers usually are either passed in from heap
        //(admittedly, the user can pass in a stack pointer, and then cause 
        //undefined behavior when the stack disappears, but we cannot control
        //everything the user does), or created internally, and still will last
        //the whole life of the program.
        template <class QueryKeyType, class QueryResultType, class DataInputType>
        using LocalOnOrderFacility = ThreeWayHolder<
            typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<QueryKeyType,QueryResultType>,QueryKeyType,QueryResultType
            , AbstractExporter<DataInputType>,DataInputType
        >;
        template <class QueryKeyType, class QueryResultType, class DataInputType>
        static std::shared_ptr<LocalOnOrderFacility<QueryKeyType, QueryResultType, DataInputType>> localOnOrderFacility(
            typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<QueryKeyType, QueryResultType> *t
            , AbstractExporter<DataInputType> *e) {
            return std::make_shared<LocalOnOrderFacility<QueryKeyType, QueryResultType, DataInputType>>(t,e);
        }
        
        template <class QueryKeyType, class QueryResultType, class DataInputType>
        class AbstractIntegratedLocalOnOrderFacility 
            : public AbstractOnOrderFacility<QueryKeyType,QueryResultType>, public AbstractExporter<DataInputType> 
        {};
        template <class QueryKeyType, class QueryResultType, class DataInputType>
        static std::shared_ptr<LocalOnOrderFacility<QueryKeyType, QueryResultType, DataInputType>> localOnOrderFacility(
            AbstractIntegratedLocalOnOrderFacility<QueryKeyType, QueryResultType, DataInputType> *p) {
            return std::make_shared<LocalOnOrderFacility<QueryKeyType, QueryResultType, DataInputType>>(p,p);
        }
        
        template <class Fac, class Exp>
        static std::shared_ptr<LocalOnOrderFacility<
            typename Fac::InputType
            , typename Fac::OutputType
            , typename Exp::DataType>> localOnOrderFacility(
            Fac &&t, Exp &&e) {
            auto *p_t = t.core_.get();
            auto *p_e = e.core_.get();
            t.release();
            e.release();
            return std::make_shared<LocalOnOrderFacility<
                typename Fac::InputType
                , typename Fac::OutputType
                , typename Exp::DataType>>(p_t,p_e);
        }

        template <class Fac, class Action1, class Action2
            , std::enable_if_t<std::is_same_v<typename Action1::OutputType::KeyType, typename Fac::InputType>,int> = 0
            , std::enable_if_t<std::is_same_v<typename Action2::InputType::KeyType, typename Fac::OutputType>,int> = 0
            >
        static std::shared_ptr<LocalOnOrderFacility<
            typename Action1::InputType::KeyType
            , typename Action2::OutputType::KeyType
            , typename Fac::DataType>> wrappedLocalOnOrderFacility(Fac &&toWrap, Action1 &&inputT, Action2 &&outputT) {
            auto *t = toWrap.core1_;
            auto *e = toWrap.core2_;
            toWrap.release();
            auto fac = fromAbstractOnOrderFacility(t);
            auto fac1 = wrappedOnOrderFacility<
                typename Action1::InputType::KeyType
                , typename Action2::OutputType::KeyType
                , typename Action1::OutputType::KeyType
                , typename Action2::InputType::KeyType
            >(std::move(*fac), std::move(inputT), std::move(outputT));
            auto *p = fac1->core_.get();
            fac1->release();
            return std::make_shared<LocalOnOrderFacility<
                typename Action1::InputType::KeyType
                , typename Action2::OutputType::KeyType
                , typename Fac::DataType>
            >(
                p, e
            );
        }

    public:
        template <class T>
        class Source {
        private:
            friend class RealTimeMonad;
            Producer<T> *producer;
            Source(Producer<T> *p) : producer(p) {}
        public:
            Source clone() const {
                return Source {producer};
            }
        };
        template <class T>
        class Sink {
        private:
            friend class RealTimeMonad;
            IHandler<T> *consumer;
            Sink(IHandler<T> *c) : consumer(c) {}
        };           
            
    private:  
        std::list<IExternalComponent *> externalComponents_[3];
        std::unordered_set<IExternalComponent *> externalComponentsSet_;
        std::mutex mutex_;
        RealTimeMonad() : externalComponents_(), externalComponentsSet_(), mutex_() {}
        ~RealTimeMonad() {}

        void registerExternalComponent(IExternalComponent *c, int idx) {
            if (c == nullptr) {
                throw RealTimeMonadException(
                    "Cannot register an external component of null"
                );
            }
            std::lock_guard<std::mutex> _(mutex_);
            if (externalComponentsSet_.find(c) == externalComponentsSet_.end()) {
                externalComponents_[idx].push_back(c);
                externalComponentsSet_.insert(c);
            }
        }

        template <class A>
        static void innerConnect(IHandler<A> *handler, Producer<A> *producer) {
            producer->addHandler(handler);
        }
        template <class A, class B>
        static void innerConnectFacility(Producer<Key<A>> *producer, typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B> *facility, IHandler<KeyedData<A,B>> *consumer) {
            class LocalC final : public virtual IHandler<Key<A>> {
            private:                    
                typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B> *p_;
                IHandler<KeyedData<A,B>> *h_;
            public:
                LocalC(typename RealTimeMonadComponents<StateT>::template AbstractOnOrderFacility<A,B> *p, IHandler<KeyedData<A,B>> *h) : p_(p), h_(h) {}
                virtual void handle(InnerData<Key<A>> &&k) {
                    p_->registerKeyHandler(k.timedData.value, h_);
                    p_->handle(std::move(k));
                }
            } *localC = new LocalC(facility, consumer);
            producer->addHandler(localC);
        }
    private:
        template <class T>
        Source<T> importerAsSource(StateT *env, Importer<T> &importer) {
            registerExternalComponent(dynamic_cast<IExternalComponent *>(importer.core_.get()), 2);
            return {dynamic_cast<Producer<T> *>(importer.core_.get())};
        }
        template <class A, class B>
        Source<B> actionAsSource(StateT *env, Action<A,B> &action) {
            return {dynamic_cast<Producer<B> *>(action.core_.get())};
        }
        template <class A, class B>
        Source<B> execute(Action<A,B> &action, Source<A> &&variable) {
            innerConnect(dynamic_cast<IHandler<A> *>(action.core_.get()), variable.producer);
            return {dynamic_cast<Producer<B> *>(action.core_.get())};
        }

        #include <tm_kit/infra/RealTimeMonad_ExecuteAction_Piece.hpp>
 
        template <class T>
        Sink<T> exporterAsSink(Exporter<T> &exporter) {
            registerExternalComponent(dynamic_cast<IExternalComponent *>(exporter.core_.get()), 0);
            return {dynamic_cast<IHandler<T> *>(exporter.core_.get())};
        }
        template <class A, class B>
        Sink<A> actionAsSink(Action<A,B> &action) {
            return {dynamic_cast<IHandler<A> *>(action.core_.get())};
        }

        #include <tm_kit/infra/RealTimeMonad_VariantSink_Piece.hpp>

        template <class A, class B>
        void placeOrderWithFacility(Source<Key<A>> &&input, OnOrderFacility<A,B> &facility, Sink<KeyedData<A,B>> const &sink) {
            auto *p = dynamic_cast<IExternalComponent *>(facility.core_.get());
            if (p != nullptr) {
                registerExternalComponent(p, 1);
            } 
            innerConnectFacility(input.producer, facility.core_.get(), sink.consumer);
        }  
        template <class A, class B>
        void placeOrderWithFacilityAndForget(Source<Key<A>> &&input, OnOrderFacility<A,B> &facility) {
            auto *p = dynamic_cast<IExternalComponent *>(facility.core_.get());
            if (p != nullptr) {
                registerExternalComponent(p, 1);
            } 
            innerConnectFacility(input.producer, facility.core_.get(), (IHandler<KeyedData<A,B>> *) nullptr);
        }

        template <class A, class B, class C>
        void placeOrderWithLocalFacility(Source<Key<A>> &&input, LocalOnOrderFacility<A,B,C> &facility, Sink<KeyedData<A,B>> const &sink) {
            auto *p = dynamic_cast<IExternalComponent *>(facility.core1_);
            if (p != nullptr) {
                registerExternalComponent(p, 1);
            } 
            innerConnectFacility(input.producer, facility.core1_, sink.consumer);
         } 
        template <class A, class B, class C>
        void placeOrderWithLocalFacilityAndForget(Source<Key<A>> &&input, LocalOnOrderFacility<A,B,C> &facility) {
            auto *p = dynamic_cast<IExternalComponent *>(facility.core1_);
            if (p != nullptr) {
                registerExternalComponent(p, 1);
            } 
            innerConnectFacility(input.producer, facility.core1_, (IHandler<KeyedData<A,B>> *) nullptr);
        } 
        template <class A, class B, class C>
        Sink<C> localFacilityAsSink(LocalOnOrderFacility<A,B,C> &facility) {
            registerExternalComponent(dynamic_cast<IExternalComponent *>(facility.core2_), 0);
            return {dynamic_cast<IHandler<C> *>(facility.core2_)};
        }

        template <class T>
        void connect(Source<T> &&src, Sink<T> const &sink) {
            innerConnect(sink.consumer, src.producer);
        }

        std::function<void(StateT *)> finalize() { 
            std::list<IExternalComponent *> aCopy;
            {
                std::lock_guard<std::mutex> _(mutex_);
                std::copy(externalComponents_[0].begin(), externalComponents_[0].end(), std::back_inserter(aCopy));
                std::copy(externalComponents_[1].begin(), externalComponents_[1].end(), std::back_inserter(aCopy));
                std::copy(externalComponents_[2].begin(), externalComponents_[2].end(), std::back_inserter(aCopy));
            }  
            return [aCopy=std::move(aCopy)](StateT *env) {
                for (auto c : aCopy) {
                    c->start(env);
                }
            };        
        }
    public:
        template <class X>
        struct GetDataType {
            using DataType = typename X::DataType;
        };
        template <class X>
        struct GetInputOutputType {
            using InputType = typename X::InputType;
            using OutputType = typename X::OutputType;
        };
    };

} } } }

#endif
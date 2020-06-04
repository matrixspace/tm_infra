#ifndef TM_KIT_INFRA_BASIC_WITHTIME_MONAD_HPP_
#define TM_KIT_INFRA_BASIC_WITHTIME_MONAD_HPP_

#include <future>
#include <tm_kit/infra/WithTimeData.hpp>

namespace dev { namespace cd606 { namespace tm { namespace infra {
    template <class StateT>
    class BasicWithTimeMonad {
    private:
        friend class MonadRunner<BasicWithTimeMonad>;
        BasicWithTimeMonad() = default;
        ~BasicWithTimeMonad() = default;
    public:
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
        using Data = TimedMonadData<T,StateT>;

        //we don't allow any action to manufacture KeyedData "out of the blue"
        //, but it is ok to manipulate Keys, so the check is one-sided
        //Moreover, we allow manipulation of keyed datas
        template <class A, class B, std::enable_if_t<!is_keyed_data_v<B> || is_keyed_data_v<A>, int> = 0>
        using Action = TimedMonadModelKleisli<A,B,StateT>;

        template <class A, class B>
        using OnOrderFacility = TimedMonadModelKleisli<Key<A>,KeyedData<A,B>,StateT>;

        template <class T1, class T2, class T3>
        static std::shared_ptr<Action<T1,T3>> compose(Action<T1,T2> &&f1, Action<T2,T3> &&f2) {
            return std::make_shared<Action<T1,T3>>();
        }

        //This lifts A->B to Action<A,B>
        template <class A, class F>
        static auto liftPure(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<Action<A, decltype(f(A()))>> {
            return std::make_shared<Action<A, decltype(f(A()))>>();
        }
        //This lifts A->optional<B> to Action<A,B>
        template <class A, class F>
        static auto liftMaybe(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<Action<A, typename decltype(f(A()))::value_type>> {
            return std::make_shared<Action<A, typename decltype(f(A()))::value_type>>();
        }
        //This lifts (time,A)->optional<B> to Action<A,B>
        template <class A, class F>
        static auto enhancedMaybe(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<Action<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>> {
            return std::make_shared<Action<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>>();
        }
        //This lifts InnerData<A>->Data<B> to Action<A,B>
        template <class A, class F>
        static auto kleisli(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<Action<A, typename decltype(f(pureInnerData<A>(nullptr,A())))::value_type::ValueType>> {
            return std::make_shared<Action<A, typename decltype(f(pureInnerData<A>(nullptr,A())))::value_type::ValueType>>();
        }
        //this lifts A->B to OnOrderFacility<A,B>
        template <class A, class F>
        static auto liftPureOnOrderFacility(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, decltype(f(A()))>> {
            return std::make_shared<OnOrderFacility<A, decltype(f(A()))>>();
        }
        //this lifts A->optional<B> to OnOrderFacility<A,B>
        template <class A, class F>
        static auto liftMaybeOnOrderFacility(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(A()))::value_type>> {
            return std::make_shared<OnOrderFacility<A, typename decltype(f(A()))::value_type>>();
        }
        //this lifts (time,A)->optional<B> to OnOrderFacility<A,B>
        template <class A, class F>
        static auto enhancedMaybeOnOrderFacility(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>> {
            return std::make_shared<OnOrderFacility<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>>();
        }
        //This lifts InnerData<A>->Data<B> to Action<A,B>
        template <class A, class F>
        static auto kleisliOnOrderFacility(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>> {
            return std::make_shared<OnOrderFacility<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>>();
        }
        //this lifts A->B to OnOrderFacility<A,B>
        template <class A, class F, class StartF>
        static auto liftPureOnOrderFacilityWithStart(F &&f, StartF &&startF, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, decltype(f(A()))>> {
            return std::make_shared<OnOrderFacility<A, decltype(f(A()))>>();
        }
        //this lifts A->optional<B> to OnOrderFacility<A,B>
        template <class A, class F, class StartF>
        static auto liftMaybeOnOrderFacilityWithStart(F &&f, StartF &&startF, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(A()))::value_type>> {
            return std::make_shared<OnOrderFacility<A, typename decltype(f(A()))::value_type>>();
        }
        //this lifts (time,A)->optional<B> to OnOrderFacility<A,B>
        template <class A, class F, class StartF>
        static auto enhancedMaybeOnOrderFacilityWithStart(F &&f, StartF &&startF, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>> {
            return std::make_shared<OnOrderFacility<A, typename decltype(f(std::tuple<TimePoint,A>()))::value_type>>();
        }
        //This lifts InnerData<A>->Data<B> to Action<A,B>
        template <class A, class F, class StartF>
        static auto kleisliOnOrderFacilityWithStart(F &&f, StartF &&startF, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) 
            -> std::shared_ptr<OnOrderFacility<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>> {
            return std::make_shared<OnOrderFacility<A, typename decltype(f(pureInnerData(nullptr,A())))::value_type::ValueType>>();
        }

        template <class I0, class O0, class I1, class O1>
        static auto wrappedOnOrderFacility(OnOrderFacility<I1,O1> &&toWrap, Action<Key<I0>,Key<I1>> &&inputT, Action<Key<O1>,Key<O0>> &&outputT) 
            -> std::shared_ptr<OnOrderFacility<I0,O0>> {
            return std::make_shared<OnOrderFacility<I0,O0>>();
        }
    
    #include <tm_kit/infra/BasicWithTimeMonad_VariantAndMerge_Piece.hpp>
    #include <tm_kit/infra/BasicWithTimeMonad_Pure_Maybe_Kleisli_Piece.hpp>
    
    public:   
        template <class T>
        class Source {
        private:
            friend class BasicWithTimeMonad;
            Data<T> data;
            Source() : data(std::nullopt) {}
            Source(Data<T> &&d) : data(std::move(d)) {}
        public:
            Source clone() const {
                return Source {
                    withtime_utils::clone(data)
                };
            }
        };
        template <class T>
        class Sink {
        private:
            friend class BasicWithTimeMonad;
            std::function<void(Source<T> &&)> action;
            Sink() : action() {}
            Sink(std::function<void(Source<T> &&)> f) : action(f) {}
        };

        //KeyedData cannot be imported "out of the blue"
        template <class T, std::enable_if_t<!is_keyed_data_v<T>,int> = 0>
        using Importer = std::function<Data<T>(StateT * const)>;

        //Keys and KeyedData can be exported, for example to be written to database,
        //so there is no check on the exporter
        template <class T>
        using Exporter = std::function<void(InnerData<T> &&)>;

        template <class T>
        static std::shared_ptr<Importer<T>> vacuousImporter() {
            return std::make_shared<Importer<T>>();
        }
        template <class T, class F>
        static std::shared_ptr<Importer<T>> simpleImporter(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) {
            return std::make_shared<Importer<T>>();
        }
        template <class T, class F>
        static std::shared_ptr<Exporter<T>> simpleExporter(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) {
            return std::make_shared<Exporter<T>>();
        }
        template <class T, class F>
        static std::shared_ptr<Exporter<T>> pureExporter(F &&f, LiftParameters<TimePoint> const &liftParam = LiftParameters<TimePoint>()) {
            return std::make_shared<Exporter<T>>();
        }
        template <class T>
        static std::shared_ptr<Exporter<T>> trivialExporter() {
            return std::make_shared<Exporter<T>>();
        }          

        template <class T1, class T2>
        static std::shared_ptr<Importer<T2>> composeImporter(Importer<T1> &&orig, Action<T1,T2> &&post) {
            return std::make_shared<Importer<T2>>();
        }
        template <class T1, class T2>
        static std::shared_ptr<Exporter<T1>> composeExporter(Action<T1,T2> &&pre, Exporter<T2> &&orig) {
            return std::make_shared<Exporter<T1>>();
        }
    
    public:
        template <class A, class B, class C>
        struct LocalOnOrderFacility {
            OnOrderFacility<A,B> facility;
            Exporter<C> exporter;
        };

        template <class A, class B, class C>
        static std::shared_ptr<LocalOnOrderFacility<A,B,C>> localOnOrderFacility(
            OnOrderFacility<A,B> &&facility, Exporter<C> &&exporter
        ) {
            return std::make_shared<LocalOnOrderFacility<A,B,C>>();
        }
        
        template <class I0, class O0, class I1, class O1, class C>
        static std::shared_ptr<LocalOnOrderFacility<I0,O0,C>> wrappedLocalOnOrderFacility(LocalOnOrderFacility<I1,O1,C> &&toWrap, Action<Key<I0>,Key<I1>> &&inputT, Action<Key<O1>,Key<O0>> &&outputT) {
            return std::make_shared<LocalOnOrderFacility<I0,O0,C>>();
        }
   
    private:
        template <class T>
        Source<T> importerAsSource(StateT *env, Importer<T> &importer) {
            return {};
        }
        template <class A, class B>
        Source<B> actionAsSource(StateT *env, Action<A,B> &action) {
            return {};
        }
        template <class A, class B>
        Source<B> execute(Action<A,B> &action, Source<A> &&variable) {
            return {};
        }

        #include <tm_kit/infra/BasicWithTimeMonad_ExecuteAction_Piece.hpp> 

        template <class T>
        Sink<T> exporterAsSink(Exporter<T> &exporter) {
            return {};
        }
        template <class A, class B>
        Sink<A> actionAsSink(Action<A,B> &action) {
            return {};
        }

        #include <tm_kit/infra/BasicWithTimeMonad_VariantSink_Piece.hpp>

        template <class A, class B>
        void placeOrderWithFacility(Source<Key<A>> &&input, OnOrderFacility<A,B> &facility, Sink<KeyedData<A,B>> const &sink) {
        }  
        template <class A, class B>
        void placeOrderWithFacilityAndForget(Source<Key<A>> &&input, OnOrderFacility<A,B> &facility) {
        }  

        template <class A, class B, class C>
        void placeOrderWithLocalFacility(Source<Key<A>> &&input, LocalOnOrderFacility<A,B,C> &facility, Sink<KeyedData<A,B>> const &sink) {
        } 
        template <class A, class B, class C>
        void placeOrderWithLocalFacilityAndForget(Source<Key<A>> &&input, LocalOnOrderFacility<A,B,C> &facility) {
        } 
        template <class A, class B, class C>
        Sink<C> localFacilityAsSink(LocalOnOrderFacility<A,B,C> &facility) {
            return {};
        }

        template <class T>
        void connect(Source<T> &&src, Sink<T> const &sink) {
        }

        std::function<void(StateT *)> finalize() {   
            return [](StateT *) {
            };        
        }

    public:
        template <class X>
        struct GetDataType {
        };
        template <class T>
        struct GetDataType<Importer<T>> {
            using DataType = T;
        };
        template <class T>
        struct GetDataType<Exporter<T>> {
            using DataType = T;
        };
        template <class A, class B, class C>
        struct GetDataType<LocalOnOrderFacility<A,B,C>> {
            using DataType = C;
        };
        template <class X>
        struct GetInputOutputType {
        };
        template <class A, class B>
        struct GetInputOutputType<Action<A,B>> {
            using InputType = A;
            using OutputType = B;
        };
        template <class A, class B>
        struct GetInputOutputType<OnOrderFacility<A,B>> {
            using InputType = A;
            using OutputType = B;
        };
        template <class A, class B, class C>
        struct GetInputOutputType<LocalOnOrderFacility<A,B,C>> {
            using InputType = A;
            using OutputType = B;
        };
    };

}}}}

#endif
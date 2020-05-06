template <class A0, class A1, class B>
class ActionCore<std::variant<A0,A1>,B> : public virtual AbstractActionCore<std::variant<A0,A1>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(2);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class B, class F>
class PureActionCore<std::variant<A0,A1>,B,F> final : public ActionCore<std::variant<A0,A1>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class B, class F>
class MaybeActionCore<std::variant<A0,A1>,B,F> final : public ActionCore<std::variant<A0,A1>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class B, class F>
class KleisliActionCore<std::variant<A0,A1>,B,F> final : public ActionCore<std::variant<A0,A1>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class B>
class ActionCore<std::variant<A0,A1,A2>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(3);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class B, class F>
class PureActionCore<std::variant<A0,A1,A2>,B,F> final : public ActionCore<std::variant<A0,A1,A2>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2>,B,F> final : public ActionCore<std::variant<A0,A1,A2>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2>,B,F> final : public ActionCore<std::variant<A0,A1,A2>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class A3, class B>
class ActionCore<std::variant<A0,A1,A2,A3>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2,A3>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual Consumer<A3>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    WithTime<A3,TimePoint> a3_;
    bool hasA3_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        auto cert3 = this->Consumer<A3>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(4);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (cert3.check()) {
            vec.push_back({3, fetchTimePointUnsafe(cert3)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 3:
            {
                auto produce = [cert3=std::move(cert3),updateIdx,this]() -> Data<B> {
                    Certificate<A3> cert3_copy = std::move(cert3);
                    auto x3 = this->Consumer<A3>::source()->next(std::move(cert3_copy));
                    if (!x3) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA3_ || x3->timedData.timePoint >= a3_.timePoint) {
                        a3_ = std::move(x3->timedData);
                        hasA3_ = true;
                        StateT *env = x3->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&, WithTime<A3,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), a3_(), hasA3_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class A3, class B, class F>
class PureActionCore<std::variant<A0,A1,A2,A3>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class A3, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2,A3>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class A3, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2,A3>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) }, InnerData<A3> { env, std::move(a3) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class B>
class ActionCore<std::variant<A0,A1,A2,A3,A4>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2,A3,A4>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual Consumer<A3>, public virtual Consumer<A4>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    WithTime<A3,TimePoint> a3_;
    bool hasA3_;
    WithTime<A4,TimePoint> a4_;
    bool hasA4_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        auto cert3 = this->Consumer<A3>::source()->poll();
        auto cert4 = this->Consumer<A4>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(5);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (cert3.check()) {
            vec.push_back({3, fetchTimePointUnsafe(cert3)});
        }
        if (cert4.check()) {
            vec.push_back({4, fetchTimePointUnsafe(cert4)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 3:
            {
                auto produce = [cert3=std::move(cert3),updateIdx,this]() -> Data<B> {
                    Certificate<A3> cert3_copy = std::move(cert3);
                    auto x3 = this->Consumer<A3>::source()->next(std::move(cert3_copy));
                    if (!x3) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA3_ || x3->timedData.timePoint >= a3_.timePoint) {
                        a3_ = std::move(x3->timedData);
                        hasA3_ = true;
                        StateT *env = x3->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 4:
            {
                auto produce = [cert4=std::move(cert4),updateIdx,this]() -> Data<B> {
                    Certificate<A4> cert4_copy = std::move(cert4);
                    auto x4 = this->Consumer<A4>::source()->next(std::move(cert4_copy));
                    if (!x4) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA4_ || x4->timedData.timePoint >= a4_.timePoint) {
                        a4_ = std::move(x4->timedData);
                        hasA4_ = true;
                        StateT *env = x4->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&, WithTime<A3,TimePoint> &&, WithTime<A4,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), a3_(), hasA3_(false), a4_(), hasA4_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class A3, class A4, class B, class F>
class PureActionCore<std::variant<A0,A1,A2,A3,A4>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2,A3,A4>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2,A3,A4>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) }, InnerData<A3> { env, std::move(a3) }, InnerData<A4> { env, std::move(a4) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class B>
class ActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual Consumer<A3>, public virtual Consumer<A4>, public virtual Consumer<A5>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    WithTime<A3,TimePoint> a3_;
    bool hasA3_;
    WithTime<A4,TimePoint> a4_;
    bool hasA4_;
    WithTime<A5,TimePoint> a5_;
    bool hasA5_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        auto cert3 = this->Consumer<A3>::source()->poll();
        auto cert4 = this->Consumer<A4>::source()->poll();
        auto cert5 = this->Consumer<A5>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(6);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (cert3.check()) {
            vec.push_back({3, fetchTimePointUnsafe(cert3)});
        }
        if (cert4.check()) {
            vec.push_back({4, fetchTimePointUnsafe(cert4)});
        }
        if (cert5.check()) {
            vec.push_back({5, fetchTimePointUnsafe(cert5)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 3:
            {
                auto produce = [cert3=std::move(cert3),updateIdx,this]() -> Data<B> {
                    Certificate<A3> cert3_copy = std::move(cert3);
                    auto x3 = this->Consumer<A3>::source()->next(std::move(cert3_copy));
                    if (!x3) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA3_ || x3->timedData.timePoint >= a3_.timePoint) {
                        a3_ = std::move(x3->timedData);
                        hasA3_ = true;
                        StateT *env = x3->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 4:
            {
                auto produce = [cert4=std::move(cert4),updateIdx,this]() -> Data<B> {
                    Certificate<A4> cert4_copy = std::move(cert4);
                    auto x4 = this->Consumer<A4>::source()->next(std::move(cert4_copy));
                    if (!x4) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA4_ || x4->timedData.timePoint >= a4_.timePoint) {
                        a4_ = std::move(x4->timedData);
                        hasA4_ = true;
                        StateT *env = x4->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 5:
            {
                auto produce = [cert5=std::move(cert5),updateIdx,this]() -> Data<B> {
                    Certificate<A5> cert5_copy = std::move(cert5);
                    auto x5 = this->Consumer<A5>::source()->next(std::move(cert5_copy));
                    if (!x5) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA5_ || x5->timedData.timePoint >= a5_.timePoint) {
                        a5_ = std::move(x5->timedData);
                        hasA5_ = true;
                        StateT *env = x5->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&, WithTime<A3,TimePoint> &&, WithTime<A4,TimePoint> &&, WithTime<A5,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), a3_(), hasA3_(false), a4_(), hasA4_(false), a5_(), hasA5_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class B, class F>
class PureActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) }, InnerData<A3> { env, std::move(a3) }, InnerData<A4> { env, std::move(a4) }, InnerData<A5> { env, std::move(a5) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class B>
class ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual Consumer<A3>, public virtual Consumer<A4>, public virtual Consumer<A5>, public virtual Consumer<A6>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    WithTime<A3,TimePoint> a3_;
    bool hasA3_;
    WithTime<A4,TimePoint> a4_;
    bool hasA4_;
    WithTime<A5,TimePoint> a5_;
    bool hasA5_;
    WithTime<A6,TimePoint> a6_;
    bool hasA6_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        auto cert3 = this->Consumer<A3>::source()->poll();
        auto cert4 = this->Consumer<A4>::source()->poll();
        auto cert5 = this->Consumer<A5>::source()->poll();
        auto cert6 = this->Consumer<A6>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(7);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (cert3.check()) {
            vec.push_back({3, fetchTimePointUnsafe(cert3)});
        }
        if (cert4.check()) {
            vec.push_back({4, fetchTimePointUnsafe(cert4)});
        }
        if (cert5.check()) {
            vec.push_back({5, fetchTimePointUnsafe(cert5)});
        }
        if (cert6.check()) {
            vec.push_back({6, fetchTimePointUnsafe(cert6)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 3:
            {
                auto produce = [cert3=std::move(cert3),updateIdx,this]() -> Data<B> {
                    Certificate<A3> cert3_copy = std::move(cert3);
                    auto x3 = this->Consumer<A3>::source()->next(std::move(cert3_copy));
                    if (!x3) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA3_ || x3->timedData.timePoint >= a3_.timePoint) {
                        a3_ = std::move(x3->timedData);
                        hasA3_ = true;
                        StateT *env = x3->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 4:
            {
                auto produce = [cert4=std::move(cert4),updateIdx,this]() -> Data<B> {
                    Certificate<A4> cert4_copy = std::move(cert4);
                    auto x4 = this->Consumer<A4>::source()->next(std::move(cert4_copy));
                    if (!x4) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA4_ || x4->timedData.timePoint >= a4_.timePoint) {
                        a4_ = std::move(x4->timedData);
                        hasA4_ = true;
                        StateT *env = x4->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 5:
            {
                auto produce = [cert5=std::move(cert5),updateIdx,this]() -> Data<B> {
                    Certificate<A5> cert5_copy = std::move(cert5);
                    auto x5 = this->Consumer<A5>::source()->next(std::move(cert5_copy));
                    if (!x5) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA5_ || x5->timedData.timePoint >= a5_.timePoint) {
                        a5_ = std::move(x5->timedData);
                        hasA5_ = true;
                        StateT *env = x5->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 6:
            {
                auto produce = [cert6=std::move(cert6),updateIdx,this]() -> Data<B> {
                    Certificate<A6> cert6_copy = std::move(cert6);
                    auto x6 = this->Consumer<A6>::source()->next(std::move(cert6_copy));
                    if (!x6) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA6_ || x6->timedData.timePoint >= a6_.timePoint) {
                        a6_ = std::move(x6->timedData);
                        hasA6_ = true;
                        StateT *env = x6->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&, WithTime<A3,TimePoint> &&, WithTime<A4,TimePoint> &&, WithTime<A5,TimePoint> &&, WithTime<A6,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), a3_(), hasA3_(false), a4_(), hasA4_(false), a5_(), hasA5_(false), a6_(), hasA6_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class B, class F>
class PureActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) }, InnerData<A3> { env, std::move(a3) }, InnerData<A4> { env, std::move(a4) }, InnerData<A5> { env, std::move(a5) }, InnerData<A6> { env, std::move(a6) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class B>
class ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual Consumer<A3>, public virtual Consumer<A4>, public virtual Consumer<A5>, public virtual Consumer<A6>, public virtual Consumer<A7>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    WithTime<A3,TimePoint> a3_;
    bool hasA3_;
    WithTime<A4,TimePoint> a4_;
    bool hasA4_;
    WithTime<A5,TimePoint> a5_;
    bool hasA5_;
    WithTime<A6,TimePoint> a6_;
    bool hasA6_;
    WithTime<A7,TimePoint> a7_;
    bool hasA7_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        auto cert3 = this->Consumer<A3>::source()->poll();
        auto cert4 = this->Consumer<A4>::source()->poll();
        auto cert5 = this->Consumer<A5>::source()->poll();
        auto cert6 = this->Consumer<A6>::source()->poll();
        auto cert7 = this->Consumer<A7>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(8);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (cert3.check()) {
            vec.push_back({3, fetchTimePointUnsafe(cert3)});
        }
        if (cert4.check()) {
            vec.push_back({4, fetchTimePointUnsafe(cert4)});
        }
        if (cert5.check()) {
            vec.push_back({5, fetchTimePointUnsafe(cert5)});
        }
        if (cert6.check()) {
            vec.push_back({6, fetchTimePointUnsafe(cert6)});
        }
        if (cert7.check()) {
            vec.push_back({7, fetchTimePointUnsafe(cert7)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 3:
            {
                auto produce = [cert3=std::move(cert3),updateIdx,this]() -> Data<B> {
                    Certificate<A3> cert3_copy = std::move(cert3);
                    auto x3 = this->Consumer<A3>::source()->next(std::move(cert3_copy));
                    if (!x3) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA3_ || x3->timedData.timePoint >= a3_.timePoint) {
                        a3_ = std::move(x3->timedData);
                        hasA3_ = true;
                        StateT *env = x3->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 4:
            {
                auto produce = [cert4=std::move(cert4),updateIdx,this]() -> Data<B> {
                    Certificate<A4> cert4_copy = std::move(cert4);
                    auto x4 = this->Consumer<A4>::source()->next(std::move(cert4_copy));
                    if (!x4) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA4_ || x4->timedData.timePoint >= a4_.timePoint) {
                        a4_ = std::move(x4->timedData);
                        hasA4_ = true;
                        StateT *env = x4->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 5:
            {
                auto produce = [cert5=std::move(cert5),updateIdx,this]() -> Data<B> {
                    Certificate<A5> cert5_copy = std::move(cert5);
                    auto x5 = this->Consumer<A5>::source()->next(std::move(cert5_copy));
                    if (!x5) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA5_ || x5->timedData.timePoint >= a5_.timePoint) {
                        a5_ = std::move(x5->timedData);
                        hasA5_ = true;
                        StateT *env = x5->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 6:
            {
                auto produce = [cert6=std::move(cert6),updateIdx,this]() -> Data<B> {
                    Certificate<A6> cert6_copy = std::move(cert6);
                    auto x6 = this->Consumer<A6>::source()->next(std::move(cert6_copy));
                    if (!x6) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA6_ || x6->timedData.timePoint >= a6_.timePoint) {
                        a6_ = std::move(x6->timedData);
                        hasA6_ = true;
                        StateT *env = x6->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 7:
            {
                auto produce = [cert7=std::move(cert7),updateIdx,this]() -> Data<B> {
                    Certificate<A7> cert7_copy = std::move(cert7);
                    auto x7 = this->Consumer<A7>::source()->next(std::move(cert7_copy));
                    if (!x7) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA7_ || x7->timedData.timePoint >= a7_.timePoint) {
                        a7_ = std::move(x7->timedData);
                        hasA7_ = true;
                        StateT *env = x7->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&, WithTime<A3,TimePoint> &&, WithTime<A4,TimePoint> &&, WithTime<A5,TimePoint> &&, WithTime<A6,TimePoint> &&, WithTime<A7,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), a3_(), hasA3_(false), a4_(), hasA4_(false), a5_(), hasA5_(false), a6_(), hasA6_(false), a7_(), hasA7_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class B, class F>
class PureActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        case 7:
            tp = a7.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value), std::move(a7.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag && a7.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        case 7:
            tp = a7.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value), std::move(a7.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag && a7.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) }, InnerData<A3> { env, std::move(a3) }, InnerData<A4> { env, std::move(a4) }, InnerData<A5> { env, std::move(a5) }, InnerData<A6> { env, std::move(a6) }, InnerData<A7> { env, std::move(a7) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class B>
class ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual Consumer<A3>, public virtual Consumer<A4>, public virtual Consumer<A5>, public virtual Consumer<A6>, public virtual Consumer<A7>, public virtual Consumer<A8>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    WithTime<A3,TimePoint> a3_;
    bool hasA3_;
    WithTime<A4,TimePoint> a4_;
    bool hasA4_;
    WithTime<A5,TimePoint> a5_;
    bool hasA5_;
    WithTime<A6,TimePoint> a6_;
    bool hasA6_;
    WithTime<A7,TimePoint> a7_;
    bool hasA7_;
    WithTime<A8,TimePoint> a8_;
    bool hasA8_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        auto cert3 = this->Consumer<A3>::source()->poll();
        auto cert4 = this->Consumer<A4>::source()->poll();
        auto cert5 = this->Consumer<A5>::source()->poll();
        auto cert6 = this->Consumer<A6>::source()->poll();
        auto cert7 = this->Consumer<A7>::source()->poll();
        auto cert8 = this->Consumer<A8>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(9);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (cert3.check()) {
            vec.push_back({3, fetchTimePointUnsafe(cert3)});
        }
        if (cert4.check()) {
            vec.push_back({4, fetchTimePointUnsafe(cert4)});
        }
        if (cert5.check()) {
            vec.push_back({5, fetchTimePointUnsafe(cert5)});
        }
        if (cert6.check()) {
            vec.push_back({6, fetchTimePointUnsafe(cert6)});
        }
        if (cert7.check()) {
            vec.push_back({7, fetchTimePointUnsafe(cert7)});
        }
        if (cert8.check()) {
            vec.push_back({8, fetchTimePointUnsafe(cert8)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 3:
            {
                auto produce = [cert3=std::move(cert3),updateIdx,this]() -> Data<B> {
                    Certificate<A3> cert3_copy = std::move(cert3);
                    auto x3 = this->Consumer<A3>::source()->next(std::move(cert3_copy));
                    if (!x3) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA3_ || x3->timedData.timePoint >= a3_.timePoint) {
                        a3_ = std::move(x3->timedData);
                        hasA3_ = true;
                        StateT *env = x3->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 4:
            {
                auto produce = [cert4=std::move(cert4),updateIdx,this]() -> Data<B> {
                    Certificate<A4> cert4_copy = std::move(cert4);
                    auto x4 = this->Consumer<A4>::source()->next(std::move(cert4_copy));
                    if (!x4) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA4_ || x4->timedData.timePoint >= a4_.timePoint) {
                        a4_ = std::move(x4->timedData);
                        hasA4_ = true;
                        StateT *env = x4->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 5:
            {
                auto produce = [cert5=std::move(cert5),updateIdx,this]() -> Data<B> {
                    Certificate<A5> cert5_copy = std::move(cert5);
                    auto x5 = this->Consumer<A5>::source()->next(std::move(cert5_copy));
                    if (!x5) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA5_ || x5->timedData.timePoint >= a5_.timePoint) {
                        a5_ = std::move(x5->timedData);
                        hasA5_ = true;
                        StateT *env = x5->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 6:
            {
                auto produce = [cert6=std::move(cert6),updateIdx,this]() -> Data<B> {
                    Certificate<A6> cert6_copy = std::move(cert6);
                    auto x6 = this->Consumer<A6>::source()->next(std::move(cert6_copy));
                    if (!x6) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA6_ || x6->timedData.timePoint >= a6_.timePoint) {
                        a6_ = std::move(x6->timedData);
                        hasA6_ = true;
                        StateT *env = x6->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 7:
            {
                auto produce = [cert7=std::move(cert7),updateIdx,this]() -> Data<B> {
                    Certificate<A7> cert7_copy = std::move(cert7);
                    auto x7 = this->Consumer<A7>::source()->next(std::move(cert7_copy));
                    if (!x7) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA7_ || x7->timedData.timePoint >= a7_.timePoint) {
                        a7_ = std::move(x7->timedData);
                        hasA7_ = true;
                        StateT *env = x7->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 8:
            {
                auto produce = [cert8=std::move(cert8),updateIdx,this]() -> Data<B> {
                    Certificate<A8> cert8_copy = std::move(cert8);
                    auto x8 = this->Consumer<A8>::source()->next(std::move(cert8_copy));
                    if (!x8) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA8_ || x8->timedData.timePoint >= a8_.timePoint) {
                        a8_ = std::move(x8->timedData);
                        hasA8_ = true;
                        StateT *env = x8->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&, WithTime<A3,TimePoint> &&, WithTime<A4,TimePoint> &&, WithTime<A5,TimePoint> &&, WithTime<A6,TimePoint> &&, WithTime<A7,TimePoint> &&, WithTime<A8,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), a3_(), hasA3_(false), a4_(), hasA4_(false), a5_(), hasA5_(false), a6_(), hasA6_(false), a7_(), hasA7_(false), a8_(), hasA8_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class B, class F>
class PureActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7, WithTime<A8,TimePoint> &&a8) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        case 7:
            tp = a7.timePoint;
            break;
        case 8:
            tp = a8.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value), std::move(a7.value), std::move(a8.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag && a7.finalFlag && a8.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7, WithTime<A8,TimePoint> &&a8) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        case 7:
            tp = a7.timePoint;
            break;
        case 8:
            tp = a8.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value), std::move(a7.value), std::move(a8.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag && a7.finalFlag && a8.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7, WithTime<A8,TimePoint> &&a8) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) }, InnerData<A3> { env, std::move(a3) }, InnerData<A4> { env, std::move(a4) }, InnerData<A5> { env, std::move(a5) }, InnerData<A6> { env, std::move(a6) }, InnerData<A7> { env, std::move(a7) }, InnerData<A8> { env, std::move(a8) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class B>
class ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B> : public virtual AbstractActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B>, public virtual Consumer<A0>, public virtual Consumer<A1>, public virtual Consumer<A2>, public virtual Consumer<A3>, public virtual Consumer<A4>, public virtual Consumer<A5>, public virtual Consumer<A6>, public virtual Consumer<A7>, public virtual Consumer<A8>, public virtual Consumer<A9>, public virtual BufferedProvider<B> {
private:
    WithTime<A0,TimePoint> a0_;
    bool hasA0_;
    WithTime<A1,TimePoint> a1_;
    bool hasA1_;
    WithTime<A2,TimePoint> a2_;
    bool hasA2_;
    WithTime<A3,TimePoint> a3_;
    bool hasA3_;
    WithTime<A4,TimePoint> a4_;
    bool hasA4_;
    WithTime<A5,TimePoint> a5_;
    bool hasA5_;
    WithTime<A6,TimePoint> a6_;
    bool hasA6_;
    WithTime<A7,TimePoint> a7_;
    bool hasA7_;
    WithTime<A8,TimePoint> a8_;
    bool hasA8_;
    WithTime<A9,TimePoint> a9_;
    bool hasA9_;
    FanInParamMask requireMask_;
protected:
    virtual typename BufferedProvider<B>::CheckAndProduceResult checkAndProduce() override final {
        auto cert0 = this->Consumer<A0>::source()->poll();
        auto cert1 = this->Consumer<A1>::source()->poll();
        auto cert2 = this->Consumer<A2>::source()->poll();
        auto cert3 = this->Consumer<A3>::source()->poll();
        auto cert4 = this->Consumer<A4>::source()->poll();
        auto cert5 = this->Consumer<A5>::source()->poll();
        auto cert6 = this->Consumer<A6>::source()->poll();
        auto cert7 = this->Consumer<A7>::source()->poll();
        auto cert8 = this->Consumer<A8>::source()->poll();
        auto cert9 = this->Consumer<A9>::source()->poll();
        std::vector<TimePointComparisonStruct> vec;
        vec.reserve(10);
        if (cert0.check()) {
            vec.push_back({0, fetchTimePointUnsafe(cert0)});
        }
        if (cert1.check()) {
            vec.push_back({1, fetchTimePointUnsafe(cert1)});
        }
        if (cert2.check()) {
            vec.push_back({2, fetchTimePointUnsafe(cert2)});
        }
        if (cert3.check()) {
            vec.push_back({3, fetchTimePointUnsafe(cert3)});
        }
        if (cert4.check()) {
            vec.push_back({4, fetchTimePointUnsafe(cert4)});
        }
        if (cert5.check()) {
            vec.push_back({5, fetchTimePointUnsafe(cert5)});
        }
        if (cert6.check()) {
            vec.push_back({6, fetchTimePointUnsafe(cert6)});
        }
        if (cert7.check()) {
            vec.push_back({7, fetchTimePointUnsafe(cert7)});
        }
        if (cert8.check()) {
            vec.push_back({8, fetchTimePointUnsafe(cert8)});
        }
        if (cert9.check()) {
            vec.push_back({9, fetchTimePointUnsafe(cert9)});
        }
        if (vec.empty()) {
            return std::nullopt;
        }
        std::sort(vec.begin(), vec.end());
        int updateIdx = vec[0].idx;
        switch (updateIdx) {
        case 0:
            {
                auto produce = [cert0=std::move(cert0),updateIdx,this]() -> Data<B> {
                    Certificate<A0> cert0_copy = std::move(cert0);
                    auto x0 = this->Consumer<A0>::source()->next(std::move(cert0_copy));
                    if (!x0) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA0_ || x0->timedData.timePoint >= a0_.timePoint) {
                        a0_ = std::move(x0->timedData);
                        hasA0_ = true;
                        StateT *env = x0->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 1:
            {
                auto produce = [cert1=std::move(cert1),updateIdx,this]() -> Data<B> {
                    Certificate<A1> cert1_copy = std::move(cert1);
                    auto x1 = this->Consumer<A1>::source()->next(std::move(cert1_copy));
                    if (!x1) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA1_ || x1->timedData.timePoint >= a1_.timePoint) {
                        a1_ = std::move(x1->timedData);
                        hasA1_ = true;
                        StateT *env = x1->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 2:
            {
                auto produce = [cert2=std::move(cert2),updateIdx,this]() -> Data<B> {
                    Certificate<A2> cert2_copy = std::move(cert2);
                    auto x2 = this->Consumer<A2>::source()->next(std::move(cert2_copy));
                    if (!x2) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA2_ || x2->timedData.timePoint >= a2_.timePoint) {
                        a2_ = std::move(x2->timedData);
                        hasA2_ = true;
                        StateT *env = x2->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 3:
            {
                auto produce = [cert3=std::move(cert3),updateIdx,this]() -> Data<B> {
                    Certificate<A3> cert3_copy = std::move(cert3);
                    auto x3 = this->Consumer<A3>::source()->next(std::move(cert3_copy));
                    if (!x3) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA3_ || x3->timedData.timePoint >= a3_.timePoint) {
                        a3_ = std::move(x3->timedData);
                        hasA3_ = true;
                        StateT *env = x3->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 4:
            {
                auto produce = [cert4=std::move(cert4),updateIdx,this]() -> Data<B> {
                    Certificate<A4> cert4_copy = std::move(cert4);
                    auto x4 = this->Consumer<A4>::source()->next(std::move(cert4_copy));
                    if (!x4) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA4_ || x4->timedData.timePoint >= a4_.timePoint) {
                        a4_ = std::move(x4->timedData);
                        hasA4_ = true;
                        StateT *env = x4->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 5:
            {
                auto produce = [cert5=std::move(cert5),updateIdx,this]() -> Data<B> {
                    Certificate<A5> cert5_copy = std::move(cert5);
                    auto x5 = this->Consumer<A5>::source()->next(std::move(cert5_copy));
                    if (!x5) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA5_ || x5->timedData.timePoint >= a5_.timePoint) {
                        a5_ = std::move(x5->timedData);
                        hasA5_ = true;
                        StateT *env = x5->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 6:
            {
                auto produce = [cert6=std::move(cert6),updateIdx,this]() -> Data<B> {
                    Certificate<A6> cert6_copy = std::move(cert6);
                    auto x6 = this->Consumer<A6>::source()->next(std::move(cert6_copy));
                    if (!x6) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA6_ || x6->timedData.timePoint >= a6_.timePoint) {
                        a6_ = std::move(x6->timedData);
                        hasA6_ = true;
                        StateT *env = x6->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 7:
            {
                auto produce = [cert7=std::move(cert7),updateIdx,this]() -> Data<B> {
                    Certificate<A7> cert7_copy = std::move(cert7);
                    auto x7 = this->Consumer<A7>::source()->next(std::move(cert7_copy));
                    if (!x7) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA7_ || x7->timedData.timePoint >= a7_.timePoint) {
                        a7_ = std::move(x7->timedData);
                        hasA7_ = true;
                        StateT *env = x7->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 8:
            {
                auto produce = [cert8=std::move(cert8),updateIdx,this]() -> Data<B> {
                    Certificate<A8> cert8_copy = std::move(cert8);
                    auto x8 = this->Consumer<A8>::source()->next(std::move(cert8_copy));
                    if (!x8) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA8_ || x8->timedData.timePoint >= a8_.timePoint) {
                        a8_ = std::move(x8->timedData);
                        hasA8_ = true;
                        StateT *env = x8->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        case 9:
            {
                auto produce = [cert9=std::move(cert9),updateIdx,this]() -> Data<B> {
                    Certificate<A9> cert9_copy = std::move(cert9);
                    auto x9 = this->Consumer<A9>::source()->next(std::move(cert9_copy));
                    if (!x9) {
                        return std::nullopt;
                    }
                    if (!StateT::CheckTime || !hasA9_ || x9->timedData.timePoint >= a9_.timePoint) {
                        a9_ = std::move(x9->timedData);
                        hasA9_ = true;
                        StateT *env = x9->environment;
                        bool good = (
                             (!requireMask_[0] || hasA0_) && 
                             (!requireMask_[1] || hasA1_) && 
                             (!requireMask_[2] || hasA2_) && 
                             (!requireMask_[3] || hasA3_) && 
                             (!requireMask_[4] || hasA4_) && 
                             (!requireMask_[5] || hasA5_) && 
                             (!requireMask_[6] || hasA6_) && 
                             (!requireMask_[7] || hasA7_) && 
                             (!requireMask_[8] || hasA8_) && 
                             (!requireMask_[9] || hasA9_)
                        );
                        if (good) {
                            return handle(updateIdx, env, withtime_utils::makeCopy(a0_), withtime_utils::makeCopy(a1_), withtime_utils::makeCopy(a2_), withtime_utils::makeCopy(a3_), withtime_utils::makeCopy(a4_), withtime_utils::makeCopy(a5_), withtime_utils::makeCopy(a6_), withtime_utils::makeCopy(a7_), withtime_utils::makeCopy(a8_), withtime_utils::makeCopy(a9_));
                        } else {
                            return std::nullopt;
                        }
                    }
                };
                return std::tuple<TimePoint, std::function<Data<B>()>> {vec[0].tp, produce};
            }
            break;
        default:
            return std::nullopt;
            break;
        }
    }
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&, WithTime<A1,TimePoint> &&, WithTime<A2,TimePoint> &&, WithTime<A3,TimePoint> &&, WithTime<A4,TimePoint> &&, WithTime<A5,TimePoint> &&, WithTime<A6,TimePoint> &&, WithTime<A7,TimePoint> &&, WithTime<A8,TimePoint> &&, WithTime<A9,TimePoint> &&) = 0;
public:
    ActionCore(FanInParamMask const &requireMask=FanInParamMask()) : a0_(), hasA0_(false), a1_(), hasA1_(false), a2_(), hasA2_(false), a3_(), hasA3_(false), a4_(), hasA4_(false), a5_(), hasA5_(false), a6_(), hasA6_(false), a7_(), hasA7_(false), a8_(), hasA8_(false), a9_(), hasA9_(false), requireMask_(requireMask) {
    }
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class B, class F>
class PureActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7, WithTime<A8,TimePoint> &&a8, WithTime<A9,TimePoint> &&a9) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        case 7:
            tp = a7.timePoint;
            break;
        case 8:
            tp = a8.timePoint;
            break;
        case 9:
            tp = a9.timePoint;
            break;
        default:
            return std::nullopt;
        }
        B b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value), std::move(a7.value), std::move(a8.value), std::move(a9.value));
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag && a7.finalFlag && a8.finalFlag && a9.finalFlag)
            }
        );
    }
public:
    PureActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~PureActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class B, class F>
class MaybeActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7, WithTime<A8,TimePoint> &&a8, WithTime<A9,TimePoint> &&a9) override final {
        TimePoint tp;
        switch (which) {
        case 0:
            tp = a0.timePoint;
            break;
        case 1:
            tp = a1.timePoint;
            break;
        case 2:
            tp = a2.timePoint;
            break;
        case 3:
            tp = a3.timePoint;
            break;
        case 4:
            tp = a4.timePoint;
            break;
        case 5:
            tp = a5.timePoint;
            break;
        case 6:
            tp = a6.timePoint;
            break;
        case 7:
            tp = a7.timePoint;
            break;
        case 8:
            tp = a8.timePoint;
            break;
        case 9:
            tp = a9.timePoint;
            break;
        default:
            return std::nullopt;
        }
        std::optional<B> b = f_(which, std::move(a0.value), std::move(a1.value), std::move(a2.value), std::move(a3.value), std::move(a4.value), std::move(a5.value), std::move(a6.value), std::move(a7.value), std::move(a8.value), std::move(a9.value));
        if (!b) {
            return std::nullopt;
        }
        return pureInnerData<B>(
            env
            , {
                tp
                , std::move(*b)
                , (a0.finalFlag && a1.finalFlag && a2.finalFlag && a3.finalFlag && a4.finalFlag && a5.finalFlag && a6.finalFlag && a7.finalFlag && a8.finalFlag && a9.finalFlag)
            }
        );
    }
public:
    MaybeActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~MaybeActionCore() {}
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class B, class F>
class KleisliActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B,F> final : public ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B> {
private:
    F f_;
protected:
    virtual Data<B> handle(int which, StateT *env, WithTime<A0,TimePoint> &&a0, WithTime<A1,TimePoint> &&a1, WithTime<A2,TimePoint> &&a2, WithTime<A3,TimePoint> &&a3, WithTime<A4,TimePoint> &&a4, WithTime<A5,TimePoint> &&a5, WithTime<A6,TimePoint> &&a6, WithTime<A7,TimePoint> &&a7, WithTime<A8,TimePoint> &&a8, WithTime<A9,TimePoint> &&a9) override final {
        return f_(which, InnerData<A0> { env, std::move(a0) }, InnerData<A1> { env, std::move(a1) }, InnerData<A2> { env, std::move(a2) }, InnerData<A3> { env, std::move(a3) }, InnerData<A4> { env, std::move(a4) }, InnerData<A5> { env, std::move(a5) }, InnerData<A6> { env, std::move(a6) }, InnerData<A7> { env, std::move(a7) }, InnerData<A8> { env, std::move(a8) }, InnerData<A9> { env, std::move(a9) });
    }
public:
    KleisliActionCore(F &&f, FanInParamMask const &requireMask=FanInParamMask()) : ActionCore<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B>(requireMask), f_(std::move(f)) {
    }
    virtual ~KleisliActionCore() {}
};
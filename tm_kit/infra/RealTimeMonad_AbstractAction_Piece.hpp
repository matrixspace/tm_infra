template <class A0, class A1, class B>
class AbstractAction<std::variant<A0,A1>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public Producer<B> {
};
template <class A0, class A1, class A2, class B>
class AbstractAction<std::variant<A0,A1,A2>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public Producer<B> {
};
template <class A0, class A1, class A2, class A3, class B>
class AbstractAction<std::variant<A0,A1,A2,A3>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public virtual IHandler<A3>, public Producer<B> {
};
template <class A0, class A1, class A2, class A3, class A4, class B>
class AbstractAction<std::variant<A0,A1,A2,A3,A4>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public virtual IHandler<A3>, public virtual IHandler<A4>, public Producer<B> {
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class B>
class AbstractAction<std::variant<A0,A1,A2,A3,A4,A5>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public virtual IHandler<A3>, public virtual IHandler<A4>, public virtual IHandler<A5>, public Producer<B> {
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class B>
class AbstractAction<std::variant<A0,A1,A2,A3,A4,A5,A6>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public virtual IHandler<A3>, public virtual IHandler<A4>, public virtual IHandler<A5>, public virtual IHandler<A6>, public Producer<B> {
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class B>
class AbstractAction<std::variant<A0,A1,A2,A3,A4,A5,A6,A7>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public virtual IHandler<A3>, public virtual IHandler<A4>, public virtual IHandler<A5>, public virtual IHandler<A6>, public virtual IHandler<A7>, public Producer<B> {
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class B>
class AbstractAction<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public virtual IHandler<A3>, public virtual IHandler<A4>, public virtual IHandler<A5>, public virtual IHandler<A6>, public virtual IHandler<A7>, public virtual IHandler<A8>, public Producer<B> {
};
template <class A0, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class B>
class AbstractAction<std::variant<A0,A1,A2,A3,A4,A5,A6,A7,A8,A9>,B> : public virtual IHandler<A0>, public virtual IHandler<A1>, public virtual IHandler<A2>, public virtual IHandler<A3>, public virtual IHandler<A4>, public virtual IHandler<A5>, public virtual IHandler<A6>, public virtual IHandler<A7>, public virtual IHandler<A8>, public virtual IHandler<A9>, public Producer<B> {
};
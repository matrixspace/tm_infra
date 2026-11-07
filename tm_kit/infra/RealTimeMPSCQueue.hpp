#ifndef TM_KIT_INFRA_REALTIME_MPSC_QUEUE_HPP_
#define TM_KIT_INFRA_REALTIME_MPSC_QUEUE_HPP_
#include <atomic>
#include <optional>
#include <utility>
namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace infra
            {
                namespace realtime_threaded_executor_detail
                {
                    //Vyukov intrusive unbounded MPSC queue with a stub node.
                    //link publication uses one exchange + one store; allocation may block.
                    //The global order is
                    //the exchange order, so items of all producers form one FIFO.
                    template <class Item>
                    class MPSCQueue {
                    private:
                        struct Node {
                            std::atomic<Node *> next{nullptr};
                            std::optional<Item> value;
                            Node() = default;
                            explicit Node(Item &&x)
                                : value(std::in_place, std::move(x))
                            {
                            }
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
                            Node *n = new Node(std::move(x));
                            Node *prev = head_.exchange(n, std::memory_order_seq_cst);
                            prev->next.store(n, std::memory_order_release);
                        }
                        //consumer only; may transiently report empty while a producer
                        //is between its exchange and its link store, the wake-up
                        //protocol of the executor covers that window
                        template <class F>
                        bool tryConsume(F &&consume)
                        {
                            Node *next = tail_->next.load(std::memory_order_acquire);
                            if (!next) {
                                return false;
                            }
                            Node *old = tail_;
                            tail_ = next;
                            delete old;
                            // The new tail remains the queue's stub. Its item
                            // can be consumed in place, without a second move.
                            // Reset also on exceptions, leaving a valid stub.
                            struct Reset {
                                std::optional<Item> &value;
                                ~Reset()
                                {
                                    value.reset();
                                }
                            } reset{next->value};
                            std::forward<F>(consume)(std::move(*next->value));
                            return true;
                        }
                        //consumer only. Unlike tryConsume this also reports items whose
                        //producer has done the exchange but not yet linked the node, so
                        //the consumer never goes to sleep inside that window
                        bool maybeNonEmpty() const
                        {
                            return head_.load(std::memory_order_seq_cst) != tail_;
                        }
                    };

                } // namespace realtime_threaded_executor_detail
            } // namespace infra
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif

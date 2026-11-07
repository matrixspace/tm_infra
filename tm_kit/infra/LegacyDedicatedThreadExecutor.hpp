#ifndef TM_KIT_INFRA_LEGACY_DEDICATED_THREAD_EXECUTOR_HPP_
#define TM_KIT_INFRA_LEGACY_DEDICATED_THREAD_EXECUTOR_HPP_
#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace infra
            {
                // Compatibility name for existing profiling clients.
                template <class Item>
                using LegacyDedicatedThreadExecutor = DedicatedThreadExecutor<Item>;
            } // namespace infra
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif

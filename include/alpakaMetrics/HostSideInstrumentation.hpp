// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "alpakaMetrics/Config.hpp"
#include "alpakaMetrics/Result.hpp"

#include <memory>

namespace alpakaMetrics
{
    namespace internal
    {
        class HostInstrumentation;
    } // namespace internal

    /** A region on the calling host thread. begin/end must use the same thread.
     * Asynchronous kernels are included only if the application waits before end().
     * CPU thread counters exclude work performed by other threads.
     */
    class HostSideInstrumentation
    {
    public:
        explicit HostSideInstrumentation(Config config = {});
        ~HostSideInstrumentation();
        HostSideInstrumentation(HostSideInstrumentation const&) = delete;
        HostSideInstrumentation& operator=(HostSideInstrumentation const&) = delete;
        HostSideInstrumentation(HostSideInstrumentation&&) = delete;
        HostSideInstrumentation& operator=(HostSideInstrumentation&&) = delete;
        void begin();
        [[nodiscard]] Result end();
        [[nodiscard]] bool isActive() const;
        [[nodiscard]] Result getResults() const;
        [[nodiscard]] static std::vector<MetricDescriptor> getAvailableMetrics();

    private:
        std::unique_ptr<internal::HostInstrumentation> m_instrumentation;
    };
} // namespace alpakaMetrics

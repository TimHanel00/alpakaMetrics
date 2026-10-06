// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Measurement.hpp>

#include <memory>

namespace alpakaMetrics
{
    struct SessionConfig
    {
        std::size_t maxPendingMeasurements{4096};
        std::size_t maxCompletedMeasurements{4096};
    };

    struct SessionStats
    {
        std::size_t pending{};
        std::size_t completed{};
        std::uint64_t droppedPending{};
        std::uint64_t droppedCompleted{};
        std::uint64_t failedDeliveries{};
        std::string lastError;
    };

    namespace internal
    {
        struct SessionState;
    } // namespace internal

    /** Shared stream of completed operations across queues. Destroying the last
     * session/queue owner cancels pending delivery; retained Measurement handles stay valid.
     */
    class Session
    {
    public:
        explicit Session(SessionConfig config = {});
        [[nodiscard]] std::uint64_t getId() const;
        /** Callback runs on the progress thread, outside queue/session locks.
         * Keep callbacks short. Calling drain() from a callback throws.
         */
        void setResultCallback(std::function<void(Result const&)> callback) const;
        [[nodiscard]] std::vector<Measurement> takeCompletedMeasurements() const;
        [[nodiscard]] SessionStats getStats() const;
        /** Waits for delivery of operations tracked before this call. */
        void drain() const;
        /** Registers a handle. Capacity exhaustion drops stream delivery without waiting. */
        bool track(Measurement measurement) const;

    private:
        std::shared_ptr<internal::SessionState> m_state;
    };
} // namespace alpakaMetrics

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpaka/onHost/Handle.hpp>

#include <alpakaMetrics/Result.hpp>

#include <exception>
#include <functional>
#include <mutex>

namespace alpakaMetrics
{
    namespace internal
    {
        struct MeasurementState
        {
            std::uint64_t id{};
            std::function<bool()> isComplete;
            std::function<Result()> read;
            mutable std::mutex mutex;
            mutable std::optional<Result> result;
            mutable std::exception_ptr failure;
            OperationProvenance provenance;

            void readResults() const
            {
                if(failure)
                    std::rethrow_exception(failure);
                if(!result)
                    try
                    {
                        result = read();
                        result->provenance = provenance;
                    }
                    catch(...)
                    {
                        failure = std::current_exception();
                        throw;
                    }
            }

            Result getResults() const
            {
                std::lock_guard lock{mutex};
                readResults();
                return *result;
            }

            void wait()
            {
                static_cast<void>(getResults());
            }
        };
    } // namespace internal

    class Measurement
    {
    public:
        using element_type = internal::MeasurementState;

        explicit Measurement(alpaka::onHost::Handle<element_type> state) : m_state{std::move(state)}
        {
        }

        [[nodiscard]] element_type* get() const
        {
            return m_state.get();
        }

        [[nodiscard]] std::uint64_t getId() const
        {
            return m_state->id;
        }

        [[nodiscard]] bool isComplete() const
        {
            return m_state->isComplete();
        }

        [[nodiscard]] Result getResults() const
        {
            return m_state->getResults();
        }

        void wait() const
        {
            static_cast<void>(getResults());
        }

    private:
        alpaka::onHost::Handle<element_type> m_state;
    };
} // namespace alpakaMetrics

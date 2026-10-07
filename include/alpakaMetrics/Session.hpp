// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Measurement.hpp>
#include <alpakaMetrics/internal/Config.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

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
        alpaka::onHost::Handle<internal::SessionState> m_state;
    };
} // namespace alpakaMetrics

namespace alpakaMetrics::internal
{
    struct SessionState
    {
        struct Pending
        {
            std::uint64_t sequence;
            Measurement measurement;
        };

        std::uint64_t id{nextMeasurementId()};
        SessionConfig config;
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::uint64_t sequence{};
        std::map<std::uint64_t, Measurement> pending;
        std::deque<Measurement> completed;
        std::function<void(Result const&)> callback;
        SessionStats stats;

        explicit SessionState(SessionConfig config) : config{config}
        {
        }
    };
} // namespace alpakaMetrics::internal

namespace alpakaMetrics
{
    namespace internal::session
    {
        inline thread_local bool onProgressThread{};

        class Progress
        {
        public:
            Progress() : m_worker{[this](std::stop_token stop) { run(stop); }}
            {
            }

            void add(alpaka::onHost::Handle<internal::SessionState> const& state)
            {
                std::lock_guard lock{m_mutex};
                m_states.push_back(state);
            }

        private:
            void run(std::stop_token stop)
            {
                onProgressThread = true;
                while(!stop.stop_requested())
                {
                    std::vector<alpaka::onHost::Handle<internal::SessionState>> states;
                    {
                        std::lock_guard lock{m_mutex};
                        std::erase_if(m_states, [](auto const& state) { return state.expired(); });
                        for(auto const& state : m_states)
                            if(auto owned = state.lock())
                                states.push_back(std::move(owned));
                    }
                    for(auto const& state : states)
                        poll(*state);
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            static void poll(internal::SessionState& state)
            {
                std::vector<internal::SessionState::Pending> pending;
                {
                    std::lock_guard lock{state.mutex};
                    pending.reserve(state.pending.size());
                    for(auto const& [sequence, measurement] : state.pending)
                        pending.push_back({sequence, measurement});
                }
                for(auto const& entry : pending)
                {
                    std::optional<Result> result;
                    std::string error;
                    try
                    {
                        result = entry.measurement.tryGetResults();
                        if(!result)
                            continue;
                    }
                    catch(std::exception const& failure)
                    {
                        error = failure.what();
                    }
                    catch(...)
                    {
                        error = "Operation result failed with an unknown exception";
                    }

                    std::function<void(Result const&)> callback;
                    {
                        std::lock_guard lock{state.mutex};
                        if(state.completed.size() == state.config.maxCompletedMeasurements)
                        {
                            state.completed.pop_front();
                            ++state.stats.droppedCompleted;
                        }
                        state.completed.push_back(entry.measurement);
                        callback = state.callback;
                    }
                    if(result && callback)
                        try
                        {
                            callback(*result);
                        }
                        catch(std::exception const& failure)
                        {
                            error = failure.what();
                        }
                        catch(...)
                        {
                            error = "Result callback failed with an unknown exception";
                        }

                    {
                        std::lock_guard lock{state.mutex};
                        if(!error.empty())
                        {
                            ++state.stats.failedDeliveries;
                            state.stats.lastError = std::move(error);
                        }
                        state.pending.erase(entry.sequence);
                    }
                    state.changed.notify_all();
                }
            }

            std::mutex m_mutex;
            std::vector<std::weak_ptr<internal::SessionState>> m_states;
            // Declared last so the worker joins before its state is destroyed.
            std::jthread m_worker;
        };

        inline Progress& progress()
        {
            static Progress instance;
            return instance;
        }
    } // namespace internal::session

    inline Session::Session(SessionConfig config) : m_state{std::make_shared<internal::SessionState>(config)}
    {
        if(!config.maxPendingMeasurements || !config.maxCompletedMeasurements)
            throw std::invalid_argument{"Session capacities must be positive"};
        internal::session::progress().add(m_state);
    }

    inline std::uint64_t Session::getId() const
    {
        return m_state->id;
    }

    inline void Session::setResultCallback(std::function<void(Result const&)> callback) const
    {
        std::lock_guard lock{m_state->mutex};
        m_state->callback = std::move(callback);
    }

    inline std::vector<Measurement> Session::takeCompletedMeasurements() const
    {
        std::lock_guard lock{m_state->mutex};
        std::vector<Measurement> result{m_state->completed.begin(), m_state->completed.end()};
        m_state->completed.clear();
        return result;
    }

    inline SessionStats Session::getStats() const
    {
        std::lock_guard lock{m_state->mutex};
        auto stats = m_state->stats;
        stats.pending = m_state->pending.size();
        stats.completed = m_state->completed.size();
        return stats;
    }

    inline void Session::drain() const
    {
        if(internal::session::onProgressThread)
            throw std::logic_error{"Session::drain cannot run on the progress thread"};
        std::unique_lock lock{m_state->mutex};
        auto const watermark = m_state->sequence;
        m_state->changed.wait(
            lock,
            [&] { return m_state->pending.empty() || m_state->pending.begin()->first > watermark; });
    }

    inline bool Session::track(Measurement measurement) const
    {
        std::lock_guard lock{m_state->mutex};
        if(m_state->pending.size() == m_state->config.maxPendingMeasurements)
        {
            ++m_state->stats.droppedPending;
            return false;
        }
        m_state->pending.emplace(++m_state->sequence, std::move(measurement));
        return true;
    }
} // namespace alpakaMetrics

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Collector.hpp>
#include <alpakaMetrics/internal/Config.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>

namespace alpakaMetrics::internal
{
    /** Provider callbacks own no application objects. All callback storage is quiesced before destruction. */
    class AsyncCounters
    {
    public:
        AsyncCounters(
            collector::Module collectorModule,
            Config const& config,
            plugin::Options const& options,
            plugin::Operation operation,
            std::chrono::milliseconds completionTimeout = std::chrono::seconds{30})
            : m_module{std::move(collectorModule)}
            , m_operation{operation}
            , m_receiver{{}, {}, m_module.info}
            , m_timeout{completionTimeout}
        {
            std::vector<plugin::Request> requests;
            for(auto const& request : config.metrics)
                if(request.name != "elapsed_time")
                {
                    requests.push_back(
                        {request.name.c_str(),
                         request.getNativeName().c_str(),
                         static_cast<std::uint32_t>(request.unit),
                         request.scale});
                    m_results.push_back(makeUnavailable(
                        request,
                        MetricStatus::collectionFailed,
                        "Asynchronous collector did not return this metric"));
                }
            if(m_module.info.requiresDeviceSynchronization && !config.allowSynchronization)
            {
                for(auto& result : m_results)
                {
                    result.status = MetricStatus::unsupportedScope;
                    result.diagnostic = "Collector requires device synchronization; opt in with allowSynchronization";
                }
                m_complete = true;
                return;
            }
            m_sink = {this, emit, complete};
            m_session = m_module.asyncApi->create(requests.data(), requests.size(), &options, &m_operation, &m_sink);
            if(!m_session)
                fail("Asynchronous collector session creation failed");
        }

        ~AsyncCounters()
        {
            // No receiver or progress lock is held: destroy must quiesce concurrent callbacks.
            if(m_session)
                m_module.asyncApi->destroy(m_session);
        }

        AsyncCounters(AsyncCounters const&) = delete;
        AsyncCounters& operator=(AsyncCounters const&) = delete;

        bool requiresSynchronization() const
        {
            return m_session && m_module.info.requiresDeviceSynchronization;
        }

        void begin()
        {
            if(m_session && !m_module.asyncApi->begin(m_session))
                fail("Asynchronous collector could not begin operation collection");
        }

        void submitted()
        {
            if(m_session)
                m_module.asyncApi->submitted(m_session);
        }

        /** Called only after execution completes. poll and provider callbacks never invoke user code. */
        bool isComplete()
        {
            std::unique_lock progressLock{m_progressMutex, std::try_to_lock};
            if(!progressLock.owns_lock())
                return false;
            {
                std::lock_guard lock{m_mutex};
                if(m_complete)
                    return true;
            }
            if(!m_executionFinished)
                m_executionFinished = std::chrono::steady_clock::now();
            m_module.asyncApi->poll(m_session, true);
            if(std::chrono::steady_clock::now() - *m_executionFinished >= m_timeout)
                fail("Asynchronous collector timed out after operation execution completed");
            std::lock_guard lock{m_mutex};
            return m_complete;
        }

        std::vector<MetricResult> getResults()
        {
            while(!isComplete())
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            std::lock_guard lock{m_mutex};
            auto results = m_results;
            if(!m_failed)
                for(auto& result : results)
                {
                    auto found = std::find_if(
                        m_receiver.metrics.begin(),
                        m_receiver.metrics.end(),
                        [&](auto const& metric) { return metric.descriptor.name == result.descriptor.name; });
                    if(found != m_receiver.metrics.end())
                        result = *found;
                }
            for(auto& result : results)
            {
                result.descriptor.collector = m_module.info.name;
                result.descriptor.collectorVersion = m_module.info.version;
                result.descriptor.collectorPath = m_module.info.path;
            }
            return results;
        }

    private:
        void fail(char const* diagnostic)
        {
            std::lock_guard lock{m_mutex};
            if(m_complete)
                return;
            failLocked(diagnostic);
        }

        void failLocked(char const* diagnostic) noexcept
        {
            for(auto& result : m_results)
            {
                result.status = MetricStatus::collectionFailed;
                result.value.reset();
                try
                {
                    result.diagnostic = diagnostic;
                }
                catch(...)
                {
                    m_receiver.failure = std::current_exception();
                }
            }
            m_failed = true;
            m_complete = true;
        }

        static void emit(void* context, std::uint64_t id, plugin::Metric const* metric) noexcept
        {
            auto& self = *static_cast<AsyncCounters*>(context);
            std::lock_guard lock{self.m_mutex};
            if(self.m_complete)
                return;
            try
            {
                if(id != self.m_operation.measurementId)
                    throw std::runtime_error{"Asynchronous collector returned an incorrect operation ID"};
                collector::Receiver::emit(&self.m_receiver, metric);
                if(self.m_receiver.failure)
                    throw std::runtime_error{"Invalid asynchronous collector result"};
                auto const& name = self.m_receiver.metrics.back().descriptor.name;
                if(std::none_of(
                       self.m_results.begin(),
                       self.m_results.end(),
                       [&](auto const& result) { return result.descriptor.name == name; })
                   || std::count_if(
                          self.m_receiver.metrics.begin(),
                          self.m_receiver.metrics.end(),
                          [&](auto const& result) { return result.descriptor.name == name; })
                          != 1)
                    throw std::runtime_error{"Asynchronous collector returned an unexpected or duplicate metric"};
            }
            catch(std::exception const& error)
            {
                self.failLocked(error.what());
            }
            catch(...)
            {
                self.failLocked("Invalid asynchronous collector result");
            }
        }

        static void complete(void* context, std::uint64_t id, bool success, char const* diagnostic) noexcept
        {
            auto& self = *static_cast<AsyncCounters*>(context);
            std::lock_guard lock{self.m_mutex};
            if(self.m_complete)
                return;
            try
            {
                if(id != self.m_operation.measurementId || !diagnostic)
                    self.failLocked("Invalid asynchronous collector completion");
                else if(!success)
                    self.failLocked(*diagnostic ? diagnostic : "Asynchronous collector failed operation delivery");
                else
                    self.m_complete = true;
            }
            catch(...)
            {
                // Allocation failure is retained without throwing through the provider ABI.
                self.m_failed = true;
                self.m_complete = true;
            }
        }

        collector::Module m_module;
        plugin::Operation m_operation;
        collector::Receiver m_receiver;
        std::vector<MetricResult> m_results;
        std::mutex m_mutex;
        std::mutex m_progressMutex;
        std::optional<std::chrono::steady_clock::time_point> m_executionFinished;
        std::chrono::milliseconds m_timeout;
        plugin::AsyncSink m_sink{};
        void* m_session{};
        bool m_complete{};
        bool m_failed{};
    };

    inline std::shared_ptr<AsyncCounters> makeAsyncCounters(
        Config const& config,
        std::uint64_t id,
        std::string_view api,
        std::uint32_t device,
        std::uintptr_t nativeQueue)
    {
        if(std::none_of(
               config.metrics.begin(),
               config.metrics.end(),
               [](auto const& request) { return request.name != "elapsed_time"; }))
            return {};
        std::optional<collector::Module> collectorModule;
        try
        {
            collectorModule.emplace(collector::loadModule(config.collectorPlugin));
        }
        catch(std::exception const&)
        {
            return {}; // Legacy path preserves selection/load diagnostics.
        }
        if(!collectorModule->asyncApi)
            return {};
        std::string apiName{api};
        plugin::Options options{static_cast<std::uint32_t>(config.counterScope), apiName.c_str(), device};
        return std::make_shared<AsyncCounters>(*collectorModule, config, options, plugin::Operation{id, nativeQueue});
    }
} // namespace alpakaMetrics::internal

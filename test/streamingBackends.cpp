// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/alpakaMetrics.hpp>

#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <thread>
#include <unordered_set>

namespace
{
    struct Increment
    {
        ALPAKA_FN_ACC void operator()(
            alpaka::onAcc::concepts::Acc auto const& acc,
            alpaka::concepts::IView auto output) const
        {
            for(auto const index : alpaka::onAcc::makeIdxMap(
                    acc,
                    alpaka::onAcc::worker::threadsInGrid,
                    alpaka::IdxRange{alpaka::Vec{1u}}))
                ++output[index];
        }
    };

    struct Gate
    {
        std::promise<void> promise;
        std::shared_future<void> ready{promise.get_future().share()};
        std::atomic<bool> released{};

        void release()
        {
            if(!released.exchange(true))
                promise.set_value();
        }

        ~Gate()
        {
            release();
        }
    };

    bool testBackend(alpaka::concepts::BackendSpec auto const& backend)
    {
        auto selector = alpaka::onHost::makeDeviceSelector(alpaka::onHost::DeviceSpec{backend});
        if(!selector.isAvailable())
            return false;
        auto device = selector.makeDevice(0u);
        auto raw = device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
        auto output = alpaka::onHost::alloc<std::uint32_t>(device, alpaka::Vec{1u});
        auto host = alpaka::onHost::allocHostLike(output);
        auto spec = alpaka::onHost::FrameSpec{alpaka::Vec{1u}, alpaka::Vec{1u}, alpaka::getExecutor(backend)};
        // Warm up backend initialization/JIT before checking submission behind blocked work.
        alpaka::onHost::memset(raw, output, 0u);
        raw.enqueue(spec, Increment{}, output);
        alpaka::onHost::memset(raw, output, 0u);
        alpaka::onHost::wait(raw);

        std::mutex mutex;
        std::vector<alpakaMetrics::Result> results;
        std::vector<std::thread::id> callbackThreads;
        alpakaMetrics::Session session;
        session.setResultCallback(
            [&](alpakaMetrics::Result const& result)
            {
                std::lock_guard lock{mutex};
                results.push_back(result);
                callbackThreads.push_back(std::this_thread::get_id());
            });
        auto queue = alpakaMetrics::makeQueue(raw, session);
        Gate gate;
        std::atomic<bool> timedOut{};
        std::jthread watchdog{[&](std::stop_token stop)
                              {
                                  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                                  while(!stop.stop_requested() && !gate.released)
                                  {
                                      if(std::chrono::steady_clock::now() >= deadline)
                                      {
                                          timedOut = true;
                                          gate.release();
                                          return;
                                      }
                                      std::this_thread::sleep_for(std::chrono::milliseconds{10});
                                  }
                              }};
        raw.enqueueHostFn([ready = gate.ready] { ready.wait(); });
        std::vector<alpakaMetrics::Measurement> measurements;
        for(std::uint32_t i = 0; i < 3; ++i)
            measurements.push_back(queue.enqueue(spec, Increment{}, output));
        bool const pending = !measurements.front().tryGetResults();
        gate.release();
        watchdog.request_stop();
        session.drain();
        alpaka::onHost::memcpy(raw, host, output);
        alpaka::onHost::wait(raw);
        if(timedOut || !pending)
            throw std::runtime_error{"Submission or polling waited for preceding queue work"};
        if(host[0] != 3 || results.size() != 3 || session.getStats().failedDeliveries)
            throw std::runtime_error{
                "Streaming lost a completion, replayed work, or failed: " + session.getStats().lastError};
        std::unordered_set<std::uint64_t> ids;
        for(std::size_t i = 0; i < results.size(); ++i)
        {
            auto const& result = results[i];
            auto const& elapsed = result.getMetric("elapsed_time");
            if(!elapsed.isAvailable() || !std::isfinite(elapsed.asDouble()) || elapsed.asDouble() < 0
               || elapsed.descriptor.scope != alpakaMetrics::MetricScope::queueInterval || result.synchronized
               || result.replayed || result.passCount != 1 || result.provenance.sessionId != session.getId()
               || result.provenance.api != alpaka::getApi(backend).getName()
               || result.provenance.kind != alpakaMetrics::OperationKind::kernel
               || callbackThreads[i] == std::this_thread::get_id() || !ids.insert(result.measurementId).second)
                throw std::runtime_error{"Completed result lost its timing, provenance, or delivery semantics"};
        }
        for(auto const& measurement : measurements)
            if(!ids.contains(measurement.getId()) || !measurement.tryGetResults())
                throw std::runtime_error{"Direct and streamed results disagree"};
        std::cout << "Asynchronous streaming passed: " << alpaka::getApi(backend).getName() << " / "
                  << alpaka::getExecutor(backend).getName() << '\n';
        return true;
    }
} // namespace

int main()
{
    try
    {
        bool tested = false;
        auto const result = alpaka::onHost::executeForEach(
            [&](alpaka::concepts::BackendSpec auto const& backend)
            {
                tested = testBackend(backend) || tested;
                return 0;
            },
            alpaka::onHost::allBackends(alpaka::onHost::enabledDeviceSpecs, alpaka::exec::enabledExecutors));
        return tested ? result : 77;
    }
    catch(std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>

/** Versioned collector ABI: no C++ objects, ownership, or exceptions cross the boundary.
 * Enum fields use the values in Result.hpp. All strings live through the receiving call.
 * Sessions are created, started, stopped and destroyed on the same thread.
 */
namespace alpakaMetrics::plugin
{
    inline constexpr std::uint32_t collectorAbiVersion = 1;
    inline constexpr std::uint32_t requiresDeviceSynchronization = 1;

    struct Request
    {
        char const* name;
        char const* nativeName;
        std::uint32_t unit;
        double scale;
    };

    struct Options
    {
        std::uint32_t scope;
        char const* deviceApi; // Empty for a host region.
        std::uint32_t device;
    };

    struct Metric
    {
        char const* name;
        char const* nativeName;
        char const* description;
        char const* provider;
        std::uint32_t unit;
        std::uint32_t scope;
        char const* nativeUnit;
        double scale;
        std::uint32_t status;
        std::uint32_t valueKind; // 0: absent, 1: signed, 2: unsigned, 3: double.
        std::int64_t signedValue;
        std::uint64_t unsignedValue;
        double doubleValue;
        char const* diagnostic;
    };

    using Emit = void (*)(void*, Metric const*) noexcept;

    struct Collector
    {
        std::uint32_t abiVersion;
        std::size_t structSize;
        char const* name;
        char const* version;
        std::uint32_t capabilities;
        void* (*create)(Request const*, std::size_t, Options const*) noexcept;
        void (*destroy)(void*) noexcept;
        void (*begin)(void*) noexcept;
        bool (*hasEvents)(void const*) noexcept;
        bool (*isRunning)(void const*) noexcept;
        void (*end)(void*, Emit, void*) noexcept;
        void (*discover)(Emit, void*) noexcept;
    };

    using Entry = Collector const* (*) () noexcept;

    /** Optional, independent asynchronous ABI. Existing Collector modules remain compatible. */
    inline constexpr std::uint32_t asyncCollectorAbiVersion = 1;

    struct Operation
    {
        std::uint64_t measurementId;
        // Host queue index or CUDA/HIP stream pointer encoded as an integer; see Options::deviceApi.
        std::uintptr_t nativeQueue;
    };

    struct AsyncSink
    {
        void* context;
        void (*emit)(void*, std::uint64_t, Metric const*) noexcept;
        // Success closes delivery; missing metrics remain failed. Failure invalidates all metrics.
        void (*complete)(void*, std::uint64_t, bool success, char const* diagnostic) noexcept;
    };

    /** begin/submitted bracket submission on its thread; poll never blocks for GPU work.
     * Callbacks may arrive on provider threads. destroy cancels and quiesces callbacks,
     * and may run on any thread. No provider call may wait for application/user callbacks.
     */
    struct AsyncCollector
    {
        std::uint32_t abiVersion;
        std::size_t structSize;
        char const* name;
        char const* version;
        std::uint32_t capabilities;
        void* (*create)(Request const*, std::size_t, Options const*, Operation const*, AsyncSink const*) noexcept;
        void (*destroy)(void*) noexcept;
        bool (*begin)(void*) noexcept;
        void (*submitted)(void*) noexcept;
        void (*poll)(void*, bool executionComplete) noexcept;
        void (*discover)(Emit, void*) noexcept;
    };

    using AsyncEntry = AsyncCollector const* (*) () noexcept;
} // namespace alpakaMetrics::plugin

// A module exports: extern "C" Collector const* alpakaMetrics_getCollector() noexcept;
// Or: extern "C" AsyncCollector const* alpakaMetrics_getAsyncCollector() noexcept;

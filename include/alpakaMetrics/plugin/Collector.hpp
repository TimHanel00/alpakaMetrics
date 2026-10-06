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
} // namespace alpakaMetrics::plugin

// A module exports: extern "C" Collector const* alpakaMetrics_getCollector() noexcept;

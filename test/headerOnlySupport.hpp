// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/alpakaMetrics.hpp>

#include <atomic>

namespace headerOnlyTest
{
    std::uint64_t nextId();
    bool sharesProgress(alpakaMetrics::internal::session::Progress const& progress);
    void setCallback(alpakaMetrics::Session session, std::atomic<bool>& rejectedDrain);
    std::string serialize(alpakaMetrics::Result const& result);
    alpakaMetrics::RooflineAnalysis analyze();
} // namespace headerOnlyTest

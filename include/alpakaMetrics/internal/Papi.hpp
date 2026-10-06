// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/internal/Counters.hpp>

namespace alpakaMetrics::internal
{
    // Compatibility for existing internal consumers; collection is provider-neutral.
    using PapiCounters = Counters;
} // namespace alpakaMetrics::internal

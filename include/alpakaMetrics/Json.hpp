// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <alpakaMetrics/Result.hpp>

namespace alpakaMetrics
{
    /** Optional export; collection never writes files. Non-finite doubles are rejected. */
    std::string toJson(Result const& result);
} // namespace alpakaMetrics

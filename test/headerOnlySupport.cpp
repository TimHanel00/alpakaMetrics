// SPDX-License-Identifier: MPL-2.0
#include "headerOnlySupport.hpp"

namespace headerOnlyTest
{
    std::uint64_t nextId()
    {
        return alpakaMetrics::internal::nextMeasurementId();
    }

    bool sharesProgress(alpakaMetrics::internal::session::Progress const& progress)
    {
        return &progress == &alpakaMetrics::internal::session::progress();
    }

    void setCallback(alpakaMetrics::Session session, std::atomic<bool>& rejectedDrain)
    {
        session.setResultCallback(
            [session, &rejectedDrain](alpakaMetrics::Result const&)
            {
                try
                {
                    session.drain();
                }
                catch(std::logic_error const&)
                {
                    rejectedDrain = true;
                }
            });
    }

    std::string serialize(alpakaMetrics::Result const& result)
    {
        return alpakaMetrics::toJson(result);
    }

    alpakaMetrics::RooflineAnalysis analyze()
    {
        return alpakaMetrics::analyzeRoofline(
            alpakaMetrics::RooflineSample{1.0, 2.0, 4.0},
            alpakaMetrics::RooflineCeilings{4.0, 8.0, "FP64", "DRAM", "test"});
    }
} // namespace headerOnlyTest

# Collection contracts

## Scope and completion

| Operation | Timing | Counters |
|---|---|---|
| Host region | Wall time between `begin()` and `end()` | Calling thread or explicit provider scope |
| Host callback | Callback wall time | Queue worker thread |
| CpuSerial kernel | Interval between queue markers | Queue worker thread |
| Parallel CPU kernel | Interval between queue markers | `unsupportedScope` |
| CUDA kernel | Alpaka timing-event interval | Optional correlated native duration; synchronized PAPI counters at context scope |
| HIP kernel | Alpaka timing-event interval | Optional correlated native duration; synchronized PAPI counters at device scope |
| SYCL kernel | Alpaka timing-event interval | `unsupportedScope` |

Queue intervals exclude preceding queue delay but may include bookkeeping and
idle gaps between markers. They are not kernel-exclusive durations. Device queues
must enable timing. Submissions through the underlying queue bypass measurement.
Kernels are launched once; no automatic replay or multiplexing is performed.

Asynchronous collector plugins are selected automatically for kernels on
timing-enabled host, CUDA and HIP queues. No collection policy is required.
`isComplete()` and `tryGetResults()` account for both execution completion and
collector delivery. Device/context-wide measurements retain their declared scope;
asynchronous delivery does not imply kernel-exclusive attribution. PAPI and host
regions retain their existing collection path. SYCL currently retains timing-only
collection.

The provider closes delivery explicitly. Omitted metrics become failed entries;
invalid correlation, duplicate records or failed delivery invalidate its metrics.
A provider that has not closed delivery within 30 seconds after execution completion
is observed reports `collectionFailed`; timing remains available. This deadline
bounds result delivery, including `Session::drain()`, without replaying work.

`Measurement::getResults()` waits and caches a snapshot. `tryGetResults()` returns
an empty optional if work is pending or another reader holds the result lock.
Both propagate operation failures. Retained handles survive queue history clearing.
Host regions must begin/end on the same thread; CPU counters exclude other threads.

`Session` polls completion on a background progress thread, without synchronizing
each submission. Completed operations can be delivered before earlier operations
on other queues. `drain()` waits for tracked submissions that preceded the call,
including callback delivery. It does not wait for submissions dropped from the
stream because of pending-capacity overflow; their direct handles still work.
Destroying the last session/queue owner cancels pending stream delivery.

Session defaults retain up to 4096 pending and 4096 completed handles. Configure
these limits with `SessionConfig`; `getStats()` reports overflow and delivery
failures. A full pending buffer declines delivery without waiting. A full completed
buffer discards its oldest handle. Callbacks still receive completed tracked
operations. The queue's separate history is retained until `clearMeasurements()`.
Callbacks execute outside queue/session locks; a slow callback delays progress for
other sessions. Keep captured objects alive through `drain()` and avoid ownership
cycles when capturing a session in its own callback. Callback exceptions are
reported in session statistics and do not invalidate direct result handles.

## Counter mappings

Semantic CPU tags include cycles, instructions, floating-point operations,
cache misses/accesses, branches/mispredictions, loads/stores and resource stalls.
The PAPI plugin resolves these to presets; availability and event definitions
vary by hardware. Occupancy, energy, frequency and transferred bytes require an
explicit mapping. Cache counts cannot substitute for transferred bytes.

```cpp
alpakaMetrics::Config config{.metrics = {
    alpakaMetrics::metric::elapsedTime,
    alpakaMetrics::metric::map(
        alpakaMetrics::metric::energy,
        alpakaMetrics::metric::native(
            "rapl:::PACKAGE_ENERGY:PACKAGE0",
            alpakaMetrics::MetricUnit::joules, 1.0e-9))}};
alpakaMetrics::HostSideInstrumentation region{config};
region.begin();
// Work on the calling thread.
auto result = region.end();
```

The pinned PAPI RAPL component reports this package event in nJ, so the explicit
scale converts it to joules. Collection requires the `rapl` component and suitable
permissions. Native mappings preserve provider scope. Conversions must be finite,
positive and compatible with the semantic unit. `MetricRequest::nativeName` is
the provider-neutral field; the former `papiName` spelling remains a compatibility
fallback. Prefer `metric::map()` in new code.

Descriptors preserve provider/component, definition, native unit and conversion.
Values retain signed/unsigned 64-bit integers or doubles; `asDouble()` may lose
integer precision. Discovery describes event definitions, not guaranteed
collectibility. Unsupported requests remain individual results without values.

## Synchronized GPU counter collection

PAPI CUDA/ROCP_SDK collection is optional and requires
`Config::allowSynchronization = true`. The adapter selects the queue device on
the submitting thread, finishes preceding work, starts counters, launches once,
waits for the completion event and stops counters on that same thread. Results
record `synchronized`. Unavailable counters fall back to asynchronous timing.

Select `-Dalpaka_DEP_CUDA=ON -DalpakaMetrics_PAPI_COMPONENTS=cuda` and configure
`PAPI_CUDA_ROOT`, or use `-Dalpaka_DEP_HIP=ON
-DalpakaMetrics_PAPI_COMPONENTS=rocp_sdk` and `PAPI_ROCP_SDK_ROOT`. Choose verified
native events from discovery: CUDA accepts `cuda:::` mappings, HIP `rocp_sdk:::`.
CPU presets are rejected for device queues. Missing `:device=` qualifiers are
filled from the queue device; explicit qualifiers must match it.

CUDA counters retain context scope; HIP sampling retains device scope. Other
streams and external profilers may affect these measurements. Overlapping GPU
counter regions within alpakaMetrics report a conflict. The pinned PAPI component
rejects CUDA events requiring multiple passes. HIP visibility remapping is rejected
because remapped ordinals cannot be matched safely to PAPI agent indices.
Leave `PAPI_ROCP_SDK_DISPATCH_MODE` unset: its dispatch records are not guaranteed
to be flushed when the launch completes, so that mode is rejected.

Hardware validation is opt-in through `ALPAKA_METRICS_TEST_CUDA_EVENT` or
`ALPAKA_METRICS_TEST_HIP_EVENT`, naming a supported single-pass event producing
positive counts on device 0. Without an event, the device test skips. See the pinned [CUDA](https://github.com/icl-utk-edu/papi/blob/72a3124d048dc5c89eb3f00c9f2866f4492b5383/src/components/cuda/README.md)
and [ROCP_SDK](https://github.com/icl-utk-edu/papi/blob/72a3124d048dc5c89eb3f00c9f2866f4492b5383/src/components/rocp_sdk/README.md)
component documentation for runtime requirements.

## Tuning, export and analysis

The current alpaka3-tuner custom objective uses the most recent launch:

```cpp
tuner.enqueue(queue, frameSpec, tunableBundle);
auto measurement = queue.getMeasurements().back();
tuner.provideMetric(measurement.getResults().getMetric("elapsed_time").asDouble());
```

Provide feedback before its next enqueue. The optional integration test accepts `alpakaMetrics_TUNER_SOURCE_DIR`.

`toJson(result)` exports schema version 1 with IDs, operation provenance, metric
metadata, statuses, typed values and diagnostics. It does not activate a collector
or write files. Emit one returned string per line for JSON Lines; serialize writes
when sharing a stream with other threads. Non-finite doubles are rejected. Consumers
must preserve 64-bit integer precision.

`analyzeRoofline(result, ceilings)` requires available elapsed seconds, executed
floating-point operations and transferred bytes with compatible attribution.
`RooflineMetrics` can select alternative names. Device/context/provider-wide
counts cannot automatically be paired with queue timing. The scalar overload
accepts an explicitly attributed `RooflineSample` at the caller's responsibility.
Ceilings must state their provenance, compute mode and memory level. Calibration,
operation convention and device configuration must match the workload. The result
reports intensity, rates, ridge point, ceiling fraction and model classification;
classification alone is not proof of the bottleneck. Values above a ceiling remain
visible; missing metrics, incompatible units/scopes and invalid values are rejected.
See [NERSC's methodology](https://docs.nersc.gov/tools/performance/roofline/).

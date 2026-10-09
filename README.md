# alpakaMetrics
This repository adds support native runtime metrics support for alpaka 3 (https://github.com/alpaka-group/alpaka3) as an optional instrumentation adapter.
The goal is to enhance profiling utilities across Vendor APIs. Most utilities and metrics are enabled by PAPI, which remains a optional but important dependency of this repository.
PAPI has support for CPU, CUDA and HIP specific hardware counters and is therefore a natural fit, that is compatible with alpakas generic approach on supporting multiple vendor APIs.


Some architectural ideas are inspired by
[`alpaka-group/bactria`](https://github.com/alpaka-group/bactria), particularly
separating instrumentation from measurement providers and output.

alpakaMetrics focuses on **Alpaka 3 operation tracking and extensible measurements
for online tuning**. It retains direct result handles and queue integration.
The core is header-only; PAPI is an optional compiled collector plugin.

## Build

```sh
cmake -S . -B build -G Ninja \
  -DalpakaMetrics_BUILD_TESTING=ON \
  -DalpakaMetrics_BUILD_EXAMPLES=ON \
  -Dalpaka_FAST_MATH=OFF -Dalpaka_FTZ=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

The floating-point options preserve non-finite checks and subnormal inputs in
result validation.

Alpaka and bundled PAPI revisions are pinned. PAPI is ON by default; disable its
plugin with `-DalpakaMetrics_DEP_PAPI=OFF`. The `alpakaMetrics::alpakaMetrics`
interface target has no compiled library or PAPI link dependency.
Bundled PAPI requires Linux and Autotools; installed PAPI can be selected with
`-DalpakaMetrics_USE_SYSTEM_PAPI=ON`. Additional components use, for example,
`'-DalpakaMetrics_PAPI_COMPONENTS=sde;rapl'`. GPU components require vendor SDKs.
A local Alpaka checkout can be supplied through
`-DFETCHCONTENT_SOURCE_DIR_ALPAKA3=/absolute/path/to/alpaka`.

Link applications to `alpakaMetrics::alpakaMetrics` and call
`alpaka_finalize(application)`. Installed consumers can use
`find_package(alpakaMetrics CONFIG REQUIRED)`.

## Measurements and streaming

```cpp
#include <alpakaMetrics/alpakaMetrics.hpp>

alpakaMetrics::Session session;
session.setResultCallback([](alpakaMetrics::Result const& result) {
    // Feed an application-owned tuning policy; runs on the progress thread.
});
auto queue = alpakaMetrics::makeQueue(
    device.makeQueue(alpaka::queueKind::nonBlocking, alpaka::timing::enabled),
    session,
    alpakaMetrics::Config{
        .metrics = {alpakaMetrics::metric::elapsedTime}, .label = "candidate"});
auto measurement = queue.enqueue(frameSpec, kernel, args...);
// Continue submitting work. No wait is required after each enqueue.
if(auto result = measurement.tryGetResults()) {
    // Inspect a completed result without waiting.
}
session.drain(); // Optional batch boundary: waits for tracked result delivery.
auto result = measurement.getResults(); // Stable snapshot; waits if still pending.
```

Run `./build/example/alpakaMetrics_asynchronousVectorAdd` for an instrumented
[Alpaka 3 vector-add example](https://github.com/alpaka-group/alpaka3/blob/b7d339d07056a9a2a6c4051cc3927157bc5f0d51/example/vectorAdd/src/vectorAdd.cpp).
It submits 16 launches, receives completion callbacks and validates the output on
each available enabled backend.

A session can span multiple queues. Completion callbacks and
`takeCompletedMeasurements()` expose ready operations independently of submission
order. Storage is bounded; `getStats()` reports dropped deliveries and failures.
Overflow leaves direct handles valid. Callbacks should be short; they may not
call `drain()`. Queue history remains explicit: use `clearMeasurements()` to release it.

Each result retains operation, session and queue IDs, device/API identity,
provider scope, native event and unit, conversion, collector identity/version/path,
and synchronization/replay flags. Unavailable metrics have a status and diagnostic
instead of a fabricated value.

Collector capabilities select the collection path automatically; applications
request metrics through the same `Config`, handles and callbacks. Asynchronous
collector results become ready after execution and metric delivery both finish.
The optional asynchronous plugin interface supports host, CUDA and HIP kernels
on timing-enabled queues; PAPI retains its existing synchronous counter path.
Optional CUPTI and ROCprofiler-SDK plugins provide correlated GPU kernel execution
time. PAPI remains independently available for CPU and GPU hardware counters; see
[provider setup](docs/plugins.md#native-gpu-activity-providers).

PAPI CPU presets and explicit native mappings are collected by the plugin.
**PAPI GPU counters require `Config::allowSynchronization = true`** and retain
context/device scope. Timing and result streaming remain asynchronous on
non-blocking queues.

## Results and optional output

`HostSideInstrumentation` supplies calling-thread regions with `begin()`/`end()`.
`metric::map()` binds semantic metrics to native counters and explicit conversions.
Discovery is available through `getAvailableMetrics()` and `getCollectorInfo()`.
The fallback provider defaults to PAPI and can be changed with
`Config::defaultCollectorPlugin` or `ALPAKA_METRICS_DEFAULT_COLLECTOR_PLUGIN`.
Enabled native providers still take precedence for their activity metrics.
Collectors can be explicitly selected with `Config::collectorPlugin` or
`ALPAKA_METRICS_COLLECTOR_PLUGIN`. The default search checks beside the application,
its parent directory, the installation library directory relative to that parent,
and the platform library search paths.
Runtime module loading currently supports POSIX platforms.

JSON export is explicit: `toJson(result)` returns a string and writes no files.
It can be used in a callback for an application-managed JSON Lines stream.
Roofline analysis remains separate from collection and requires compatible scopes,
units and calibrated ceilings. The alpaka3-tuner custom objective can consume
results, but its current feedback API targets its most recent launch.

See [collection contracts and usage](docs/collection.md) and the
[collector plugin interface](docs/plugins.md).

Licensed under MPL-2.0. The adapted vector-add example retains [ISC](LICENSES/ISC.txt).
Fetched dependencies retain their own licenses.

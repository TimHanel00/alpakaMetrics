# alpakaMetrics
This repository adds support native runtime metrics support for alpaka 3 (https://github.com/alpaka-group/alpaka3) as an optional instrumentation adapter.
The goal is to enhance profiling utilities across Vendor APIs. Most utilities and metrics are enabled by PAPI, which remains a optional but important dependency of this repository. 
PAPI has support for CPU, CUDA and HIP specific hardware counters and is therefore a natural fit, that is compatible with alpakas generic approach on supporting multiple vendor APIs.

## Build

Alpaka and PAPI sources are downloaded with CMake FetchContent. Both revisions
are pinned. **PAPI is optional and ON by default.** The initial bundled PAPI
build requires Linux and Autotools. System PAPI can be supplied separately.
The bundled configuration enables PAPI's default platform components.
GPU and energy components require their corresponding SDKs and can be selected
explicitly.

```sh
cmake -S . -B build -G Ninja \
  -DalpakaMetrics_BUILD_TESTING=ON \
  -DalpakaMetrics_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/example/alpakaMetrics_host
```

Disable PAPI with `-DalpakaMetrics_DEP_PAPI=OFF`. Timing and the queue adapter
remain available. To use installed PAPI, set
`-DalpakaMetrics_USE_SYSTEM_PAPI=ON` and supply `CMAKE_PREFIX_PATH` if needed.
An existing `PAPI::PAPI` target is also accepted.

Additional bundled components are selected with, for example:

```sh
cmake -S . -B build-papi \
  '-DalpakaMetrics_PAPI_COMPONENTS=sde;rapl'
```

Use a separate build directory when changing component sets. Components such as
`cuda`, `rocp_sdk`, and `intel_gpu` need vendor libraries and their PAPI-specific
environment configuration. Enabling a component does not guarantee that the
driver, permissions, hardware, or requested counter combination permits
collection.

For a local Alpaka checkout, pass
`-DFETCHCONTENT_SOURCE_DIR_ALPAKA3=/absolute/path/to/alpaka`. A previously
provided `alpaka::alpaka` target takes precedence.

## Use from another CMake project

```cmake
include(FetchContent)
FetchContent_Declare(alpakaMetrics SOURCE_DIR /absolute/path/to/alpakaMetrics)
FetchContent_MakeAvailable(alpakaMetrics)

add_executable(application main.cpp)
target_link_libraries(application PRIVATE alpakaMetrics::alpakaMetrics)
alpaka_finalize(application)
```

The adapter does not select an accelerator for the application. Configure
Alpaka's backend options normally. Installed consumers may instead use
`find_package(alpakaMetrics CONFIG REQUIRED)`; installation includes the fetched
Alpaka package and, for bundled PAPI, its private runtime libraries.

## Queue-attached instrumentation

```cpp
#include <alpakaMetrics/alpakaMetrics.hpp>

auto rawQueue = device.makeQueue(
    alpaka::queueKind::nonBlocking, alpaka::timing::enabled);
alpakaMetrics::Config config{
    .metrics = {alpakaMetrics::metric::elapsedTime,
                alpakaMetrics::metric::instructions},
    .label = "kernel"};
auto queue = alpakaMetrics::makeQueue(rawQueue, config);

auto measurement = queue.enqueue(frameSpec, kernel, args...);
// Other host work can proceed while the queue executes.
auto result = measurement.getResults(); // waits for this measurement
```

The kernel is submitted once. `getResults()` and `alpaka::onHost::wait(measurement)`
wait for completion. Retaining a handle keeps its result associated with its launch.
`queue.enqueueHostFn(task)` measures the host function on its executing thread
and propagates task exceptions through the measurement handle.

Queue copies share the underlying queue and measurement history.
`getMeasurements()` returns retained handles. Call `clearMeasurements()`
periodically during long tuning runs; externally retained handles remain valid.
The underlying queue must remain usable until pending work completes.

`get()` exposes the original Alpaka implementation for generic native-handle
queries and waiting. Operations submitted through `getUnderlyingQueue()`,
`enqueueNativeFn()`, or `enqueueHostFnDeferred()` are forwarded without profiling.
Use the underlying queue for Alpaka free functions whose overloads specifically
require an `alpaka::onHost::Queue`, including memory-copy operations.

## Host-side regions

```cpp
auto hostSession = alpakaMetrics::HostSideInstrumentation(config);
hostSession.begin();
queue.enqueue(frameSpec, kernelA, argsA...);
hostFunction();
queue.enqueue(frameSpec, kernelB, argsB...);
alpaka::onHost::wait(queue);
auto region = hostSession.end();
```

Host elapsed time is the wall interval between `begin()` and `end()`. Without the
explicit wait, it includes submission rather than completion of asynchronous
kernels. CPU thread counters describe the calling thread, not the queue worker
or an OpenMP team. They may include CPU time spent in the wait implementation.
Native PAPI metrics retain their own component-defined scope and units.

Begin, end, and destroy an active session on the same thread. Overlapping regions
can compete for counter resources; conflicts are reported per metric.

## Implemented measurement semantics

| Execution path | Timing | Counters |
| --- | --- | --- |
| Host region | Host wall interval | PAPI presets and native events, with component scope |
| Queued host function | Function execution interval | Calling-thread counters on the executing callback thread |
| CPU kernel with explicit `exec::cpuSerial` | Host queue execution interval | PAPI CPU counters on the queue worker |
| OpenMP/TBB/default CPU executor | Host queue execution interval | `unsupportedScope`; no launcher-only counters passed off as team counters |
| GPU/SYCL kernel | Alpaka timing-event interval | `unsupportedScope` until a native attribution provider is implemented |

Queue intervals exclude work queued before the start marker. They can include
queue bookkeeping and idle gaps **between** markers, so they are not labelled
kernel-exclusive durations. Device timing requires a timing-enabled queue.
The portable event provider is covered by host-event tests; CUDA, HIP and SYCL
hardware execution has not yet been validated.

Semantic CPU mappings currently include cycles, instructions, floating-point
operations and L2/L3 total cache misses through their PAPI presets. Always inspect
the returned native definition; cache-event meaning can vary with hardware.
The occupancy, energy, frequency and transferred-byte tags require an explicit
native mapping. Without one, they return `unsupported`.

Additional tags cover L1 data misses, L2/L3 accesses, branches and mispredictions,
loads, stores, and resource stalls. Availability depends on hardware and PAPI.
Cache counts are not transferred bytes; resource stalls do not identify L2/L3
stalls. Preset definitions are documented in
[PAPI's event definitions](https://github.com/icl-utk-edu/papi/blob/72a3124d048dc5c89eb3f00c9f2866f4492b5383/src/papiStdEventDefs.h).

### Native mappings and units

Bind a semantic tag to an event from a configured PAPI component:

```cpp
alpakaMetrics::Config config{.metrics = {
    alpakaMetrics::metric::elapsedTime,
    alpakaMetrics::metric::map(
        alpakaMetrics::metric::energy,
        alpakaMetrics::metric::native(
            "rapl:::PACKAGE_ENERGY:PACKAGE0",
            alpakaMetrics::MetricUnit::joules, 1.0e-9))}};
auto hostSession = alpakaMetrics::HostSideInstrumentation(config);
```

This example selects package 0 explicitly. The pinned RAPL component reports
its package-energy event in nJ, so the declared conversion produces joules.
It requires a build with the `rapl` component and permission to collect it.
See [PAPI's RAPL implementation](https://github.com/icl-utk-edu/papi/blob/72a3124d048dc5c89eb3f00c9f2866f4492b5383/src/components/rapl/linux-rapl.c).

Other mappings need verified units and a positive finite scale. Mapping preserves
provider scope and sampling semantics; it does not establish per-kernel
attribution. Device/context events cannot be collected as CPU queue worker counters.

```cpp
auto available = alpakaMetrics::HostSideInstrumentation::getAvailableMetrics();
// Discovery describes supported event definitions, not guaranteed collectibility.
config.metrics = {alpakaMetrics::metric::native("PAPI_TOT_INS")};
```

Every result includes status, provider, native name, definition, unit, scope and
diagnostic. Native units are preserved in `nativeUnit`; no unverified energy or
frequency unit conversion is performed unless explicitly declared. Values retain signed/unsigned 64-bit or
floating-point representations. `asDouble()` is a convenience conversion and
may lose integer precision. Unsupported metrics have no value, and converting
them throws. Collection does not automatically replay kernels or multiplex
counter sets. PAPI's global runtime is not shut down by this library.

## Tuner interoperability

The current alpaka3-tuner accepts the adapter as a queue. Its custom-objective
interface can consume the completed measurement:

```cpp
tuner.enqueue(queue, frameSpec, tunableBundle);
auto measurement = queue.getMeasurements().back();
tuner.provideMetric(measurement.getResults().getMetric("elapsed_time").asDouble());
```

Provide the objective before the next tuner enqueue: `provideMetric()` refers to
the most recent launch. Built-in tuner timing also works with the adapter.
Enable the optional integration test with `-DalpakaMetrics_BUILD_TESTING=ON`
and `-DalpakaMetrics_TUNER_SOURCE_DIR=/absolute/path/to/alpaka3-tuner`.

## Roofline analysis

The analysis layer is independent of collection and of the tuner:

```cpp
alpakaMetrics::RooflineCeilings ceilings{
    .computeFlopsPerSecond = measuredComputeCeiling,
    .memoryBytesPerSecond = measuredMemoryCeiling,
    .computeMode = "FP64",
    .memoryLevel = "DRAM",
    .provenance = "device calibration under the workload's execution configuration"};
auto analysis = alpakaMetrics::analyzeRoofline(measurement.getResults(), ceilings);
```

The result must contain available `elapsed_time` in seconds,
`floating_point_operations` as an operation count, and `transferred_bytes` in
bytes. `RooflineMetrics` selects alternative names for native mappings. The
operation convention (including fused multiply-add counting), compute mode,
memory level and device configuration must agree with the supplied ceilings.

The analysis returns arithmetic intensity (FLOP/byte), achieved FLOP/s and byte/s,
ridge point, the applicable roofline ceiling, fraction of that ceiling, and
whether the selected model ceiling is compute or bandwidth limited. This model
classification is not proof of the workload's actual bottleneck. Measurements
above the ceiling remain visible through `exceedsCeiling`; they are not clamped.

Time, counts and ceilings must be positive and finite. Missing metrics, wrong
units, mismatched attribution, missing ceiling provenance and unrepresentable
derived values are rejected. Cache miss/access counts cannot stand in for bytes.
The result overload accepts host-region timing with calling-thread counts, or
queue-interval timing with queue-worker counts. It also accepts counts explicitly
attributed to the corresponding host region or queue interval. Device-wide,
context-wide and provider-defined counts cannot automatically be paired with
per-queue timing.

If an external collector establishes matching attribution, use the scalar
overload explicitly:

```cpp
auto analysis = alpakaMetrics::analyzeRoofline(
    alpakaMetrics::RooflineSample{elapsedSeconds, executedFlops, transferredBytes},
    ceilings);
```

That overload relies on the caller to establish a common measured region. No
hardware calibration, vendor counter selection or automatic plot generation is
performed. This is the analysis layer, not an NCU-equivalent collection workflow.
See [NERSC's Roofline methodology](https://docs.nersc.gov/tools/performance/roofline/)
for measurement and calibration requirements.

GPU dispatch counters, OpenMP/TBB worker aggregation, automatic telemetry sampling
and roofline device calibration are **planned, not implemented**. Native mappings
and roofline analysis do not remove these collection requirements.

See the [PAPI documentation](https://github.com/icl-utk-edu/papi) for component setup.

Licensed under MPL-2.0. Fetched dependencies retain their own licenses.

# alpakaMetrics

A standalone, optional instrumentation adapter for Alpaka 3. It uses Alpaka-style
handles, semantic concepts, named metric tags, and `internal::*::Op` dispatch.
Neither Alpaka nor alpaka3-tuner depends on this repository. This repository is
currently local and unpublished.

## Build

Alpaka and PAPI sources are downloaded with CMake FetchContent. Both revisions
are pinned. **PAPI is optional and ON by default.** The initial bundled PAPI
build requires Linux. System PAPI can be supplied separately. PAPI uses Autotools;
an ExternalProject builds a private source copy inside the build directory and
stages its libraries without running `ldconfig` or installing system packages.
The initial bundled configuration enables PAPI's default platform components.
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
collection. PAPI 7.2.0 is the initial pinned version; the revision and configure
arguments are overridable for newer vendor-toolkit requirements.

For a local Alpaka checkout, pass
`-DFETCHCONTENT_SOURCE_DIR_ALPAKA3=/absolute/path/to/alpaka`. A previously
provided `alpaka::alpaka` target takes precedence. Source overrides are never
configured or built in place by the PAPI integration.

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
wait for completion. Each measurement has a stable ID, so retaining a handle
keeps the result associated with its own launch even after later submissions.
Synchronization events are forwarded without creating extra measurements.
`queue.enqueueHostFn(task)` measures the host function on its executing thread
and propagates task exceptions through the measurement handle.

Queue copies share the underlying queue, submission lock, and measurement
history. `getMeasurements()` returns retained handles. Call `clearMeasurements()`
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

`begin()`/`end()` may be repeated. Ending an inactive region, beginning an active
one, or ending on another thread throws. An active session must also be destroyed
on the thread that began it. Overlapping regions can conflict over limited
counter resources; those conflicts are reported per metric.

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
The occupancy, energy and frequency tags currently return `unsupported` until a
semantic provider mapping exists. Native energy/frequency events can be requested
through PAPI when their components are configured and accessible.

```cpp
auto available = alpakaMetrics::HostSideInstrumentation::getAvailableMetrics();
// Discovery describes supported event definitions, not guaranteed collectibility.
config.metrics = {alpakaMetrics::metric::native("PAPI_TOT_INS")};
```

Every result includes status, provider, native name, definition, unit, scope and
diagnostic. Native units are preserved in `nativeUnit`; no unverified energy or
frequency unit conversion is performed. Values retain signed/unsigned 64-bit or
floating-point representations. `asDouble()` is a convenience conversion and
may lose integer precision. Unsupported metrics have no value, and converting
them throws. Collection does not automatically replay kernels or multiplex
counter sets. PAPI's global runtime is not shut down by this library.

Native values follow PAPI's reported datatype. PAPI 7.2's SDE component does not
report floating-point datatype metadata; use integer SDE counters with this
version. SDE can also accept unregistered names as placeholders, so discovery
and successful registration do not prove that an application supplies a counter.

## Tuner interoperability

The current alpaka3-tuner accepts the adapter as a queue. Its custom-objective
interface can consume the completed measurement:

```cpp
tuner.enqueue(queue, frameSpec, tunableBundle);
auto measurement = queue.getMeasurements().back();
tuner.provideMetric(measurement.getResults().getMetric("elapsed_time").asDouble());
```

Provide the objective before the tuner's next enqueue: its current
`provideMetric()` contract refers to the most recent launch. This example does
not claim asynchronous feedback support inside the tuner. Built-in tuner timing
also composes with the adapter; its synchronization events are forwarded.

An optional interoperability test reads a supplied tuner checkout without
changing it or adding a runtime library dependency:

```sh
cmake -S . -B build-tuner -DalpakaMetrics_DEP_PAPI=OFF \
  -DalpakaMetrics_BUILD_TESTING=ON \
  -DalpakaMetrics_TUNER_SOURCE_DIR=/absolute/path/to/alpaka3-tuner
cmake --build build-tuner
ctest --test-dir build-tuner --output-on-failure
```

## Further providers and roofline analysis

Backend providers specialize `alpakaMetrics::internal::Enqueue::Op<Api>` and
return a `Measurement`. Keep vendor resource management and dispatch attribution
inside those specializations. Do not invoke GPU profiling APIs from vendor host
callbacks. A future collection plan must expose synchronization, serialization,
replay and attribution scope before execution.

GPU dispatch counters, OpenMP/TBB worker aggregation, telemetry sampling and
roofline calibration/analysis are **planned, not implemented**. Roofline analysis
will need executed-operation counts, traffic at a specified memory level, timing
and independently established device ceilings. Occupancy alone cannot provide it.

Primary references:

- [PAPI overview](https://github.com/icl-utk-edu/papi)
- [PAPI CUDA component](https://github.com/icl-utk-edu/papi/blob/master/src/components/cuda/README.md)
- [PAPI ROCprofiler-SDK component](https://github.com/icl-utk-edu/papi/blob/master/src/components/rocp_sdk/README.md)
- [PAPI Intel GPU component](https://github.com/icl-utk-edu/papi/blob/master/src/components/intel_gpu/README.md)
- [CUPTI collection and replay](https://docs.nvidia.com/cupti/main/main.html)
- [ROCprofiler counter collection](https://rocm.docs.amd.com/projects/rocprofiler-sdk/en/latest/api-reference/counter_collection_services.html)
- [Nsight Compute roofline methodology](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html)
- [PAPI exascale paper](https://doi.org/10.1177/10943420241303884)

Licensed under MPL-2.0. Fetched dependencies retain their own licenses.

# Collector plugins

Instrumentation, collection and output have separate roles. The architectural
separation takes inspiration from
[`alpaka-group/bactria`](https://github.com/alpaka-group/bactria).
alpakaMetrics owns Alpaka 3 operation identity, queue integration, completion
tracking and direct results. Collectors own event selection, units, scopes and
collection errors. Export consumes completed results independently.

The core is header-only and has no PAPI link dependency. Enabling
`alpakaMetrics_DEP_PAPI` builds `libalpakaMetrics_papi.so`, which privately links PAPI. Bundled installations
place the module and its PAPI runtime in the installation library directory.
The module can be removed without disabling timing; requested counters then report unavailable.

Collector selection, in priority order:

1. `Config::collectorPlugin`, an explicit module path.
2. `ALPAKA_METRICS_COLLECTOR_PLUGIN`, an environment override.
3. `libalpakaMetrics_papi.so` beside the application, in its parent directory,
   in the configured installation library directory relative to that parent,
   or on the platform library search paths. For applications installed elsewhere,
   select the module explicitly or configure the library search path.

`getCollectorInfo(path)` and `getAvailableMetrics(path)` inspect an explicit
collector. Invalid explicit selections produce per-metric `collectionFailed`
results; an absent default produces `dependencyDisabled`. Timing remains usable.
Collector identity, version and resolved module path are attached to metric
provenance. Module paths identify the loaded artifact, not a cryptographic hash.
Successfully loaded modules remain resident for process lifetime because collectors
can own process-global runtime state and thread-local teardown.

## ABI and lifecycle

Implement the versioned POD interface in
[`plugin/Collector.hpp`](../include/alpakaMetrics/plugin/Collector.hpp). Export:

```cpp
extern "C" alpakaMetrics::plugin::Collector const*
alpakaMetrics_getCollector() noexcept;
```

The returned table has static lifetime and declares an ABI version, table size,
name, actual provider version, capabilities and lifecycle/discovery functions.
No STL objects, ownership or exceptions cross the boundary. Strings passed to an
emit callback remain valid through that call; the core copies every field. Metric
enum fields use the documented enums in `Result.hpp`. Missing requested results
remain failed entries. Modules must emit each requested metric at most once.

ABI version 1 provides thread-affine counter sessions: create, begin, end and
destroy run on the collection thread. CPU queue counters use the queue worker.
Synchronized device counters use the submitting thread and must declare
`requiresDeviceSynchronization`. Discovery emits descriptors without values.
See [the PAPI adapter](../plugins/papi/Plugin.cpp) for a concrete module.

The completion service polls Alpaka events and publishes immutable snapshots.
Collector sessions remain on their collection thread.

## Asynchronous operation collection

An asynchronous module exports an independent entry point, preserving the
existing collector ABI:

```cpp
extern "C" alpakaMetrics::plugin::AsyncCollector const*
alpakaMetrics_getAsyncCollector() noexcept;
```

A module may export either or both entry points. For tracked host, CUDA and HIP
kernels on timing-enabled queues, the asynchronous interface takes precedence.
Host regions and host tasks use the legacy interface. The asynchronous core is
validated with a deterministic test module; no native asynchronous hardware
collector is bundled yet.

`AsyncCollector::create` receives requested metrics, device options, an
`Operation` and an `AsyncSink`. Copy borrowed requests, strings, options and sink
fields before returning; the operation ID and native queue identify the submission.
`Options::deviceApi` is `host`, `cuda` or `hip`. `Operation::nativeQueue` is a host
queue index or an encoded CUDA/HIP stream pointer. CUDA/HIP create and submission
calls run with the queue device selected. Providers must retain any additional
native context needed for later calls.

The lifecycle is:

1. `create`, `begin` and `submitted` run on the submitting thread. `begin` and
   `submitted` bracket queue markers and exactly one kernel launch. `submitted`
   also closes the bracket if submission throws; destruction then cancels delivery.
2. After the execution event completes, the core calls `poll(session, true)`.
   Calls are serialized per operation and may run on a result reader or the
   session progress thread. Polling must return promptly and must not wait for
   device work or record arrival; providers may request nonblocking flushes here.
3. Provider threads may call `sink.emit(context, measurementId, metric)`.
   The core copies the record immediately. Emit each requested metric at most
   once with its actual units and attribution scope.
4. Call `sink.complete(context, measurementId, success, diagnostic)` after all
   records for that operation have been emitted. The diagnostic pointer must be
   non-null. Success seals the result; omitted metrics remain failed entries.
   Failure marks all requested collector metrics `collectionFailed`.
5. `destroy` may run on any thread. It must cancel pending work and quiesce all
   callbacks before returning, including when collection never began. It must
   not wait for GPU execution or callbacks into application code. Providers must
   release submission-thread correlation state in `submitted`.

The sink supports concurrent provider callbacks. Completion must happen after
all emit calls return; further records are ignored. Incorrect operation IDs,
malformed records and duplicate or unrequested metrics fail collection.
Callbacks only update core storage; user callbacks run through `Session`.

Execution completion and record readiness are independent. Results wait for
both; a stalled provider becomes failed after the core's 30-second delivery
window following observed execution completion. Destruction still must quiesce
callbacks promptly. Providers remain responsible for reporting dropped records
and collection conflicts, and must not synchronize or replay secretly.

`requiresDeviceSynchronization` is honored by both interfaces. Without
`Config::allowSynchronization`, such an asynchronous provider reports
`unsupportedScope`. With opt-in, queue execution is synchronized and the result
records `synchronized`. An ordinary asynchronous provider adds neither per-launch
waits nor replay.

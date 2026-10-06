# Collector plugins

Instrumentation, collection and output have separate roles. The architectural
separation takes inspiration from
[`alpaka-group/bactria`](https://github.com/alpaka-group/bactria).
alpakaMetrics owns Alpaka 3 operation identity, queue integration, completion
tracking and direct results. Collectors own event selection, units, scopes and
collection errors. Export consumes completed results independently.

The core library has no PAPI link dependency. Enabling `alpakaMetrics_DEP_PAPI`
builds `libalpakaMetrics_papi.so`, which privately links PAPI. Bundled installations
place the module and its PAPI runtime beside the core library. The module can be
removed without disabling timing; requested counters then report unavailable.

Collector selection, in priority order:

1. `Config::collectorPlugin`, an explicit module path.
2. `ALPAKA_METRICS_COLLECTOR_PLUGIN`, an environment override.
3. `libalpakaMetrics_papi.so` beside the loaded core library.

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

---
status: current
owner: process-bootstrap
last_verified: 2026-08-22
related_code:
  - src/telemetry/
  - src/Main.cpp
  - include/gargantuan/editor/EditorHost.hpp
  - src/editor/EditorHost.cpp
  - tests/OptionalTelemetryTests.cpp
related_adrs: []
---
# Optional telemetry host integration

## Ownership and availability

Telemetry is an optional process-level observer. `Main.cpp` owns one
`telemetry::OptionalTelemetry` for the complete Engine or EditorHost process;
it is not owned by `Engine`, `DataModel`, `EditorHost`, a project, or a Play
session. The adapter contains the only telemetry ABI calls in this repository.
The callback accepted by `EditorHost::Run` is a generic fail-open process
observer used only to check a cached performance deadline after a request.

Both Crash Reports and Performance Snapshots default disabled. With both
disabled, the adapter does not try to load a library, create telemetry storage,
register crash observation, or start telemetry work. Standalone Engine enables
a category only through the explicit process arguments
`--telemetry-crashes` and `--telemetry-performance`.

Studio-launched EditorHost accepts the three closed environment fields
`GARGANTUAN_TELEMETRY_CRASHES`, `GARGANTUAN_TELEMETRY_PERFORMANCE`, and
`GARGANTUAN_TELEMETRY_LAUNCH_ID` only in `--editor-host` mode when the launch
token has Studio's random 64-lowercase-hex form. A standalone Engine never
treats ambient values as consent. The launch ID becomes only the child's
nonpersistent parent-launch correlation ID; the EditorHost creates its own
random launch ID.

## Discovery and ABI negotiation

The normal path is derived from the operating system's current executable
location, never the working directory, project root, `.gargantuan`, `PATH`, a
script location, or project input:

| Platform | Executable-adjacent filename |
| --- | --- |
| Windows | `gargantuan_telemetry.dll` |
| Linux | `libgargantuan_telemetry.so` |
| macOS | `libgargantuan_telemetry.dylib` |

Windows uses `LoadLibraryExW` with DLL-directory and system-directory search
constraints. Unix-like hosts use `dlopen` with immediate, local resolution.
The adapter resolves exactly `GargantuanTelemetry_GetApi`, requests major ABI
1 with its caller table size, and validates the returned ABI, structure size,
and every required V1 function pointer before initialization. There is no
static telemetry link and Rust is not an Engine build dependency.

An absent image is a silent `Unavailable` state. An invalid image, missing
export, incompatible/short API table, initialization failure, caught ABI
exception, or unhealthy runtime status produces at most one fixed stderr
diagnostic and permanently changes that adapter to no-op. No diagnostic uses
EditorHost stdout. Initialization failure unloads the not-yet-owned image;
after successful initialization V1 remains mapped until process exit. Shutdown
uses a 100 ms best-effort flush request and contains every returned error or
exception.

## Exact Engine and EditorHost data

Initialization forwards only:

- the closed component (`engine` or `editor_host`);
- fixed application version `0.0.0`, build ID `gargantuan-main`, and build
  configuration `debug` or `release`;
- a random per-process launch ID and optional random Studio parent launch ID;
- the two category bits; and
- an absolute application-local telemetry storage directory.

The current host integration supplies no collector endpoint or routing key.
A distributor operating a collector must supply those as trusted build/install
configuration without accepting project input.

Controlled-fatal reports contain only a fixed host-owned numeric failure code,
the current closed phase, process uptime, and zero host-supplied frames. Raw
exception text and logs remain in the existing local error path and never enter
the telemetry report. When Crash Reports consent is active, the V1 library owns
its documented Windows unhandled-crash observer and bounded crash spool.

Engine performance observation maintains only a fixed in-process accumulator:
frame count, saturating total/average, maximum, count above 50 ms, and eight
fixed buckets with upper bounds 8.333, 16.667, 25, 33.333, 50, 100, 250, and
1,000 ms. Approximate p50/p95 are derived from those buckets. A frame performs
no allocation, lock, telemetry ABI call, storage, or network operation before
the cached V1 due uptime. At that due point the adapter submits one fixed
snapshot and refreshes the library-owned schedule. EditorHost supplies no frame
block and polls only after bounded protocol requests.

## Explicit exclusions

The adapter has no arguments or fields for project path/name, script source,
Output/log text, usernames, machine name, environment contents, account IDs,
Instance names, persistent identifiers, command lines, input, screenshots,
minidumps, DataModel pointers, Studio documents, Luau values, or arbitrary
events. It does not traverse or serialize project state. Only the three closed
Studio child-policy environment values are read, and their names/values are not
forwarded as arbitrary environment data.

## Consent, revocation, and deployment

Engine command-line category choices last for that process. `SetConsent`
remains a narrow adapter operation so a host-owned preference surface can apply
revocation without exposing the ABI elsewhere; V1 performs its queue/spool
purge. Studio owns its separate persisted preferences and invokes the same ABI
transition before saving a revoked choice.

A distribution opts in by placing the correctly named library beside the
corresponding executable and establishing explicit local consent. The Engine
build never copies, downloads, repairs, or requires it. A distributor opts out
completely by omitting or deleting that single library; Engine startup,
EditorHost startup, project open, Play/Stop, Save, and shutdown retain their
ordinary paths.

## Verification

`gargantuan_optional_telemetry` builds host-independent fake dynamic libraries
and covers all-disabled, absent, valid, wrong ABI, short table, missing export,
initialization failure, runtime submission failure, shutdown failure,
independent categories, revocation, repeated phase/Play-style lifecycle, fixed
crash/performance structures, and forbidden-data canaries. The fake libraries
are test targets only and are never deployed with the Engine.

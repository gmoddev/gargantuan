# Distributed Player package format 3

Status: Foundation 10A integration candidate. Public distribution requires the
audience and native multiplayer qualification gates below.

The distributed Player builds from the same authoritative capture entry point
as trusted offline and Server packages. Capture produces an immutable client
code and asset projection for one project and revision on Main. The package
worker stages that projection. Trusted packages retain their format-1 reader,
format-2 writer, runtime compatibility, project payload and Server content
services.

## Authoring boundary

Build with `--audience DistributedPlayer`, or pass
`Audience: "DistributedPlayer"` to EditorHost `StartPackageBuild` with the
`DistributedPlayerPackages` handshake capability. Trusted builds remain the
default for existing offline, Server and controlled developer workflows.
`Development` and `Release` remain configuration choices within an audience.

A distributed build requires a native-only frozen schema and no project
PreRun file. A missing, empty or unsupported sharing configuration never
silently falls back to trusted content. A project supplies a bounded,
root-confined `.gargantuan/package-audience.json` file:

```json
{"Version":1,"SharedAssets":[]}
```

`SharedAssets` contains distinct canonical project AssetId strings. Empty is
an explicit asset-free projection. Include the IDs needed by replicated
visuals and client code, including runtime-selected assets and the complete
dependency closure. Missing, unknown, unavailable or unlisted dependencies
reject capture. Unlisted catalog records and artifacts are excluded from the
distributed output. The policy is a project input and is never staged itself.
Selecting an asset authorizes its whole canonical artifact.

Client startup code contains archivable Client Script sources outside
replicated-storage policy and shared ModuleScript sources under the existing
Replicated service policy. Non-archivable, FileLink, server-only service,
non-client Script and non-shared ModuleScript branches are excluded. Move
client dependencies into the existing shared ModuleScript scope before
shipping. Mounted source remains a local authoring feature. This contract
does not execute or copy the mount, copy arbitrary project properties, or
infer dynamic asset reachability.

## Manifest and startup

Format 3 adds required `Audience: "DistributedPlayer"` and defines exactly
two startup fields. Trusted formats retain their original closed shape.

| Field | Fixed relative path |
| --- | --- |
| `Startup.ClientCode` | `content/client-code.json` |
| `Startup.AssetCatalog` | `content/assets/catalog.json` |

The code document contains exactly `Version: 1` and a `Code` array. Each
entry contains `Path` as an array of exact UTF-8 name segments relative to
the DataModel, `Class` as `Script` or `ModuleScript`, and `Source` as bounded
UTF-8 Luau text. Entries sort by segment sequence and class. Duplicates reject.
The code index has a 16 MiB limit, 256-byte segment limit, and the existing
script, Instance-count and hierarchy-depth bounds. A path segment retains its
literal spelling, including dots.

The distribution stages the supported Engine Player executable and closed
runtime resources from the trusted Engine build. The Engine's GNS-enabled
Player build writes a tool-only `distributed-player-capability.json` with
the supported format and the Player executable digest. Packaging requires
that digest to match the staged executable. Older or transport-disabled
distributions cannot produce this audience. The capability file stays out
of the shipped artifact.

Any other project-category file or unreferenced asset artifact rejects
validation. The existing content table hashes and atomic sibling staging
apply to the complete artifact. Current packagers serialize publication
through a per-destination sibling directory and revalidate the destination
under that lease. A valid package cannot replace a destination of a
different audience. Manifest integrity is relative to the trusted
distribution and manifest inputs. If a publisher terminates abruptly, it can
leave an empty `.gpk-publish` sibling. Remove that empty directory only after
confirming that publication at the destination has stopped.

The Player joins a multiplayer endpoint, loads the shared asset catalog into
the authoritative replica and hydrates matching code targets before attaching
the client Engine. Matching uses the exact name segments, class and current
Script or ModuleScript policy. Ambiguous targets and conflicting existing
source reject. Hierarchy, gameplay state and later shared-module changes
remain owned by normal replication. Format 3 carries no authoritative world,
region provider, project PreRun or source-mount path and cannot launch offline.
Client Script targets are populated at initial replica startup. Client
scripts first materialized after that require a later runtime feature.

## Qualification gate

Check public candidate artifacts with `gargantuan-packager validate <package>
--audience DistributedPlayer`. Default validation continues to check trusted
package integrity. An audience check establishes content shape and integrity.
The builder additionally checks the trusted GNS runtime capability. Require
the exact Player/Server multiplayer smoke,
ordinary JSON/GASP/EditorHost artifact checks, private fixture omission,
authorized shared asset closure, and trusted Server/offline compatibility
before approving a release. Studio should expose the target as a distinct
explicit build choice and use a distinct destination.

---
status: planned
authority: non-normative
owner: assets
related_current_architecture:
  - devdocs/CurrentArchitecture/AssetFoundation2A.md
  - devdocs/CurrentArchitecture/SpatialRuntimeProjectionFoundation3K.md
---

# Solid geometry compiler research

## Validated direction

Gargantuan should treat constructive solid geometry (CSG) as a versioned,
bounded geometry compiler. An editable authoring graph should compile
transactionally into an ordinary canonical `Mesh` asset and produce collision
data through the prerequisite, backend-neutral Asset Foundation 2B contract.
Rendering, physics, replication, and spatial indexing should consume only those
compiled artifacts and should not evaluate or retain the authoring graph.

```text
editable solid graph
    -> bounded headless compiler
    -> canonical Mesh + collision output through Asset Foundation 2B
    -> ordinary AssetService/runtime projections
```

The key boundary is:

```text
authoring representation != render representation != collision representation
```

This direction is consistent with the current repository:

- Asset Foundation 2A already owns canonical platform-neutral indexed triangle
  meshes, atomic compound import, stable source-group logical keys, revisions,
  and shared renderer residency.
- `MeshPart` physics still intentionally uses inherited box geometry.
- Asset Foundation 2B already lists a separate mesh-collision asset boundary as
  future work.
- Foundation 3K keeps semantic Instances authoritative while orthogonal runtime
  systems own derived projections. A CSG graph should not become another
  runtime Instance capability or projection.

The external evidence also supports baked output. Godot can bake CSG separately
to `MeshInstance3D` and collision, citing faster loading and transform updates;
Unity ProBuilder's experimental Boolean tool creates a new mesh; and Unreal's
Boolean modeling tool emits mesh output. Roblox's current `GeometryService`
returns `PartOperation` or `MeshPart` results and exposes collision fidelity
separately from render fidelity.

## Kernel recommendation

[Manifold](https://github.com/elalish/manifold) is the first Boolean-kernel
candidate to prototype, not yet an accepted dependency. It requires manifold
inputs, reports invalid imported meshes, guarantees manifold output for its
supported input contract, preserves arbitrary vertex properties and source IDs,
and exposes `BatchBoolean`. Its documented CSG evaluation includes n-ary
flattening, size-based reordering, bounding-box shortcuts, parallel evaluation,
and shared subexpression caching. Blender 4.5 and Godot provide useful
integration evidence.

Before adoption, a prototype must establish:

- license and pinned-version/provenance policy;
- Windows and Linux build integration without weakening offline/reproducible
  builds;
- deterministic canonical bytes for the same graph, inputs, compiler version,
  and settings on every supported toolchain;
- material, normal, UV, tangent, and source-face provenance through cuts;
- cancellation, memory, node/depth, input/output triangle, and diagnostic bounds;
- behavior for coplanar, touching, near-coplanar, thin, disconnected, extreme
  scale, and malformed/non-manifold inputs; and
- measured quality and cost against at least one fallback or reference corpus.

[meshoptimizer](https://github.com/zeux/meshoptimizer) is a reasonable measured
post-process candidate for indexing, vertex-cache ordering, optional overdraw
ordering, and vertex-fetch ordering. It is not part of Boolean correctness and
must not be required unless benchmarks show a benefit for Gargantuan's renderer.

## Proposed ownership and lifecycle

A future `SolidModel` authoring asset may contain a bounded, versioned graph of
stable node identities for primitives, imported mesh operands, transforms,
union, intersection, and ordered difference. Associative union/intersection
should be representable as n-ary operations. Pointer identity, display names,
and array position must not be durable node identity.

The expected scene and runtime representation is an ordinary `MeshPart` whose
generated mesh and collision data retain source-group provenance back to that
authoring asset. `SolidModel` is authoring/source state, not a special runtime
`BasePart` subclass. Any alternative semantic Instance requires a separate ADR
and evidence that an ordinary `MeshPart` cannot preserve the required authoring
identity.

Compilation should validate the complete source revision, build visual and
collision candidates off-thread, validate and encode all artifacts, then use an
AssetService compound commit. Failure publishes no partial revision and leaves
the prior valid bake available. Stable logical child keys should preserve
semantic asset identity across rebakes while content revisions advance.

Normal packaged games should contain only the compiled mesh, collision, and
dependencies unless source retention is explicitly requested. They should
perform zero Boolean evaluations for pre-authored solids. A future runtime
geometry API, destruction system, automatic UV unwrap, LOD generator, or convex
decomposition backend is separate work and requires its own bounded asynchronous
contract.

## Roadmap placement

1. Build on the completed Foundation 3K semantic/projection boundary and finish
   Asset Foundation 2B's generic mesh-collision boundary.
2. Add Geometry Foundation 1A in Phase 2: prototype and gate a headless compiler
   that produces ordinary canonical mesh assets and collision data only through
   the established Asset Foundation 2B contract. This milestone includes corpus
   tests, fuzzing, determinism checks, bounds, cancellation, benchmarks, and
   transactional AssetService integration; it does not include Studio modeling
   UI or game-facing runtime CSG.
3. Add Geometry Foundation 1B in Phase 4: Studio union, intersection,
   difference, edit/separate, preview, collision inspection, diagnostics, and
   command-backed undo/redo over the headless compiler.
4. Defer runtime game-facing CSG and destruction until creator demand and a
   separately reviewed service contract justify them.

Exact asset kinds, schema IDs, serialized graph format, compiler ABI, collision
representations, and public names require prototype evidence and an ADR before
becoming contracts. Existing stable `AssetKind` values must not be renumbered.

## Validation notes and sources

Validated on 2026-09-07 against current repository architecture and primary
vendor/project documentation:

- [Godot CSG workflow](https://docs.godotengine.org/en/4.5/tutorials/3d/csg_tools.html)
- [Godot's Manifold-based CSG implementation](https://github.com/godotengine/godot/blob/master/modules/csg/csg_shape.cpp)
- [Unity ProBuilder Boolean tool](https://docs.unity.cn/Packages/com.unity.probuilder%406.1/manual/boolean.html)
- [Unreal Boolean modeling tool](https://dev.epicgames.com/documentation/en-us/unreal-engine/boolean-tool-in-unreal-engine)
- [Roblox GeometryService](https://create.roblox.com/docs/reference/engine/classes/GeometryService/FragmentAsync)
- [Roblox PartOperation](https://create.roblox.com/docs/reference/engine/classes/PartOperation/RenderFidelity)
- [Manifold API](https://github.com/elalish/manifold/blob/master/include/manifold/manifold.h)
- [Manifold performance notes](https://github.com/elalish/manifold/wiki/Performance-Considerations)
- [Blender 4.5 modeling notes](https://developer.blender.org/docs/release_notes/4.5/modeling/)
- [meshoptimizer pipeline](https://github.com/zeux/meshoptimizer#core-pipeline)

The supplied research also cited a Roblox staff statement about converging
UnionOperation storage with MeshPart assets, a legacy 20,000-triangle limit,
OpenSCAD's default-backend timing, and Manifold cross-platform determinism.
Those details were not needed for the architectural conclusion and were not
independently established strongly enough to make them roadmap premises.

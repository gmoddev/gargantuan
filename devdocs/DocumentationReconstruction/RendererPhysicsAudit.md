# Renderer and physics documentation reconstruction audit

The first audit below records the pre-integration main
`2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`. Its renderer limits are
historical at that source, not current guidance. The 2026-11-15 reconciliation
after it owns the current `renderer.mdx`, `physics.mdx` and
`render-extraction.mdx` claims. Neither pass is a new performance or platform
qualification.

## Original 2026-11-14 baseline

| Claim in site pages | Integrated evidence | Limit or classification |
| --- | --- | --- |
| Immutable publication and disposable render projection | `include/gargantuan/render/RenderPublication.hpp`, `src/render/RenderPublisher.cpp`, `src/render/RenderProjection.cpp`, `tests/RendererPublicationTests.cpp` | SDL resource application clears derived state and requires full resync on failure (`src/render/Renderer.cpp`); no general device-loss acceptance claimed. |
| SDL is the selected runtime backend | `include/gargantuan/render/SDLRenderer.hpp`, `src/render/Renderer.cpp`, `src/render/passes/`, `CMakeLists.txt`; `devdocs/CurrentArchitecture/RendererBackendBoundary.md` | Filament is optional experimental benchmark; no runtime backend switch or generic RHI. |
| Current material rendering is narrower than the neutral value | `RenderPublication.hpp`, `src/render/passes/OpaquePass.cpp`, `assets/shaders/opaque.frag`, `src/render/passes/ShadowPass.cpp` | Shader samples base color and handles masked alpha; normal texture and metallic/roughness are carried but unused for PBR; shadow pass lacks masked alpha sampling. |
| One sun, fixed shadow, Sky/fog/exposure, D32 scene depth | `src/render/passes/ShadowPass.cpp`, `src/render/passes/SkyPass.cpp`, `src/render/sdl/SDLSceneDepth.hpp`, `src/render/Renderer.cpp`; `devdocs/CurrentArchitecture/EnvironmentLightingFoundation1.md`, `devdocs/Validation/RendererDepthKI008.md` | Every visible `CastShadow` mesh draws an unmasked silhouette; no cascades, local lights, IBL, HDR output or complete color-transfer contract. Windowed swapchain format is queried from SDL; offscreen is fixed RGBA8 UNORM. |
| Box3D rigid and XPBD deformable worlds remain separate | `include/gargantuan/classes/WorldRoot.hpp`, `include/gargantuan/physics/PhysicsBackend.hpp`, `SoftBodyBackend.hpp`, `src/physics/Box3DPhysicsBackend.cpp`, `XpbdSoftBodyBackend.cpp`; `tests/PhysicsBackendTests.cpp`, `SoftBodyPhysicsTests.cpp` | Jolt is comparison material only; no Jolt runtime adapter. |
| Main safe-point sync, owned results, and bounded jobs | `src/classes/WorldRoot.cpp`, `include/gargantuan/physics/PhysicsTypes.hpp`, `SoftBodyTypes.hpp`; `devdocs/CurrentArchitecture/PhysicsBackend.md`, `SoftBodyPhysicsFoundation2.md`, `PhysicsFoundationSoftControl.md` | Soft collision is rigid-to-soft, discrete, and one-way; no soft/soft or soft query participation. |
| Shape casts and overlaps are integrated APIs | `include/gargantuan/physics/PhysicsQueryTypes.hpp`, `PhysicsBackend.hpp`, `src/classes/WorldRoot.cpp`, `src/physics/Box3DAdvancedQueries.inl`, `tests/PhysicsBackendTests.cpp`; `devdocs/CurrentArchitecture/PhysicsQueryFoundation1.md` | `PhysicsBackend.md` previously said these APIs were absent. Its query section is corrected here; exact platform qualification remains in its campaign receipts. |

## Original branch and qualification boundary

At that original main SHA, the separately pinned prospective commit on
`integration/engine-foundations-3-6` is
`4b32a822a26492907005ce18c3b1045550bfce86`. Read-only `git show` of its
`RendererFoundation3.md` and `RendererFoundation5.md` and the exact main-to-ref
path diff confirms that Renderer Foundations 3–5, Environment/Lighting 4,
HDR Environment Assets 5, and related Renderer 5 experiments are absent from
integrated main. The ref's experimental or qualified receipts cannot upgrade
main's material, lighting, shadow, HDR, instancing, culling, or residency
claims. Runtime 6 is likewise outside this main SHA. The site page's pending
table therefore describes main source rather than that ref's target behavior.

Historical acceptance sources remain distinct: `RendererFoundation2C.md`
records two-device SDL deformable/GUI acceptance; `RendererDepthKI008.md`
records D32 depth correction. The physics expansion inventory at
`devdocs/Validation/PhysicsFoundationExpansion20261107.json` explicitly calls
itself pre-qualification and says final-source hosted execution was pending at
its recorded checkpoint. API/source presence in main and a campaign's platform
qualification are different claims. This audit ran no native build, GPU test,
or physical qualification.

## Original review disposition

A proposed correction said missing collision content for an anchored
`MeshPart.CollisionMode = StaticMesh` would fall back to Box because
`MeshPart::GetPhysicsShape` starts as Box. The current implementation changes
the neutral kind to `StaticTriangleMesh` before resolving the Collision asset
(`src/classes/WorldRoot.cpp::DescribePart`); absent content leaves that
description incomplete and native admission fails. The focused fixture in
`tests/PhysicsBackendTests.cpp::TestMeshPartCollisionFallback` asserts that a
new anchored StaticMesh body waits for valid content and can later recover when
its collision mode is changed back to Box.
An existing unanchored Box body retains its previous coherent identity when
the anchored mesh update fails. The owning document and site page already
state these different outcomes, so this review finding was rejected.

## 2026-11-15 integrated-source reconciliation

Engine PR #24 is integrated at main
`b5df1e7b1b80da27158ea794626f90a8ebd8a6d2`; this documentation
worktree's merged source tree is
`7ef2a2b6b2bb34732bbb8335f30dff9588a68f39`. The integration
[ledger](../../devdocs/Validation/EngineFoundations3To6Integration.md)
describes original branch heads, core acceptance and separate device scopes.
Its pre-merge wording is historical; the current tree is checked against
production source and tests below. Original qualification SHAs, measured
values, failures and verdicts were not rewritten.

| Current claim | Production and test evidence | Limit |
| --- | --- | --- |
| Immutable publication and disposable projection remain authoritative seam | `src/render/RenderPublisher.cpp`, `RenderProjection.cpp`, `Renderer.cpp`; `tests/RendererPublicationTests.cpp` | Rejection preserves accepted state; required resource failure requests full resync. |
| Direct material PBR, normal, packed metallic-roughness, masks and sorted blends are present | `src/render/passes/OpaquePass.cpp`, `ShadowPass.cpp`, `assets/shaders/opaque.frag`; `tests/RendererDepthTests.cpp`, `RendererMaterialMipTests.cpp` | Primitive fallback uses legacy shading; blend order is object-center, not per-triangle. |
| HDR scene working target resolves once to SDR, with device-checked direct SDR fallback | `src/render/Renderer.cpp`, `src/editor/EditorViewport.cpp`, `src/render/sdl/SDLSceneColor.hpp`, `src/render/passes/SceneResolvePass.cpp`; `tests/EnvironmentLightingReferenceTests.cpp` | No HDR display promise or automatic exposure; runtime swapchain queried, offscreen output UNORM. |
| Local lights, global Sky IBL and bounded authored probes use immutable environment values | `src/render/RenderPublisher.cpp`, `src/render/passes/SkyPass.cpp`, `OpaquePass.cpp`; `tests/ReflectionProbeSemanticsTests.cpp`, `ReflectionProbeGpuTests.cpp` | No automatic capture/GI; 512 published lights, 32 probes, 12 resident and two sampled per point. |
| Default legacy directional shadow, optional cascades and spot atlas | `src/render/passes/ShadowPass.cpp`, `src/render/sdl/SDLShadowResources.hpp`; `tests/ShadowFoundation5GpuTests.cpp`, `DirectionalCascadeNumericsTests.cpp` | Cascade count defaults zero; at most four cascades/eight spot layers; no point cube shadow. |
| Bounded HDR RGBE import and material mip/source-view residency | `src/assets/AssetImporter.cpp`, `src/render/TextureProvider.cpp`; `tests/AssetFoundationTests.cpp`, `RendererMaterialMipTests.cpp` | EXR, block compression and complete base-texture/mesh streaming absent. |
| Recovery and optional submission retain bounded scope | `src/render/Renderer.cpp`, `src/render/passes/OpaquePass.cpp`, `src/render/InstanceGpuCache.cpp`; `tests/Runtime6ReadbackFaultTests.cpp`, `RendererInstanceRecordCacheTests.cpp` | Fault/recreate/churn tests do not prove physical driver-removal recovery; instancing/envelope remain opt-in and 50K dynamic tails block default promotion. |
| Rigid and deformable physics ownership unchanged | `src/classes/WorldRoot.cpp`, `src/physics/Box3DPhysicsBackend.cpp`, `XpbdSoftBodyBackend.cpp`; `tests/PhysicsBackendTests.cpp`, `SoftBodyPhysicsTests.cpp` | Jolt draft experiments remain separate; no claim of new physical/provider F1 qualification. |

Historical [Renderer 3](../../devdocs/Validation/RendererFoundation3Qualification.md),
[Renderer 4](../../devdocs/Validation/RendererFoundation4Qualification.md)
and [Renderer 5](../../devdocs/Validation/RendererFoundation5Qualification.md)
receipts retain source-specific device and workload meanings. The integration
ledger distinguishes exact-source core qualification from unavailable or
unrun device lanes. No native suite or physical GPU test was rerun for this
documentation reconciliation.

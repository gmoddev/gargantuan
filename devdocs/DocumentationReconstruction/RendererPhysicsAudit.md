# Renderer and physics documentation reconstruction audit

Scope: integrated main `2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`.
This audit supports `renderer.mdx`, `physics.mdx`, the narrow
`render-extraction.mdx` correction, and the repaired query paragraph in
`PhysicsBackend.md`. It records documentation evidence, not a new performance
or platform qualification.

| Claim in site pages | Integrated evidence | Limit or classification |
| --- | --- | --- |
| Immutable publication and disposable render projection | `include/gargantuan/render/RenderPublication.hpp`, `src/render/RenderPublisher.cpp`, `src/render/RenderProjection.cpp`, `tests/RendererPublicationTests.cpp` | SDL resource application clears derived state and requires full resync on failure (`src/render/Renderer.cpp`); no general device-loss acceptance claimed. |
| SDL is the selected runtime backend | `include/gargantuan/render/SDLRenderer.hpp`, `src/render/Renderer.cpp`, `src/render/passes/`, `CMakeLists.txt`; `devdocs/CurrentArchitecture/RendererBackendBoundary.md` | Filament is optional experimental benchmark; no runtime backend switch or generic RHI. |
| Current material rendering is narrower than the neutral value | `RenderPublication.hpp`, `src/render/passes/OpaquePass.cpp`, `assets/shaders/opaque.frag`, `src/render/passes/ShadowPass.cpp` | Shader samples base color and handles masked alpha; normal texture and metallic/roughness are carried but unused for PBR; shadow pass lacks masked alpha sampling. |
| One sun, fixed shadow, Sky/fog/exposure, D32 scene depth | `src/render/passes/ShadowPass.cpp`, `src/render/passes/SkyPass.cpp`, `src/render/sdl/SDLSceneDepth.hpp`, `src/render/Renderer.cpp`; `devdocs/CurrentArchitecture/EnvironmentLightingFoundation1.md`, `devdocs/Validation/RendererDepthKI008.md` | Every visible `CastShadow` mesh draws an unmasked silhouette; no cascades, local lights, IBL, HDR output or complete color-transfer contract. Windowed swapchain format is queried from SDL; offscreen is fixed RGBA8 UNORM. |
| Box3D rigid and XPBD deformable worlds remain separate | `include/gargantuan/classes/WorldRoot.hpp`, `include/gargantuan/physics/PhysicsBackend.hpp`, `SoftBodyBackend.hpp`, `src/physics/Box3DPhysicsBackend.cpp`, `XpbdSoftBodyBackend.cpp`; `tests/PhysicsBackendTests.cpp`, `SoftBodyPhysicsTests.cpp` | Jolt is comparison material only; no Jolt runtime adapter. |
| Main safe-point sync, owned results, and bounded jobs | `src/classes/WorldRoot.cpp`, `include/gargantuan/physics/PhysicsTypes.hpp`, `SoftBodyTypes.hpp`; `devdocs/CurrentArchitecture/PhysicsBackend.md`, `SoftBodyPhysicsFoundation2.md`, `PhysicsFoundationSoftControl.md` | Soft collision is rigid-to-soft, discrete, and one-way; no soft/soft or soft query participation. |
| Shape casts and overlaps are integrated APIs | `include/gargantuan/physics/PhysicsQueryTypes.hpp`, `PhysicsBackend.hpp`, `src/classes/WorldRoot.cpp`, `src/physics/Box3DAdvancedQueries.inl`, `tests/PhysicsBackendTests.cpp`; `devdocs/CurrentArchitecture/PhysicsQueryFoundation1.md` | `PhysicsBackend.md` previously said these APIs were absent. Its query section is corrected here; exact platform qualification remains in its campaign receipts. |

## Branch and qualification boundary

The separately pinned prospective commit on
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

## Review disposition

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

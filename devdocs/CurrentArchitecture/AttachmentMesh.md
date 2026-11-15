# AttachmentMesh visual socket composition

`AttachmentMesh` extends `Attachment`. Its inherited authored `CFrame` and
`JointPath` identify a semantic socket. Saved and replicated `Mesh`, `Material`,
`Size`, `Color`, `Transparency`, `Visible`, and `CastShadow` describe one rigid
visual. It is not a `BasePart`: it owns no collision, touch, physics query,
movement, or animation authority. A Character may retain its existing nullable
`RootPart`; the accessory does not change Character ownership.

On Main, `SemanticSpatialResolver` resolves the full socket matrix, including
joint scale. `RenderPublisher` applies the owning Part's transient presentation
offset and the visual's local size, then copies an immutable `RenderItem`, mesh
identity, and material values into `RenderPublication`. The renderer-owned
`RenderProjection` uses the accessory's generation-safe `ObjectId` for picking.
Neither the render publication nor projection retains an `Instance` pointer.

The producer rejects an unavailable owner, socket, skeleton, rigid Mesh, or
explicit Material reference with a diagnostic and removes any previously
projected visual. A missing or incompatible joint path cannot place an
accessory at a static fallback origin. Runtime pose changes, static owner
`CFrame`/`Size`, ancestor changes, authored appearance changes, and canonical
asset reimport dirty the visual through bounded reverse dependencies. Pose and
presentation changes do not write the authored graph or history.
The publisher registers the bounded ancestor Attachment chain when indexing a
visual, so an editor resolver that discovers the visual lazily also observes
parent socket offsets and joint-path changes. AssetService protects visual
Mesh and explicit Material references from source-group deletion and generated
child retirement until the visual releases them.

Full resync discovers visual attachments with an iterative 131,072-object
scene limit, above the 65,536-object saved scene bound, and indexes at most
1,024 visuals. An overflow omits **all** visual attachments from the complete
replacement, sets `VisualAttachmentsComplete = false`, and emits a diagnostic.
Incremental frames use the reverse index; they do not traverse the whole scene.
The inherited Attachment depth limit is 64. Imported primitive material
dependencies use the existing 256-submesh asset limit.

The class uses the existing asset catalog, graph serialization, structural
replication, semantic resolver, render publication, and renderer backend. It
does not add a preset asset format, second character controller, or GPU path.
Focused source tests are in `tests/AttachmentMeshTests.cpp`; native
qualification is coordinated with the campaign integration branch.

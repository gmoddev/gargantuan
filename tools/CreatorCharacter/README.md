# ClockworkWisp public authoring harness

`AuthorProject.py` opens a **closed** Studio project in a separate, hidden
Engine EditorHost process. It uses public EditorHost RPCs to create and save a
playable, project-owned `KinematicCharacter` template. It never edits the
serialized instance file or imports an asset itself. Use the Engine binary
qualified with `CharacterComposition` and `AttachmentMesh` support.

Before running, import `samples/CreatorCharacter/ClockworkWisp.gltf` and
`samples/CreatorCharacter/ClockworkCharm.obj` through Studio into the target
project, save, then close Studio. The imported catalog must expose one ready
skinned Wisp Mesh, the `WispIdle`, `WispMove`, and `WispAction` clips on its
canonical skeleton, and one ready Charm Mesh. Source basenames may be changed
with `--wisp-source` and `--charm-source` if the imported files were renamed.

Run from the Engine repository with Python 3.10 or newer:

```powershell
python tools/CreatorCharacter/AuthorProject.py `
  --binary C:\Build\gargantuan.exe `
  --project C:\Projects\ClockworkWispGame `
  --receipt C:\Receipts\ClockworkWispAuthoring.json
```

The receipt must be a new file outside the project. The client checks the
handshake, exact schema versions, live project scope, unique catalog identities,
rig joint paths, clip compatibility, source readback, and Character composition.
It saves only after every check succeeds and requires a normal EditorHost exit
after standard-input EOF. An authoring check failure occurs before SaveProject;
an EOF, exit, response or receipt-write failure can occur after the save has
committed. Reopen the project in Studio to inspect its durable state before
retrying; a missing success receipt does not prove that nothing was saved. The receipt records
the saved revision, object IDs, asset references, and source hashes, but no
session token or script body. Run the harness once per fresh project; it refuses
name collisions rather than replacing existing authored objects.

The saved template is `ReplicatedStorage/PlayableCharacterTemplate`. Its
`SkinnedRoot` MeshPart has the Wisp Mesh, a direct `RigAnimator`, a `CrownSocket`
Attachment on `WispRoot/WispCrown`, and a visual-only `WispCharm`
AttachmentMesh on `WispRoot/WispCrown/WispAntenna`. The typed `RootPart` points
to `SkinnedRoot`. Workspace also contains an anchored collidable 40 × 1 × 40
`CreatorFloor` at y = −0.5 and an anchored 1 × 4 × 12 `CreatorWall` centered
at (4, 2, 0) for movement and collision acceptance. The two ModuleScripts
live in `ReplicatedStorage`; the four
context-specific Scripts live in `Workspace`. Their `InitialSource` is copied
from the six `docs/examples/Character*` Luau files. The client sets the three
animation reference attributes on `CharacterAnimationDriver`, the action
reference on both action Scripts, and the three saved Players default policy
switches to true.

The receipt verifies authoring state only. Native Studio reopening, source UI
commit, standalone/player runtime, visible animation, network action admission,
and post-reopen asset availability remain separate acceptance gates. Do not
run the harness while Studio also has the project open.

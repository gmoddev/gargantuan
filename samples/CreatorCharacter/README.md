# Clockwork Wisp creator fixture

`ClockworkWisp.gltf` is a compact, self-contained raw glTF import source for a visible nonhumanoid Character: a turquoise faceted body, coral fins, gold eyes, and an antenna. Its three-joint skinned Mesh has no external texture or buffer files. Importing the source should produce one Mesh, its three material records, and three Animation records. The importer assigns the project-specific `asset://` references; copy those from Studio's asset catalog instead of hardcoding IDs.

| Clip | Duration | Authored visual motion |
| --- | ---: | --- |
| `WispIdle` | 1 s | Gentle crown and antenna sway; loop. |
| `WispMove` | 1 s | Crown bob and fin movement; loop. |
| `WispAction` | 0.5 s | Crown lean and antenna stretch; one shot. |

All clips use the same canonical skeleton. Useful socket paths are `WispRoot`, `WispRoot/WispCrown`, and `WispRoot/WispCrown/WispAntenna`. `ClockworkCharm.obj` is a separate tiny rigid octahedral Mesh source. In the Engine creator feature branch, an `AttachmentMesh` under the Wisp's root `MeshPart` can use the Charm Mesh reference and `JointPath = "WispRoot/WispCrown/WispAntenna"`; it is visual only and does not create a second physics body. The base public `Attachment` can use the same path for semantic audio, prompts, or other consumers.

To make this the playable actor, follow [the Character template setup](../../docs/examples/README.md#play-as-an-authored-skinned-character): author a saved `KinematicCharacter` template in `ReplicatedStorage`, give it a root `MeshPart` using the imported Wisp Mesh, put `Animator` directly under that part, and set the template's typed `RootPart` reference to the part. Use the three imported clip references for the client animation policy's `IdleReference`, `MoveReference`, and `ActionReference` attributes. The server/client action scripts use the same `WispAction` reference and their own explicit `CreatorLunge` motion request. `WispAction` has no root translation keys, so visual playback does not add a second displacement on top of the native Character action path.

These are source assets only. Import, authored Instances, script Source, saved `Players` switches, and Play/Stop acceptance go through Studio's ordinary asset and source tools. No serialized project state is included here.

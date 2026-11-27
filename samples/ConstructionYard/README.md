# Construction Yard

Build a retaining wall alongside sculpted ground, excavate underneath it, and step into a workshop at the same coordinates. The yard uses one Workspace terrain field, a native BlockGrid for each construction and ordinary Parts for survey markers and a loose stone. Its world is editable in Studio.

With a Windows or Linux Engine built from the spatial construction candidate, run the following command from the repository root. Choose an unused destination directory.

```sh
python samples/ConstructionYard/BuildProject.py --engine /path/to/Engine --project /path/to/ConstructionYard
```

The builder uses EditorHost to place the grids and apply terrain brushes. It saves, reopens the project and checks cells in both domains. Open that project in Studio and press Play, or build a trusted Offline Player package.

| Key | Action during offline Play |
| --- | --- |
| B | Add one brick to the next course of the current construction. |
| E | Excavate the outdoor terrain just ahead of the character. |
| F | Move the local character and camera into the workshop or back to the yard after checking the destination floor. |

The amber and blue markers use the same local position. The wall and workshop floors live in different physics worlds. Place and remove cells in the Studio grid inspector, then save and reopen to keep authored changes. Play uses a separate world, so Stop restores the edit scene. The reference script exercises local transfer. Multiplayer construction, bilateral admission and journal replay require additional runtime support on this candidate.

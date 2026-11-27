# Terrain Foundation 1 sample

Run from the repository root using a build with the Terrain Foundation 1 runtime.

~~~powershell
.\build\gargantuan.exe --script=samples/TerrainFoundation1/Terrain.luau
~~~

The sample builds an editable plateau and rolling slopes, a floating island, and connected cave chambers. It moves the local Character onto the surface after collision is ready. The cave entrance is near (15, 4, 6), along the low-Z side of the plateau. The orange marker on top opens a second entrance through an excavation prompt.

All ground and caves use public Luau terrain edits. The marker is a normal Part. Terrain is generated at launch for an offline world and runtime changes last for that world.

For display-free initialization verification, add `--headless` and look for `[Terrain:Sample] Landscape collision is ready` in runtime output. The process continues running after scene initialization.

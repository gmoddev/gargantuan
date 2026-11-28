# Standalone Construction Yard

Build a project with the existing Construction Yard terrain, exterior wall, workshop, construction tools and persistence scripts. The application adds a title, world selection and pause controls.

```powershell
python samples/StandaloneConstructionYard/BuildProject.py --engine C:/path/to/gargantuan.exe --project C:/Games/YardProject
gargantuan-packager build --project C:/Games/YardProject --runtime C:/path/to/RuntimeDistribution --output C:/Games/Yard --configuration Release
C:/Games/Yard/GargantuanPlayer.exe
```

Enter a world name and seed, then choose **Create world**. **Next world** cycles through the saved slots. **Play selected world** opens the selected slot with its own construction history. Press **B** to build, **E** to dig and **F** to use the doorway. **Escape** pauses the world and opens the menu. **Resume** continues the current world. **Return to title** finishes its pending writes and releases the slot.

The application entry is a client Script in the Application folder. The builder designates it through the EditorHost property transaction. The regular Construction Yard builder remains available for direct-start games. [Construction Yard](../ConstructionYard/README.md)

# PreRun as an Explorer service

**Status** · Design record for Studio and Engine. The baseline is Windows Studio RC9, Studio `5a4aca95` and Engine `284443879`. The implemented Explorer service keeps ordering and sources in one atomic resource and uses a collection-wide save revision. The [current authoring contract](../CurrentArchitecture/PreRunAuthoring.md) describes the implemented format and editor workflow.

## What a creator sees

```text
Explorer
  ▸ Game
  ▾ PreRun                         [+]
      Default
      CustomScriptHere
      ItemDefinitions
```

Every open local project gets one **PreRun** row at the top level. Collapsing Game still leaves PreRun visible. A new project shows just **Default**. Choosing **+ → New PreRun Script** adds a sibling, focuses its name, and opens its Luau document. Double-clicking a child opens or focuses its existing tab. The Game menu and command palette can keep **Project PreRun** as shortcuts that reveal the same row.

| Interaction | Proposed behavior |
| --- | --- |
| Open Default | Edit the project's existing bootstrap source. Its name and first position are fixed. Its contents can be empty. |
| Open a custom script | Give it its own tab, draft, diagnostics and recovery state. Two open PreRun tabs can have independent edits. |
| Rename, move up or move down | Change a custom entry's display name or run position. Renaming preserves its tab, source and declared schema names. Display names are unique within the service. |
| Remove a custom script | Resolve any draft before removing it. Cancel keeps the entry and editor intact. Default remains available. |
| Select Validate PreRun | Check the whole ordered set including local drafts. An error identifies the entry and, when available, the line. Selecting it opens that document. |

Children run from top to bottom, with Default first. The project saves that order explicitly, so renaming a script leaves its position intact. Each entry has its own Luau locals. A declaration in one entry can reference a schema definition in another under the Engine's existing whole-schema resolution rules.

## What the service owns

**PreRun is an Engine-owned project authoring service.** The Engine needs these declarations before it can construct the gameplay DataModel. EditorHost exposes the collection and its source through scoped project commands. Explorer projects a service row and document rows, each with its own kind. Instance selection, deletion, reparenting and plugin object actions continue to receive real DataModel IDs. Creating a PreRun entry is an explicit authoring action. Moving an ordinary Script in Explorer cannot turn it into a bootstrap script.

The project owns persistent entry IDs, labels, order and committed source. Studio keys each buffer by open-project generation and entry ID. It owns drafts, tabs, expansion and focus. A source write checks the project identity and that entry's expected source revision. A structural edit checks the collection revision. Concurrent edits to different entries can succeed. A stale source write, deletion during editing, or late callback after switching projects preserves the local draft and cannot change a replacement entry. Team Create continues to enforce its existing capability gate until its authority supports these operations.

## Saving, validation and execution

Saving writes bounded source, including an unfinished draft that needs repair. Validation executes each entry as a separately named Luau chunk against **one disposable schema candidate**. Cross-entry collisions report both origins when known. A failed chunk discards the candidate. The registration capabilities and the budgets apply to the whole set, including the existing 256 KiB source limit, 16 MiB VM limit, 250 ms execution budget and registration limits. A proposed initial cap of 64 entries also bounds empty documents and ordering work.

Only explicit project reopen activates a successfully validated set. If activation fails, Studio reports the failing entry and retains the last working session. On cold open, EditorHost should offer a recovery session that can read, edit and validate the project sources before a world exists. Play and supported package builds compare the active fingerprint with a coherent snapshot of the saved ordered sources. Changing source, membership or order requires reopen. Labels and tree expansion are excluded from the fingerprint. Packaged startup consumes the same ordered entries and validator. Distributed Player and compiled/protected configurations keep their existing PreRun restrictions.

## Existing projects and storage

Opening an existing project maps `.gargantuan/prerun.luau` to **Default** byte for byte. A missing file gives an empty editable Default with `Exists=false`. Browsing Explorer writes nothing. Editing only Default keeps the existing single-file format and compatibility.

The proposed multi-entry format keeps Default at its current path. Custom sources live at `.gargantuan/prerun/<EntryId>.luau`. A versioned index records ordered IDs and display names. Renames never move a source file. On the first committed custom entry, stage the new source and complete index, then atomically switch the project metadata to the multi-entry format while preserving project identity. The project-storage owner serializes writers and rechecks revisions at commit. Create writes source before publishing the index. Delete publishes the index change before retiring the old source. Unlisted files never execute. A missing listed source is an error with its entry name.

Version the multi-entry project, GASP place and package representations so single-source readers reject them explicitly. Save As and supported export/import round-trip IDs, order, names, source bytes and the distinction between missing and empty Default. Old EditorHosts continue to expose the single Default document for legacy projects through their existing capabilities. New authoring operations require an advertised collection capability. An incompatible client opening a multi-entry project receives an unsupported-format response.

## Implementation and acceptance

1. **Explorer and Default.** Add a project-resource row kind in Studio's Explorer projection, bind it in the view/controller, and route opening Default to the existing PreRun document. Test disclosure, filtering, keyboard activation and project-switch cleanup with an unmodified single-source Engine.
2. **Named entries.** Add Engine collection storage, bounded EditorHost reads and revision-checked edits. Extend the existing PreRun buffer, wire and restoration modules to handle stable entry IDs. Enable the plus button only when the host supports the full path.
3. **Runtime parity.** Test deterministic registration, duplicate definitions across two files, an invalid later chunk, aggregate limits, dirty-buffer conflicts, reorder then reopen, and legacy Default migration. Test Save As, GASP and supported packaged startup with two entries. Verify an old client cannot open or overwrite the new format, unsupported distribution presets stay gated, and Team Create exposes only authorized operations.

In Studio, the relevant owners under `experiments/LuiStudio/App/Src` are `EditorShell.luau` for the Game command, `ExplorerProjection.luau` and `ExplorerController.luau` for tree routing, and `PreRunDocument.luau`, `PreRunBuffer.luau` and `PreRunWire.luau` for document editing. `ProjectTransition.luau` and `TeamCreate.luau` own the applicable transition and collaboration gates.

The existing [PreRun authoring contract](../CurrentArchitecture/PreRunAuthoring.md), [runtime schema lifecycle](../CurrentArchitecture/RuntimeSchema.md), [project persistence](../CurrentArchitecture/ProjectRevisionAndPersistence.md) and [bootstrap tests](../../tests/PreRunBootstrapTests.cpp) are the Engine implementation starting points.

---
title: DataModel lifecycle candidate
description: Candidate-only reference for DataModel.
---

Source `61f8520d782ea2810f168fa38d3b28027aff7a89`. Qualified unreleased lifecycle candidate.

Class that provides the root hierachy and services

[Lifecycle usage guide](../../guides/standalone-application.mdx)

## DataModel.ApplicationEntry

Project-local durable SourceId of the standalone client entry. Empty uses direct startup.

```json
{
  "Name": "ApplicationEntry",
  "Description": "Project-local durable SourceId of the standalone client entry. Empty uses direct startup.",
  "Type": "t.String",
  "Unmodified": "std::string()",
  "Serializable": true,
  "Replicated": false,
  "AuthoringOnly": true,
  "Category": "Application"
}
```

## DataModel.IsMainMenuVisible

Reads the current application's menu visibility.

```json
{
  "Name": "IsMainMenuVisible",
  "Description": "Reads the current application's menu visibility.",
  "AsLuau": true,
  "LuauSignature": "(self: DataModel) -> boolean"
}
```

## DataModel.SetGameplayInputEnabled

Controls local input delivery to the active gameplay session.

```json
{
  "Name": "SetGameplayInputEnabled",
  "Description": "Controls local input delivery to the active gameplay session.",
  "AsLuau": true,
  "LuauSignature": "(self: DataModel, Enabled: boolean) -> { Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }"
}
```

## DataModel.SetMainMenu

Registers the application's ScreenGui. Pass nil to clear the selection.

```json
{
  "Name": "SetMainMenu",
  "Description": "Registers the application's ScreenGui. Pass nil to clear the selection.",
  "AsLuau": true,
  "LuauSignature": "(self: DataModel, Menu: ScreenGui?) -> { Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }"
}
```

## DataModel.SetMainMenuVisible

Shows or hides the registered application menu.

```json
{
  "Name": "SetMainMenuVisible",
  "Description": "Shows or hides the registered application menu.",
  "AsLuau": true,
  "LuauSignature": "(self: DataModel, Visible: boolean) -> { Ok: true, Value: boolean } | { Ok: false, Code: string, Message: string }"
}
```


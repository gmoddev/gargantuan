---
title: WorldSaveService lifecycle candidate
description: Candidate-only reference for WorldSaveService.
---

Source `61f8520d782ea2810f168fa38d3b28027aff7a89`. Qualified unreleased lifecycle candidate.

Manages world slots in the local project profile.

[Lifecycle usage guide](../../guides/standalone-application.mdx)

## WorldSaveService.Create

Commits a world slot with an optional seed.

```json
{
  "Name": "Create",
  "AsLuau": true,
  "Description": "Commits a world slot with an optional seed.",
  "LuauSignature": "(self: WorldSaveService, Options: {Name: string, Seed: string?}) -> { Await: (self: any) -> ({ Ok: true, Value: { SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string } } | { Ok: false, Code: string, Message: string, RecoveryId: string? }), GetResult: (self: any) -> ({ Ok: true, Value: { SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string } } | { Ok: false, Code: string, Message: string, RecoveryId: string? })?, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## WorldSaveService.GetCurrentWorld

Reads the selected save in authoritative gameplay.

```json
{
  "Name": "GetCurrentWorld",
  "AsLuau": true,
  "Description": "Reads the selected save in authoritative gameplay.",
  "LuauSignature": "(self: WorldSaveService) -> { Ok: true, Value: { SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string } } | { Ok: false, Code: string, Message: string }"
}
```

## WorldSaveService.List

Lists a page of saved worlds.

```json
{
  "Name": "List",
  "AsLuau": true,
  "Description": "Lists a page of saved worlds.",
  "LuauSignature": "(self: WorldSaveService, Cursor: string?) -> { Await: (self: any) -> ({ Ok: true, Value: { Items: {{ SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string }}, NextCursor: string? } } | { Ok: false, Code: string, Message: string, RecoveryId: string? }), GetResult: (self: any) -> ({ Ok: true, Value: { Items: {{ SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string }}, NextCursor: string? } } | { Ok: false, Code: string, Message: string, RecoveryId: string? })?, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

## WorldSaveService.ResolveCreate

Reads a world slot after an uncertain create outcome.

```json
{
  "Name": "ResolveCreate",
  "AsLuau": true,
  "Description": "Reads a world slot after an uncertain create outcome.",
  "LuauSignature": "(self: WorldSaveService, RecoveryId: string) -> { Await: (self: any) -> ({ Ok: true, Value: { SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string } } | { Ok: false, Code: string, Message: string, RecoveryId: string? }), GetResult: (self: any) -> ({ Ok: true, Value: { SaveId: string, WorldId: string, Name: string, Seed: string?, Revision: string, Compatibility: string } } | { Ok: false, Code: string, Message: string, RecoveryId: string? })?, GetProgress: (self: any) -> {Phase: string}?, ProgressChanged: Signal, Cancel: (self: any) -> boolean, Completed: Signal }"
}
```

